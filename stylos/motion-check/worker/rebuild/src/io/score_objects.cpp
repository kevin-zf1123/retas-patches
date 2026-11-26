#include "retas/score_objects.hpp"
#include "score_page_objects.hpp"
#include <algorithm>

namespace retas {
namespace {
void require(bool condition,const char* message){if(!condition)throw FormatError(0,message);}
struct Budget {
    std::size_t used{};
    void add(std::size_t size){require(size<=score_objects_max_bytes-used,"Score object storage exceeds supported limit");used+=size;}
};
Bytes remainder(Reader& reader,Budget& budget){budget.add(reader.remaining());return reader.rest();}
bool supported(ScoreMotionKind kind){return kind==ScoreMotionKind::Arrow||kind==ScoreMotionKind::Text||kind==ScoreMotionKind::FadeIn||kind==ScoreMotionKind::FadeOut||kind==ScoreMotionKind::Overlap;}
std::uint16_t coordinate_width(const ScorePencilObject& pencil,ScoreObjectsWriteMode mode) {
    require(mode==ScoreObjectsWriteMode::PreserveWire||mode==ScoreObjectsWriteMode::NativeCanonical,"Invalid Score object write mode");
    require(pencil.coordinate_width==2||pencil.coordinate_width==4,"Invalid Score pencil coordinate width");
    if(mode==ScoreObjectsWriteMode::NativeCanonical)
        return std::all_of(pencil.points.begin(),pencil.points.end(),[](const auto& p){return p.x<=65535&&p.y<=65535;})?2:4;
    if(pencil.coordinate_width==2)require(std::all_of(pencil.points.begin(),pencil.points.end(),[](const auto& p){return p.x>=0&&p.y>=0&&p.x<=65535&&p.y<=65535;}),"Score width2 points cannot be represented exactly");
    return pencil.coordinate_width;
}
std::size_t measure(const ScoreObjects& objects,ScoreObjectsWriteMode mode) {
    require(mode==ScoreObjectsWriteMode::PreserveWire||mode==ScoreObjectsWriteMode::NativeCanonical,"Invalid Score object write mode");
    require(objects.motion.size()<=score_objects_max_entries&&objects.pencils.size()<=score_objects_max_entries,"Score object collection exceeds supported limit");
    Budget wire,model;wire.add(8);model.add(sizeof(ScoreObjects));model.add(objects.motion.size()*sizeof(ScoreMotionObject));model.add(objects.pencils.size()*sizeof(ScorePencilObject));
    for(const auto& object:objects.motion) {
        require(supported(object.kind()),"Unsupported Score motion kind");wire.add(46);wire.add(object.common.suffix.size());model.add(object.common.suffix.size());
        if(object.kind()==ScoreMotionKind::Arrow) {
            const auto* arrow=std::get_if<ScoreArrowAttributes>(&object.attributes);require(arrow,"Score Arrow requires Arrow attributes");wire.add(14);wire.add(arrow->suffix.size());model.add(arrow->suffix.size());
        }
        else if(object.kind()==ScoreMotionKind::Text) {
            const auto* text=std::get_if<ScoreTextAttributes>(&object.attributes);require(text,"Score Text requires Text attributes");
            require(text->text.size()<=65535&&std::find(text->text.begin(),text->text.end(),0)==text->text.end(),"Invalid Score Text wire string");
            wire.add(22);wire.add(text->text.size());wire.add(text->suffix.size());model.add(text->text.size());model.add(text->suffix.size());
        }
        else require(std::holds_alternative<std::monostate>(object.attributes),"Score transition cannot carry a subtype section");
    }
    std::size_t points{};
    for(const auto& pencil:objects.pencils) {
        require(pencil.points.size()<=score_objects_max_points-points,"Score pencil aggregate point count exceeds supported limit");points+=pencil.points.size();
        const auto width=coordinate_width(pencil,mode);wire.add(24);wire.add(pencil.points.size()*width*2);wire.add(pencil.points_suffix.size());wire.add(pencil.mode_suffix.size());
        model.add(pencil.points.size()*sizeof(ScorePencilPoint));model.add(pencil.points_suffix.size());model.add(pencil.mode_suffix.size());
    }
    wire.add(objects.suffix.size());model.add(objects.suffix.size());return wire.used;
}
ScoreMotionCommon common(Reader& reader,Budget& budget) {
    ScoreMotionCommon value;value.kind_flags=reader.u16();value.field6=reader.u16();value.field8=reader.i32();value.field_c=reader.i32();value.frame0=reader.i32();value.frame1=reader.i32();
    value.x0=reader.i32();value.y0=reader.i32();value.x1=reader.i32();value.y1=reader.i32();value.field28=reader.i32();value.suffix=remainder(reader,budget);return value;
}
void write_common(Writer& writer,const ScoreMotionCommon& value) {
    writer.u16(value.kind_flags);writer.u16(value.field6);writer.i32(value.field8);writer.i32(value.field_c);writer.i32(value.frame0);writer.i32(value.frame1);
    writer.i32(value.x0);writer.i32(value.y0);writer.i32(value.x1);writer.i32(value.y1);writer.i32(value.field28);writer.raw(value.suffix);
}
}
namespace {
ScoreObjects decode_objects(std::span<const std::uint8_t> payload,bool page_wire) {
    require(payload.size()<=score_objects_max_bytes,"Score object payload exceeds supported limit");Reader reader(payload);ScoreObjects objects;Budget model;model.add(sizeof(ScoreObjects));
    const auto count=reader.i32();reader.require_count(count,46,score_objects_max_entries);model.add(std::size_t(count)*sizeof(ScoreMotionObject));objects.motion.reserve(count);
    for(std::int32_t i=0;i<count;++i) {
        const auto tag=reader.u16();ScoreMotionObject object;const auto kind=ScoreMotionKind(tag&0xcfff);require(supported(kind),"Unsupported Score motion kind");
        auto base=reader.section();object.common=common(base,model);require(tag==object.common.kind_flags,"Score motion outer and inner tags differ");
        if(kind==ScoreMotionKind::Arrow) {auto section=reader.section();ScoreArrowAttributes arrow;arrow.field2c=section.u16();arrow.field30=section.i32();arrow.field34=section.u32();arrow.suffix=remainder(section,model);object.attributes=std::move(arrow);}
        else if(kind==ScoreMotionKind::Text) {auto section=reader.section();ScoreTextAttributes text;text.field38=section.u32();text.field3c=section.u32();text.field40=section.i32();text.field44=section.i32();const auto length=section.u16();model.add(length);text.text=section.bytes(length);require(page_wire||std::find(text.text.begin(),text.text.end(),0)==text.text.end(),"Invalid Score Text wire string");text.suffix=remainder(section,model);object.attributes=std::move(text);}
        objects.motion.push_back(std::move(object));
    }
    const auto pencils=reader.i32();reader.require_count(pencils,24,score_objects_max_entries);model.add(std::size_t(pencils)*sizeof(ScorePencilObject));objects.pencils.reserve(pencils);std::size_t total_points{};
    for(std::int32_t i=0;i<pencils;++i) {
        auto section=reader.section();const auto count_points=section.i32();ScorePencilObject pencil;pencil.stroke_width=section.i32();pencil.color=section.u32();pencil.coordinate_width=section.u16();
        require(pencil.coordinate_width==2||pencil.coordinate_width==4,"Invalid Score pencil coordinate width");section.require_count(count_points,std::size_t(pencil.coordinate_width)*2,score_objects_max_points);
        require(std::size_t(count_points)<=score_objects_max_points-total_points,"Score pencil aggregate point count exceeds supported limit");total_points+=std::size_t(count_points);model.add(std::size_t(count_points)*sizeof(ScorePencilPoint));pencil.points.reserve(count_points);
        for(std::int32_t point=0;point<count_points;++point){const auto x=pencil.coordinate_width==2?std::int32_t(section.u16()):section.i32();const auto y=pencil.coordinate_width==2?std::int32_t(section.u16()):section.i32();pencil.points.push_back({x,y});}
        pencil.points_suffix=remainder(section,model);auto mode=reader.section();pencil.mode=mode.u16();pencil.mode_suffix=remainder(mode,model);objects.pencils.push_back(std::move(pencil));
    }
    objects.suffix=remainder(reader,model);return objects;
}
}
ScoreObjects decode_score_objects(std::span<const std::uint8_t> payload){return decode_objects(payload,false);}
ScoreObjects score_page_detail::decode_page_objects(std::span<const std::uint8_t> payload){return decode_objects(payload,true);}
bool can_decode_score_objects(std::span<const std::uint8_t> payload) noexcept {try{(void)decode_score_objects(payload);return true;}catch(...){return false;}}
void validate_score_objects(const ScoreObjects& objects,ScoreObjectsWriteMode mode){(void)measure(objects,mode);}
Bytes encode_score_objects(const ScoreObjects& objects,ScoreObjectsWriteMode mode) {
    const auto size=measure(objects,mode);Writer writer;writer.data.reserve(size);writer.i32(std::int32_t(objects.motion.size()));
    for(const auto& object:objects.motion) {
        writer.u16(object.common.kind_flags);Writer base;write_common(base,object.common);writer.section(base.data);
        if(const auto* arrow=std::get_if<ScoreArrowAttributes>(&object.attributes)){Writer section;section.u16(arrow->field2c);section.i32(arrow->field30);section.u32(arrow->field34);section.raw(arrow->suffix);writer.section(section.data);}
        else if(const auto* text=std::get_if<ScoreTextAttributes>(&object.attributes)){Writer section;section.u32(text->field38);section.u32(text->field3c);section.i32(text->field40);section.i32(text->field44);section.string(text->text);section.raw(text->suffix);writer.section(section.data);}
    }
    writer.i32(std::int32_t(objects.pencils.size()));
    for(const auto& pencil:objects.pencils) {const auto width=coordinate_width(pencil,mode);Writer points;points.i32(std::int32_t(pencil.points.size()));points.i32(pencil.stroke_width);points.u32(pencil.color);points.u16(width);
        for(const auto& point:pencil.points){if(width==2){points.u16(std::uint16_t(point.x));points.u16(std::uint16_t(point.y));}else{points.i32(point.x);points.i32(point.y);}}
        points.raw(pencil.points_suffix);writer.section(points.data);Writer section;section.u16(pencil.mode);section.raw(pencil.mode_suffix);writer.section(section.data);
    }
    writer.raw(objects.suffix);return std::move(writer.data);
}
}
