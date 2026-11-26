#include "retas/mn6_archive.hpp"
#include <algorithm>

namespace retas {
namespace {
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("MN6 archive operation cancelled");}
struct Ledger {
    std::size_t records{},points{};std::stop_token stop;
    void count(std::size_t n,bool point=false){
        poll(stop);auto& used=point?points:records;const auto cap=point?mn6_max_points:mn6_max_records;
        if(used>cap||n>cap-used)throw std::length_error(point?"MN6 exceeds4Mi points":"MN6 exceeds100000 records");used+=n;
    }
};
void version(std::uint16_t value){if(value>0x1ff)throw FormatError(4,"Unsupported CT06 version");}
template<class T>void collection(Reader& r,std::vector<T>& out,std::int32_t n,std::size_t minimum,Ledger& ledger,bool points=false){
    r.require_count(n,minimum,points?mn6_max_points:mn6_max_records);ledger.count(std::size_t(n),points);out.reserve(std::size_t(n));
}
Mn6Length length(Reader& r){return {r.u64(),r.u32(),r.u64()};}
Mn6RootInfo info(Reader r){
    Mn6RootInfo out;auto scalars=r.section();out.kind=scalars.u16();out.text_04=scalars.string();out.text_10=scalars.string();out.text_1c=scalars.string();out.text_28=scalars.string();
    out.field_34=scalars.i32();out.field_38=scalars.i32();out.text_48=scalars.string();out.field_54=scalars.i32();out.scalar_suffix=scalars.rest();
    auto paper=r.section();auto name=paper.section();out.paper_name=name.string();out.paper_name_suffix=name.rest();auto base=paper.section();auto& b=out.base;
    b.lengths[0]=length(base);b.lengths[1]=length(base);b.frame_mode=base.i32();for(std::size_t i=2;i<14;++i)b.lengths[i]=length(base);
    b.scale_bits=base.u64();b.lengths[14]=length(base);b.lengths[15]=length(base);b.dpi_bits=base.u64();b.dialog_dpi_bits=base.u64();b.flags=base.i32();b.suffix=base.rest();
    out.paper_suffix=paper.rest();out.suffix=r.rest();return out;
}
Mn6CategoryChild child(Reader r){
    Mn6CategoryChild out;out.first_text=r.string();out.field_0c=r.i32();out.second_text=r.string();out.time=r.u32();out.suffix=r.rest();return out;
}
Mn6Category category(Reader r,Ledger& ledger){
    Mn6Category out;out.kind=r.i32();out.name=r.string();out.second_text=r.string();out.field_1c=r.i32();out.time=r.u32();const auto n=r.i32();collection(r,out.children,n,16,ledger);
    for(std::int32_t i=0;i<n;++i){poll(ledger.stop);out.children.push_back(child(r.section()));}out.suffix=r.rest();return out;
}
Mn6Model model(Reader r){
    Mn6Model out;out.field_00=r.i32();out.time=r.u32();out.text_54=r.string();out.text_60=r.string();out.text_6c=r.string();out.field_78=r.u32();out.field_7c=r.u32();out.field_80=r.u32();out.field_84=r.i32();out.image_path=r.string();out.relative_path_flag=r.i32();out.suffix=r.rest();return out;
}
Mn6Stroke stroke(Reader r,Ledger& ledger){
    Mn6Stroke out;const auto n=r.i32();out.width=r.i32();out.color=r.u32();out.coordinate_width=r.u16();collection(r,out.points,n,out.coordinate_width==2?4:8,ledger,true);
    for(std::int32_t i=0;i<n;++i){poll(ledger.stop);out.points.push_back(out.coordinate_width==2?Mn6Point{r.u16(),r.u16()}:Mn6Point{r.i32(),r.i32()});}out.suffix=r.rest();return out;
}
// A shared traversal first counts and validates the complete wire image, then
// emits directly into one reserved vector. Section lengths are patched in
// place, avoiding an allocation/copy for every nested section.
struct Count {
    std::size_t size{};Ledger ledger;
    void add(std::size_t n){poll(ledger.stop);if(size>mn6_max_bytes||n>mn6_max_bytes-size)throw std::length_error("MN6 exceeds64MiB");size+=n;}
    void u16(std::uint16_t){add(2);}void u32(std::uint32_t){add(4);}void i32(std::int32_t){add(4);}void u64(std::uint64_t){add(8);}
    void raw(const Bytes& bytes){add(bytes.size());}
    void string(const Bytes& bytes){if(bytes.size()>65535)throw std::length_error("MN6 string exceeds65535 bytes");add(2);raw(bytes);}
    template<class F>void section(F emit){add(4);emit();}
    void count(std::size_t n,bool points=false){ledger.count(n,points);i32(static_cast<std::int32_t>(n));}
};
struct Emit {
    Writer writer;std::stop_token stop;
    void u16(std::uint16_t v){writer.u16(v);}void u32(std::uint32_t v){writer.u32(v);}void i32(std::int32_t v){writer.i32(v);}void u64(std::uint64_t v){writer.u64(v);}
    void raw(const Bytes& bytes){poll(stop);writer.raw(bytes);}void string(const Bytes& bytes){poll(stop);writer.string(bytes);}
    template<class F>void section(F emit){poll(stop);const auto start=writer.data.size();writer.u32(0);emit();const auto n=writer.data.size()-start-4;for(unsigned i=0;i<4;++i)writer.data[start+i]=std::uint8_t(n>>((3-i)*8));}
    void count(std::size_t n,bool=false){poll(stop);i32(static_cast<std::int32_t>(n));}
};
template<class S>void length(S& s,const Mn6Length& v){s.u64(v.value_bits);s.u32(v.unit);s.u64(v.dpi_bits);}
template<class S>void archive(S& s,const Mn6Archive& a){
    s.u32(0x43543036);s.u16(a.version);const auto& info=a.info;
    s.section([&]{
        s.section([&]{s.u16(info.kind);s.string(info.text_04);s.string(info.text_10);s.string(info.text_1c);s.string(info.text_28);s.i32(info.field_34);s.i32(info.field_38);s.string(info.text_48);s.i32(info.field_54);s.raw(info.scalar_suffix);});
        s.section([&]{
            s.section([&]{s.string(info.paper_name);s.raw(info.paper_name_suffix);});const auto& b=info.base;
            s.section([&]{length(s,b.lengths[0]);length(s,b.lengths[1]);s.i32(b.frame_mode);for(std::size_t i=2;i<14;++i)length(s,b.lengths[i]);s.u64(b.scale_bits);length(s,b.lengths[14]);length(s,b.lengths[15]);s.u64(b.dpi_bits);s.u64(b.dialog_dpi_bits);s.i32(b.flags);s.raw(b.suffix);});
            s.raw(info.paper_suffix);
        });s.raw(info.suffix);
    });
    s.section([&]{for(const auto& group:a.categories){s.count(group.size());for(const auto& c:group)s.section([&]{
        s.i32(c.kind);s.string(c.name);s.string(c.second_text);s.i32(c.field_1c);s.u32(c.time);s.count(c.children.size());for(const auto& child:c.children)s.section([&]{s.string(child.first_text);s.i32(child.field_0c);s.string(child.second_text);s.u32(child.time);s.raw(child.suffix);});s.raw(c.suffix);
    });}s.raw(a.categories_suffix);});
    s.section([&]{s.count(a.models.size());s.i32(a.selected_model);for(const auto& m:a.models)s.section([&]{s.i32(m.field_00);s.u32(m.time);s.string(m.text_54);s.string(m.text_60);s.string(m.text_6c);s.u32(m.field_78);s.u32(m.field_7c);s.u32(m.field_80);s.i32(m.field_84);s.string(m.image_path);s.i32(m.relative_path_flag);s.raw(m.suffix);});s.raw(a.models_suffix);});
    s.section([&]{s.count(a.pencil_groups.size());for(const auto& g:a.pencil_groups){s.i32(g.canvas_width);s.i32(g.canvas_height);s.count(g.strokes.size());for(const auto& t:g.strokes)s.section([&]{
        s.count(t.points.size(),true);s.i32(t.width);s.u32(t.color);s.u16(t.coordinate_width);for(const auto& p:t.points){
            if(t.coordinate_width==2){if(p.x<0||p.y<0||p.x>65535||p.y>65535)throw std::invalid_argument("MN6 16-bit point is out of range");s.u16(std::uint16_t(p.x));s.u16(std::uint16_t(p.y));}else{s.i32(p.x);s.i32(p.y);}
        }s.raw(t.suffix);
    });}s.raw(a.pencil_groups_suffix);});s.raw(a.tail);
}
}
Mn6Archive decode_mn6_archive(std::span<const std::uint8_t> bytes,std::stop_token stop){
    poll(stop);if(bytes.size()>mn6_max_bytes)throw std::length_error("MN6 exceeds64MiB");Reader r(bytes);if(r.u32()!=0x43543036)throw FormatError(0,"Expected CT06 magic");Mn6Archive out;out.version=r.u16();version(out.version);out.info=info(r.section());Ledger ledger{0,0,stop};
    auto cats=r.section();for(auto& group:out.categories){const auto n=cats.i32();collection(cats,group,n,24,ledger);for(std::int32_t i=0;i<n;++i){poll(stop);group.push_back(category(cats.section(),ledger));}}out.categories_suffix=cats.rest();
    auto models=r.section();const auto n=models.i32();out.selected_model=models.i32();collection(models,out.models,n,40,ledger);for(std::int32_t i=0;i<n;++i){poll(stop);out.models.push_back(model(models.section()));}out.models_suffix=models.rest();
    auto pencils=r.section();const auto groups=pencils.i32();collection(pencils,out.pencil_groups,groups,12,ledger);for(std::int32_t i=0;i<groups;++i){
        Mn6PencilGroup group;group.canvas_width=pencils.i32();group.canvas_height=pencils.i32();const auto n=pencils.i32();collection(pencils,group.strokes,n,18,ledger);for(std::int32_t j=0;j<n;++j){poll(stop);group.strokes.push_back(stroke(pencils.section(),ledger));}out.pencil_groups.push_back(std::move(group));
    }out.pencil_groups_suffix=pencils.rest();out.tail=r.rest();poll(stop);return out;
}
Bytes encode_mn6_archive(const Mn6Archive& value,std::stop_token stop){
    poll(stop);version(value.version);Count count{0,{0,0,stop}};archive(count,value);Emit out{Writer{},stop};out.writer.data.reserve(count.size);archive(out,value);poll(stop);return std::move(out.writer.data);
}
Mn6Archive make_mn6_archive(std::uint16_t kind,const std::array<Bytes,9>& names){
    for(const auto& name:names)if(name.size()>65535)throw std::length_error("MN6 category label exceeds65535 bytes");Mn6Archive out;auto& i=out.info;i.kind=kind;i.text_10=i.text_1c=i.text_28=ascii("1");i.field_34=24;i.field_54=1;
    auto& b=i.base;const auto bits=[](double n){return std::bit_cast<std::uint64_t>(n);};const auto len=[&](double v,std::uint32_t unit,double dpi){return Mn6Length{bits(v),unit,bits(dpi)};};
    // Paper profile temporaries carry DPI0, but RCLength::operator= only
    // overwrites a positive source DPI. Every base member retains its144.
    b.lengths={len(720,0,144),len(540,0,144),len(0,0,144),len(0,0,144),len(22.57,1,144),len(16.93,1,144),len(2,1,144),len(1.5,1,144),len(2,1,144),len(1.5,1,144),len(2.2,1,144),len(1.65,1,144),len(2.2,1,144),len(1.65,1,144),len(720,0,144),len(540,0,144)};
    b.frame_mode=b.flags=1;b.scale_bits=bits(100);b.dpi_bits=b.dialog_dpi_bits=bits(144);
    for(std::size_t n=0;n<9;++n){Mn6Category c;c.kind=std::int32_t(n);c.name=names[n];out.categories[n/3].push_back(std::move(c));}out.pencil_groups.emplace_back();return out;
}
}
