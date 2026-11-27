#include "retas/document.hpp"
#include "retas/numeric.hpp"
#include "overlay_lengths.hpp"
#include <cmath>
#include <stdexcept>

namespace retas {
GuideState make_guide(double dpi){
    if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("参考线分辨率无效。");
    GuideState guide;guide.dpi=dpi;guide.position={Length{0,2,dpi},Length{0,2,dpi}};return guide;
}
int guide_coordinate(const GuideState& guide,const GuideRecord& record){
    return overlay_detail::pixels(record.position,guide.dpi);
}
std::vector<GuideSnapshot> selected_guides(const GuideState& guide){
    std::vector<GuideSnapshot> result;
    for(std::size_t index=0;index<guide.records.size();++index)
        if(guide.records[index].marker)result.push_back({index,guide.records[index]});
    return result;
}
void move_guide(GuideState& guide,int dx,int dy){
    using namespace overlay_detail;
    using numeric_detail::binary64;
    if(!std::isfinite(guide.dpi)||guide.dpi<=0)throw std::invalid_argument("参考线分辨率无效。");
    auto candidate=guide;
    candidate.origin={integer(std::int64_t(guide.origin[0])+dx),integer(std::int64_t(guide.origin[1])+dy)};
    const double x=millimeters({double(dx),0,guide.dpi}),y=millimeters({double(dy),0,guide.dpi});
    for(auto& record:candidate.records){
        const double value=binary64(millimeters(record.position)+(record.orientation==1?x:y));
        if(!std::isfinite(value))throw std::invalid_argument("参考线坐标超出支持范围。");
        record.position={value,2,guide.dpi};
    }
    // Unlike Grid movement, Guide movement leaves the common typed origins
    // unchanged. Only cached origin and each record's Length are translated.
    guide=std::move(candidate);
}
void move_guide_to(GuideState& guide,std::array<int,2> origin){
    const auto dx=overlay_detail::integer(std::int64_t(origin[0])-guide.origin[0]);
    const auto dy=overlay_detail::integer(std::int64_t(origin[1])-guide.origin[1]);
    move_guide(guide,dx,dy);
}
}
