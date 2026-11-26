#include "retas/tsf.hpp"
#include "retas/score.hpp"
#include "score_native_text.hpp"
#include <algorithm>
#include <bit>
#include <string>

namespace retas {
namespace {
void require(bool yes,const char* message){if(!yes)throw FormatError(0,message);}
struct Budget {
    std::size_t used{},limit;
    void add(std::size_t bytes){require(bytes<=limit-used,"TSF exceeds modern resource budget");used+=bytes;}
    void records(std::size_t count,std::size_t width){require(count<= (limit-used)/width,"TSF record budget overflow");add(count*width);}
};
template<std::size_t N> std::array<std::uint8_t,N> raw(Reader& r){
    std::array<std::uint8_t,N> out;auto bytes=r.take(N);std::copy(bytes.begin(),bytes.end(),out.begin());return out;
}
template<class T,std::size_t N> void integers(Reader& r,std::array<T,N>& out){
    for(auto& n:out)if constexpr(sizeof(T)==2)n=r.u16();else n=r.u32();
}
template<class T,std::size_t N> void integers(Writer& w,const std::array<T,N>& values){
    for(auto n:values)if constexpr(sizeof(T)==2)w.u16(n);else w.u32(n);
}
Bytes string(Reader& r,Budget& budget,std::size_t cap=255){const auto n=r.u16();require(n<=cap,"TSF string exceeds known writer profile");budget.add(n);return r.bytes(n);}
Bytes suffix(Reader& r,Budget& budget){budget.add(r.remaining());return r.rest();}
std::size_t string_size(const Bytes& bytes,Budget& decoded,std::size_t cap=255){
    require(bytes.size()<=cap,"TSF string exceeds known writer profile");decoded.add(bytes.size());return bytes.size()+2;
}
TsfTransform transform(Reader& r){
    TsfTransform out;
    for(auto& group:out.groups){for(auto& bits:group.bits)bits=r.u64();group.flags=raw<3>(r);}
    for(auto& bits:out.anchor_bits)bits=r.u64();out.anchor_flags=raw<2>(r);return out;
}
void transform(Writer& w,const TsfTransform& value){
    for(const auto& group:value.groups){for(auto bits:group.bits)w.u64(bits);w.raw(group.flags);}
    for(auto bits:value.anchor_bits)w.u64(bits);w.raw(value.anchor_flags);
}
void counts(const TsfMain& m){
    require(m.source_count<=tsf_max_sources,"TSF source count exceeds modern limit");
    require(m.page_count<=tsf_max_family_records&&m.item_count<=tsf_max_family_records&&m.extra_count<=tsf_max_family_records,"TSF family count exceeds modern limit");
}
void minimum_records(const TsfMain& m,Budget& wire){
    wire.records(m.source_count,108);
    wire.records(std::size_t(m.source_count)*m.page_count,129);
    wire.records(std::size_t(m.source_count)*m.extra_count,99);
}
void reserve_families(TsfData& s,Budget& decoded){
    decoded.records(s.main.page_count,sizeof(TsfPage));decoded.records(s.main.item_count,sizeof(TsfItem));decoded.records(s.main.extra_count,sizeof(TsfExtra));
    s.pages.reserve(s.main.page_count);s.items.reserve(s.main.item_count);s.extras.reserve(s.main.extra_count);
}
std::size_t validate(const TsfData& s){
    require(s.version==0x15e0,"Unsupported TSF version");counts(s.main);
    const auto frames=s.main.source_count;
    require(s.pages.size()==s.main.page_count&&s.items.size()==s.main.item_count&&s.extras.size()==s.main.extra_count,"TSF family count mismatch");
    Budget wire{18,tsf_max_file_bytes},decoded{sizeof(TsfData),tsf_max_decoded_bytes};
    decoded.records(s.pages.size(),sizeof(TsfPage));decoded.records(s.items.size(),sizeof(TsfItem));decoded.records(s.extras.size(),sizeof(TsfExtra));
    const auto add_suffix=[&](const Bytes& b){wire.add(b.size());decoded.add(b.size());};
    wire.add(4+44+94);for(const auto& text:s.main.strings)wire.add(string_size(text,decoded,63));wire.add(string_size(s.main.comment,decoded,65535));add_suffix(s.main.suffix);
    wire.add(4+30);add_suffix(s.fixed.suffix);
    wire.add(4+80+string_size(s.folder.folder,decoded)+string_size(s.folder.filename,decoded));add_suffix(s.folder.suffix);
    require(s.global.records.size()==frames,"TSF global source count mismatch");decoded.records(frames,sizeof(TsfSourceRecord));
    wire.add(4+22+string_size(s.global.name,decoded));wire.records(frames,108);add_suffix(s.global.suffix);
    for(const auto& page:s.pages){
        require(page.cells.size()==frames&&page.modes.size()==frames,"TSF page frame count mismatch");
        decoded.records(frames,sizeof(TsfCell)+sizeof(std::array<std::uint32_t,2>));
        wire.add(4+66+string_size(page.name,decoded));wire.records(frames,129);add_suffix(page.suffix);
    }
    for(const auto& item:s.items){
        require(item.children.size()<=tsf_max_children,"TSF child count exceeds modern limit");decoded.records(item.children.size(),sizeof(TsfChild));
        wire.add(4+100+string_size(item.name,decoded)+string_size(item.path,decoded));
        for(const auto& child:item.children)wire.add(24+string_size(child.name,decoded));add_suffix(item.suffix);
    }
    for(const auto& extra:s.extras){
        require(extra.records.size()==frames,"TSF extra frame count mismatch");decoded.records(frames,sizeof(TsfTransform));
        wire.add(4+4+string_size(extra.name,decoded));wire.records(frames,99);add_suffix(extra.suffix);
    }
    add_suffix(s.trailing);return wire.used;
}
TsfTransform default_transform(){
    TsfTransform t;for(auto& group:t.groups)group.flags.fill(1);t.groups[1].bits.fill(0x4059000000000000ull);t.anchor_flags.fill(1);return t;
}
Bytes layer_name(std::size_t index){
    if(!index)return ascii("-BG");--index;
    const auto prefix=std::uint8_t('@'+index/26);Bytes out;
    if(index/26&&prefix)out.push_back(prefix);out.push_back(std::uint8_t('A'+index%26));return out;
}
void ascii_profile(std::span<const std::uint8_t> text){
    for(auto c:text)require(c&&c<128&&c!=13,"TSF scaffold requires ASCII strings without NUL/CR");
}
Bytes number(const ScoreNumber& raw_number,bool strict=true){
    const auto ending=std::find(raw_number.begin()+4,raw_number.end(),0);
    require(ending!=raw_number.end(),"TSF scaffold number suffix requires NUL");
    const std::span<const std::uint8_t> tail(raw_number.begin()+4,ending);if(strict)ascii_profile(tail);
    auto text=ascii(std::to_string(Reader(raw_number).i32()));text.insert(text.end(),tail.begin(),tail.end());return text;
}
std::uint32_t exposure(std::span<const std::uint8_t> text,bool strict=true){
    std::size_t end=text.size();while(end&&!(text[end-1]>='0'&&text[end-1]<='9'))--end;
    if(!end)return 0;
    auto start=end;while(start&&text[start-1]>='0'&&text[start-1]<='9')--start;
    std::uint32_t value{};for(auto i=start;i<end;++i)value=std::min(6000u,value*10+(text[i]-'0'));
    if(!value)return 0;
    std::array<std::uint8_t,2> tail{};
    for(std::size_t i=end,out=0;i<text.size()&&out<2;){auto c=text[i++];
        if(!strict&&c>=0x81&&c<=0xfe&&i<text.size()&&text[i]>=0x40&&text[i]<=0xfe&&text[i]!=0x7f){++i;continue;}
        if(c>='a'&&c<='z')c=std::uint8_t(c-'a'+'A');
        if((c>='A'&&c<='Z')||c=='+'||c=='-')tail[out]=c;else if(strict)break;++out;
    }
    if(!tail[0])return value;return (value<<16)|(std::uint32_t(tail[1])<<8)|tail[0];
}
}
TsfData decode_tsf(std::span<const std::uint8_t> bytes){
    require(bytes.size()<=tsf_max_file_bytes,"TSF file exceeds modern limit");Reader r(bytes);TsfData s;
    s.version=r.u16();require(s.version==0x15e0,"Unsupported TSF version");integers(r,s.prologue);
    Budget decoded{sizeof(TsfData),tsf_max_decoded_bytes};auto main=r.section();auto& m=s.main;
    m.source_count=main.u32();m.page_count=main.u32();m.extra_count=main.u32();m.item_count=main.u32();counts(m);
    m.width=main.u32();m.height=main.u32();m.dpi=main.u32();m.flag0=main.u16();m.fps=main.u32();m.flag1=main.u16();m.step=main.u32();m.guide=main.u32();
    for(auto& text:m.strings)text=string(main,decoded,63);m.comment=string(main,decoded,65535);m.tail=raw<94>(main);m.suffix=suffix(main,decoded);
    require(std::size_t(3)+m.page_count+m.item_count+m.extra_count<=r.remaining()/4,"Truncated TSF segment count");
    Budget minimum{0,bytes.size()};minimum_records(m,minimum);reserve_families(s,decoded);
    auto control=r.section();integers(control,s.fixed.words0);integers(control,s.fixed.shorts);integers(control,s.fixed.words1);s.fixed.suffix=suffix(control,decoded);
    auto folder=r.section();s.folder.folder=string(folder,decoded);s.folder.filename=string(folder,decoded);require(folder.u32()==72,"Unsupported TSF folder metadata width");
    s.folder.metadata=raw<72>(folder);s.folder.tail=folder.u32();s.folder.suffix=suffix(folder,decoded);
    auto global=r.section();auto& g=s.global;g.name=string(global,decoded);integers(global,g.words0);g.short0=global.u16();integers(global,g.words1);
    const auto frames=m.source_count;require(frames<=global.remaining()/108,"Truncated TSF global records");decoded.records(frames,sizeof(TsfSourceRecord));g.records.reserve(frames);
    for(std::uint32_t i=0;i<frames;++i){TsfSourceRecord source;source.transform=transform(global);source.final_bits=global.u64();source.final_flag=global.u8();g.records.push_back(source);}g.suffix=suffix(global,decoded);
    for(std::uint32_t i=0;i<m.page_count;++i){auto section=r.section();TsfPage page;page.name=string(section,decoded);integers(section,page.words0);integers(section,page.shorts);integers(section,page.words1);
        require(section.remaining()>=4&&frames<=(section.remaining()-4)/129,"Truncated TSF page records");decoded.records(frames,sizeof(TsfCell)+sizeof(std::array<std::uint32_t,2>));page.cells.reserve(frames);page.modes.reserve(frames);
        for(std::uint32_t j=0;j<frames;++j){TsfCell cell;cell.exposure=section.u32();cell.transform=transform(section);cell.percent_bits=section.u64();cell.percent_flag=section.u8();cell.final_bits=section.u64();cell.final_flag=section.u8();page.cells.push_back(cell);}
        for(std::uint32_t j=0;j<frames;++j)page.modes.push_back({section.u32(),section.u32()});page.tail=section.u32();page.suffix=suffix(section,decoded);s.pages.push_back(std::move(page));
    }
    for(std::uint32_t i=0;i<m.item_count;++i){auto section=r.section();TsfItem item;item.name=string(section,decoded);item.path=string(section,decoded);require(section.u32()==72,"Unsupported TSF item metadata width");item.metadata=raw<72>(section);
        const auto n=section.u32();integers(section,item.words);require(n<=tsf_max_children&&section.remaining()>=12&&n<=(section.remaining()-12)/26,"Invalid TSF child count");decoded.records(n,sizeof(TsfChild));item.children.reserve(n);
        for(std::uint32_t j=0;j<n;++j){TsfChild child;child.name=string(section,decoded);integers(section,child.words0);child.bits=section.u64();integers(section,child.words1);item.children.push_back(std::move(child));}
        integers(section,item.tail);item.suffix=suffix(section,decoded);s.items.push_back(std::move(item));
    }
    for(std::uint32_t i=0;i<m.extra_count;++i){auto section=r.section();TsfExtra extra;extra.name=string(section,decoded);extra.color=section.u32();require(frames<=section.remaining()/99,"Truncated TSF extra records");decoded.records(frames,sizeof(TsfTransform));extra.records.reserve(frames);
        for(std::uint32_t j=0;j<frames;++j)extra.records.push_back(transform(section));extra.suffix=suffix(section,decoded);s.extras.push_back(std::move(extra));
    }
    s.trailing=suffix(r,decoded);(void)validate(s);return s;
}
Bytes encode_tsf(const TsfData& s){
    const auto size=validate(s);Writer w;w.data.reserve(size);w.u16(s.version);integers(w,s.prologue);
    const auto emit=[&](const auto& serialize){Writer section;serialize(section);w.section(section.data);};
    emit([&](Writer& p){const auto& m=s.main;p.u32(m.source_count);p.u32(m.page_count);p.u32(m.extra_count);p.u32(m.item_count);p.u32(m.width);p.u32(m.height);p.u32(m.dpi);p.u16(m.flag0);p.u32(m.fps);p.u16(m.flag1);p.u32(m.step);p.u32(m.guide);for(const auto& text:m.strings)p.string(text);p.string(m.comment);p.raw(m.tail);p.raw(m.suffix);});
    emit([&](Writer& p){integers(p,s.fixed.words0);integers(p,s.fixed.shorts);integers(p,s.fixed.words1);p.raw(s.fixed.suffix);});
    emit([&](Writer& p){p.string(s.folder.folder);p.string(s.folder.filename);p.u32(72);p.raw(s.folder.metadata);p.u32(s.folder.tail);p.raw(s.folder.suffix);});
    emit([&](Writer& p){const auto& g=s.global;p.string(g.name);integers(p,g.words0);p.u16(g.short0);integers(p,g.words1);for(const auto& record:g.records){transform(p,record.transform);p.u64(record.final_bits);p.u8(record.final_flag);}p.raw(g.suffix);});
    for(const auto& page:s.pages)emit([&](Writer& p){p.string(page.name);integers(p,page.words0);integers(p,page.shorts);integers(p,page.words1);for(const auto& cell:page.cells){p.u32(cell.exposure);transform(p,cell.transform);p.u64(cell.percent_bits);p.u8(cell.percent_flag);p.u64(cell.final_bits);p.u8(cell.final_flag);}for(const auto& pair:page.modes)integers(p,pair);p.u32(page.tail);p.raw(page.suffix);});
    for(const auto& item:s.items)emit([&](Writer& p){p.string(item.name);p.string(item.path);p.u32(72);p.raw(item.metadata);p.u32(std::uint32_t(item.children.size()));integers(p,item.words);for(const auto& child:item.children){p.string(child.name);integers(p,child.words0);p.u64(child.bits);integers(p,child.words1);}integers(p,item.tail);p.raw(item.suffix);});
    for(const auto& extra:s.extras)emit([&](Writer& p){p.string(extra.name);p.u32(extra.color);for(const auto& record:extra.records)transform(p,record);p.raw(extra.suffix);});
    w.raw(s.trailing);return std::move(w.data);
}
static TsfData project_score(const ScoreData& score,bool strict,const TsfSwfSidecarContext* context=nullptr){
    const auto& g=score.general;require(g.frame_count>=4&&g.frame_count<=score_max_frames&&g.layer_count>=1&&g.layer_count<=score_max_layers&&g.fps>0,"Invalid Score TSF scaffold dimensions/FPS");
    const auto frames=std::uint32_t(g.frame_count),layers=std::uint32_t(g.layer_count);
    require(score.derived_layers.size()==layers,"TSF scaffold Derived layer count mismatch");if(strict)ascii_profile(score.direction);require(score.direction.size()<=32767,"TSF scaffold direction exceeds native safe signed-size profile");
    std::vector<Bytes> groups;Bytes title,episode;
    if(context){require(context->width&&context->height&&context->dpi&&context->width<=INT32_MAX&&context->height<=INT32_MAX&&context->dpi<=INT32_MAX,"Invalid associated SWF TSF canvas");require(context->groups.size()<=100000,"Too many associated TSF groups");
        const auto text=[](const Bytes& raw,std::size_t cap){const auto end=std::find(raw.begin(),raw.end(),0);require(std::size_t(end-raw.begin())<=cap,"Associated TSF string exceeds native profile");return Bytes(raw.begin(),end);};title=text(context->title,63);episode=text(context->episode,63);
        for(const auto& raw:context->groups){auto name=text(raw,255);if(name!=ascii("-LO")&&name!=ascii("-BG")){require(groups.size()<tsf_max_family_records-1,"Too many associated TSF items");groups.push_back(std::move(name));}}
    }
    TsfData s;s.main.source_count=frames;s.main.page_count=layers+1;s.main.extra_count=layers+2;s.main.item_count=std::max(layers+1,std::uint32_t(groups.size()+1));
    s.prologue={0,0,std::min(layers+2,4u),std::min(frames,24u)};
    s.main.width=context?context->width:720;s.main.height=context?context->height:540;s.main.dpi=context?context->dpi:72;s.main.strings[0]=std::move(title);s.main.strings[1]=std::move(episode);s.main.flag0=1;s.main.fps=g.fps;s.main.flag1=3;s.main.step=144;s.main.guide=g.guide_line;
    s.main.strings[2]=number(g.scene,strict);s.main.strings[3]=number(g.cut,strict);s.main.comment=score.direction;s.main.tail[35]=10;std::fill(s.main.tail.begin()+36,s.main.tail.begin()+76,0xff);
    s.fixed.words0={0,0};s.fixed.shorts={4,3,1};s.fixed.words1={0,1,1,0xffffff};s.global.name=ascii("CAM");s.global.words0={0,1};s.global.short0=1;s.global.words1={0,0,0xffffffff};
    Budget minimum{0,tsf_max_file_bytes};minimum_records(s.main,minimum);
    for(const auto& row:score.derived_layers){require(row.cells.size()==frames&&row.name.size()<=255,"TSF scaffold Derived row width/name mismatch");if(strict)ascii_profile(row.name);
        for(const auto& cell:row.cells)if(cell.marker&1){const auto end=std::find(cell.label.begin(),cell.label.end(),0);if(strict)ascii_profile({cell.label.begin(),end});}
    }
    // Account the complete projected file/model before allocating frame banks.
    minimum.add(18+4*(4+s.main.page_count+s.main.item_count+s.main.extra_count)+160+30+84+27);
    Budget decoded{sizeof(TsfData),tsf_max_decoded_bytes};decoded.records(frames,sizeof(TsfSourceRecord));
    decoded.records(s.main.page_count,sizeof(TsfPage));decoded.records(std::size_t(frames)*s.main.page_count,sizeof(TsfCell)+sizeof(std::array<std::uint32_t,2>));
    decoded.records(s.main.item_count,sizeof(TsfItem));decoded.records(s.main.extra_count,sizeof(TsfExtra));decoded.records(std::size_t(frames)*s.main.extra_count,sizeof(TsfTransform));
    std::size_t strings=s.main.strings[0].size()+s.main.strings[1].size()+s.main.strings[2].size()+s.main.strings[3].size()+s.main.comment.size()+3;
    for(std::uint32_t i=0;i<=layers;++i){const auto page_size=i?score.derived_layers[i-1].name.size():3;minimum.add(68+page_size);strings+=page_size;}
    for(std::uint32_t i=0;i<s.main.item_count;++i){const auto item_size=i==1&&!groups.empty()?groups.back().size():layer_name(i).size();minimum.add(104+item_size);strings+=item_size;}
    // All output groups are empty: native overwrites item1.name for each
    // group without advancing its ordinal. Account temporary names as well.
    for(const auto& name:groups)decoded.add(name.size());
    for(std::uint32_t i=0;i<layers+2;++i){const auto name_size=i<2?3:1+std::max(std::size_t(2),std::to_string(i-1).size());minimum.add(6+name_size);strings+=name_size;}
    minimum.add(s.main.strings[0].size()+s.main.strings[1].size()+s.main.strings[2].size()+s.main.strings[3].size()+s.main.comment.size());decoded.add(strings);
    const auto defaults=default_transform();s.global.records.assign(frames,{defaults,0,1});
    for(std::uint32_t i=0;i<=layers;++i){TsfPage page;page.name=i?score.derived_layers[i-1].name:layer_name(0);page.words0={i+1,i,1,1,0,0,0};page.shorts={1,std::uint16_t(i?1:4),0x33};page.words1={0,0xffffffff,0,0xffffffff,0,0,0xffffffff};
        page.cells.assign(frames,{0,defaults,0x4059000000000000ull,0,1,1});page.modes.resize(frames);
        if(i){std::span<const std::uint8_t> held;for(std::uint32_t f=0;f<frames;++f){const auto& cell=score.derived_layers[i-1].cells[f];
            if(cell.marker&1){const auto end=std::find(cell.label.begin(),cell.label.end(),0);held={cell.label.begin(),end};}else if(cell.marker&8)held={};page.cells[f].exposure=exposure(held,strict);}}
        s.pages.push_back(std::move(page));
    }
    for(std::uint32_t i=0;i<s.main.item_count;++i){TsfItem item;item.name=layer_name(i);s.items.push_back(std::move(item));}for(const auto& name:groups)s.items[1].name=name;
    constexpr std::array<std::uint32_t,8> colors{0xffcc33,0xff00,0xff0000,0x99ff99,0xffff,0xff00ff,0x3399ff,0xcc99ff};
    for(std::uint32_t i=0;i<layers+2;++i){TsfExtra extra;extra.name=i==0?ascii("CAM"):i==1?ascii("-BG"):ascii("#"+(i<11?std::string("0"):std::string{})+std::to_string(i-1));extra.color=i?colors[(i-1)&7]:0xff;
        auto t=defaults;if(!i){t.groups[0].bits={std::bit_cast<std::uint64_t>(double(s.main.width>>1)),std::bit_cast<std::uint64_t>(double(s.main.height>>1)),std::bit_cast<std::uint64_t>(500.0)};t.anchor_bits={t.groups[0].bits[0],t.groups[0].bits[1]};}extra.records.assign(frames,t);s.extras.push_back(std::move(extra));
    }
    (void)validate(s);return s;
}
TsfData make_tsf_score_scaffold(const ScoreData& score){return project_score(score,true);}
TsfData make_tsf_score(const ScoreData& score){
    validate_score(score);auto live=score;
    live.direction=score_native_detail::runtime_string(score.direction);for(auto& row:live.derived_layers)row.name=score_native_detail::runtime_string(row.name);
    return project_score(live,false);
}

TsfData make_tsf_swf_sidecar(const ScoreData& score,const TsfSwfSidecarContext& context){
    validate_score(score);ScoreData live;live.general=score.general;live.direction=score_native_detail::runtime_string(score.direction);live.derived_layers.reserve(score.derived_layers.size());
    for(const auto& row:score.derived_layers){ScoreLayer target;target.name=score_native_detail::runtime_string(row.name);target.cells.reserve(row.cells.size());for(const auto& cell:row.cells)target.cells.push_back({cell.marker,cell.label,{}});live.derived_layers.push_back(std::move(target));}
    return project_score(live,false,&context);
}

}
