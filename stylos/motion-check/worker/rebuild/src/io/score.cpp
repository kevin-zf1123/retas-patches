#include "retas/score.hpp"
#include <algorithm>
#include <limits>

namespace retas {
namespace {
constexpr std::uint32_t magic=0x57425343;
void require(bool condition,const char* message) {if(!condition)throw FormatError(0,message);}
template<std::size_t N> std::array<std::uint8_t,N> fixed(Reader& r) {
    std::array<std::uint8_t,N> result{};auto bytes=r.take(N);std::copy(bytes.begin(),bytes.end(),result.begin());return result;
}
void dimensions(const ScoreGeneral& g) {
    require(g.frame_count>=4&&g.frame_count<=score_max_frames,"Score frame count outside 4..6000");
    require(g.layer_count>=1&&g.layer_count<=score_max_layers,"Score layer count outside 1..100");
    require(g.current_frame>=0&&g.current_frame<g.frame_count&&g.current_layer>=0&&g.current_layer<g.layer_count,"Invalid Score current cell");
    require(g.fps>0,"Invalid Score frame rate");
    require(g.settings.size()<=65535&&g.visibility.size()<=65535,"Score settings count overflow");
}
ScoreGeneral general(Reader r) {
    ScoreGeneral g;const auto visibility_count=r.u16();g.frame_count=r.i32();g.layer_count=r.i32();g.fps=r.u16();
    g.scene=fixed<8>(r);g.cut=fixed<8>(r);g.current_frame=r.i32();g.current_layer=r.i32();
    g.frame_display=r.u16();g.guide_line=r.u16();g.page=r.u16();const auto settings_count=r.u16();
    require(settings_count<=r.remaining()/2,"Truncated Score settings");
    for(unsigned i=0;i<settings_count;++i)g.settings.push_back(r.u16());
    require(visibility_count<=r.remaining()/4,"Truncated Score visibility");
    for(unsigned i=0;i<visibility_count;++i)g.visibility.push_back(r.i32());
    g.suffix=r.rest();dimensions(g);return g;
}
std::vector<ScoreLayer> layers(Reader& r,const ScoreGeneral& g) {
    std::vector<ScoreLayer> result;
    require(std::size_t(g.layer_count)<=r.remaining()/14,"Truncated Score layer table");
    result.reserve(g.layer_count);
    for(int i=0;i<g.layer_count;++i) {
        auto s=r.section();ScoreLayer layer;layer.name=s.string();layer.field=s.i32();layer.cell_stride=s.u32();
        require(layer.cell_stride>=10&&layer.cell_stride<=s.remaining()/std::size_t(g.frame_count),"Invalid Score cell stride");
        layer.cells.reserve(g.frame_count);
        for(int frame=0;frame<g.frame_count;++frame) {
            ScoreCell cell;cell.marker=s.u16();cell.label=fixed<8>(s);cell.suffix=s.bytes(layer.cell_stride-10);layer.cells.push_back(std::move(cell));
        }
        layer.suffix=s.rest();result.push_back(std::move(layer));
    }
    return result;
}
void validate_objects(std::span<const std::uint8_t> bytes) {
    Reader r(bytes);const auto count=r.i32();r.require_count(count,6,100000);
    for(int i=0;i<count;++i) {
        const auto kind=r.u16()&0xcfff;
        require(kind==1||kind==2||kind==4||kind==8||kind==16,"Unsupported typed Score object framing");
        auto base=r.section();base.take(40);
        if(kind==1){auto arrow=r.section();arrow.take(10);}
        else if(kind==2){auto text=r.section();text.take(16);text.string();}
    }
    const auto pencils=r.i32();r.require_count(pencils,24,100000);
    for(int i=0;i<pencils;++i) {
        auto points=r.section();const auto n=points.i32();points.i32();points.u32();const auto width=points.u16();
        require(width==2||width==4,"Invalid Score pencil coordinate width");
        points.require_count(n,std::size_t(width)*2,1000000);points.take(std::size_t(n)*width*2);
        auto mode=r.section();mode.u16();
    }
    // Native EndSection permits extensions. They remain part of opaque FF07.
}
Bytes encode_general(const ScoreGeneral& g) {
    Writer w;w.u16(std::uint16_t(g.visibility.size()));w.i32(g.frame_count);w.i32(g.layer_count);w.u16(g.fps);
    w.raw(g.scene);w.raw(g.cut);w.i32(g.current_frame);w.i32(g.current_layer);w.u16(g.frame_display);w.u16(g.guide_line);w.u16(g.page);
    w.u16(std::uint16_t(g.settings.size()));for(auto value:g.settings)w.u16(value);for(auto value:g.visibility)w.i32(value);w.raw(g.suffix);return std::move(w.data);
}
Bytes encode_layers(const std::vector<ScoreLayer>& layers,const Bytes& suffix) {
    Writer w;
    for(const auto& layer:layers) {
        Writer s;s.string(layer.name);s.i32(layer.field);s.u32(layer.cell_stride);
        for(const auto& cell:layer.cells) {s.u16(cell.marker);s.raw(cell.label);s.raw(cell.suffix);}
        s.raw(layer.suffix);w.section(s.data);
    }
    w.raw(suffix);return std::move(w.data);
}
}
void validate_score(const ScoreData& s) {
    require(s.version==0x100,"Unsupported Score version");dimensions(s.general);
    require(s.section_order[0]==0xff01,"Score general section must precede dependent sections");
    std::array<bool,7> seen{};for(auto id:s.section_order){require(id>=0xff01&&id<=0xff07,"Unknown Score section");require(!seen[id-0xff01],"Duplicate Score section");seen[id-0xff01]=true;}
    require(s.direction.size()<=65535&&s.sound_reference.size()<=65535,"Score string overflow");
    require(s.direction_mode.has_value()||s.direction_suffix.empty(),"Direction suffix requires mode");
    std::size_t total=8+7*6+44+s.general.settings.size()*2+s.general.visibility.size()*4;
    auto account=[&](std::size_t n){require(n<=score_max_file_bytes-total,"Score file exceeds supported limit");total+=n;};
    account(s.general.suffix.size());account(2+s.direction.size()+(s.direction_mode?4:0));account(s.direction_suffix.size());
    for(const auto* bank:{&s.primary_layers,&s.derived_layers}) {
        require(bank->size()==std::size_t(s.general.layer_count),"Score layer count mismatch");
        for(const auto& layer:*bank) {
            require(layer.name.size()<=65535,"Score layer name overflow");require(layer.cell_stride>=10,"Invalid Score cell stride");
            require(layer.cells.size()==std::size_t(s.general.frame_count),"Score frame count mismatch");
            account(14+layer.name.size());
            for(const auto& cell:layer.cells){require(cell.suffix.size()==layer.cell_stride-10,"Score cell suffix size mismatch");account(layer.cell_stride);}
            account(layer.suffix.size());
        }
    }
    account(s.primary_suffix.size());account(s.derived_suffix.size());
    require(s.voice_stride>=6&&s.voice.size()==std::size_t(s.general.frame_count),"Invalid Score voice geometry");account(4);
    for(const auto& record:s.voice){require(record.size()==s.voice_stride,"Score voice stride mismatch");account(record.size());}
    account(s.voice_suffix.size());account(2+s.sound_reference.size());account(s.sound_suffix.size());account(s.objects.size());account(s.trailing.size());
    validate_objects(s.objects);
}
ScoreData decode_score(std::span<const std::uint8_t> bytes) {
    require(bytes.size()<=score_max_file_bytes,"Score file exceeds supported limit");
    Reader r(bytes);require(r.u32()==magic,"Invalid Score magic");ScoreData s;s.version=r.u16();require(s.version==0x100,"Unsupported Score version");
    require(r.u16()==7,"Score requires seven sections");std::array<Bytes,7> payloads;std::array<bool,7> seen{};
    for(auto& id:s.section_order) {
        id=r.u16();require(id>=0xff01&&id<=0xff07,"Unknown Score section");require(!seen[id-0xff01],"Duplicate Score section");seen[id-0xff01]=true;
        auto section=r.section();payloads[id-0xff01]=section.rest();
    }
    require(s.section_order[0]==0xff01,"Score general section must precede dependent sections");s.trailing=r.rest();s.general=general(Reader(payloads[0]));
    Reader direction(payloads[1]);s.direction=direction.string();if(direction.remaining()){s.direction_mode=direction.i32();s.direction_suffix=direction.rest();}
    Reader primary(payloads[2]),derived(payloads[3]);s.primary_layers=layers(primary,s.general);s.primary_suffix=primary.rest();s.derived_layers=layers(derived,s.general);s.derived_suffix=derived.rest();
    Reader voice(payloads[4]);s.voice_stride=voice.u32();require(s.voice_stride>=6&&s.voice_stride<=voice.remaining()/std::size_t(s.general.frame_count),"Invalid Score voice stride");
    for(int i=0;i<s.general.frame_count;++i)s.voice.push_back(voice.bytes(s.voice_stride));s.voice_suffix=voice.rest();
    Reader sound(payloads[5]);s.sound_reference=sound.string();s.sound_suffix=sound.rest();s.objects=std::move(payloads[6]);validate_score(s);return s;
}
Bytes encode_score(const ScoreData& s) {
    validate_score(s);std::array<Bytes,7> payloads;payloads[0]=encode_general(s.general);
    Writer direction;direction.string(s.direction);if(s.direction_mode)direction.i32(*s.direction_mode);direction.raw(s.direction_suffix);payloads[1]=std::move(direction.data);
    payloads[2]=encode_layers(s.primary_layers,s.primary_suffix);payloads[3]=encode_layers(s.derived_layers,s.derived_suffix);
    Writer voice;voice.u32(s.voice_stride);for(const auto& record:s.voice)voice.raw(record);voice.raw(s.voice_suffix);payloads[4]=std::move(voice.data);
    Writer sound;sound.string(s.sound_reference);sound.raw(s.sound_suffix);payloads[5]=std::move(sound.data);payloads[6]=s.objects;
    Writer w;w.u32(magic);w.u16(s.version);w.u16(7);for(auto id:s.section_order){w.u16(id);w.section(payloads[id-0xff01]);}w.raw(s.trailing);return std::move(w.data);
}
}
