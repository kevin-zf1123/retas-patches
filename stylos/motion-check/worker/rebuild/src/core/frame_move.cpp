#include "retas/frame_move.hpp"
#include "retas/document.hpp"
#include "retas/fill.hpp"
#include "retas/numeric.hpp"
#include "overlay_lengths.hpp"
#include "modal_document_guard.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace retas {
std::optional<FrameDragOperation> hit_frame_move(const std::array<FramePoint,4>& corners,FramePoint p,bool camera){
    if(camera)for(const auto c:corners)if(std::int64_t(p.x)>=std::int64_t(c.x)-4&&std::int64_t(p.x)<std::int64_t(c.x)+4&&std::int64_t(p.y)>=std::int64_t(c.y)-4&&std::int64_t(p.y)<std::int64_t(c.y)+4)return FrameDragOperation::Scale;
    int sign=0;bool inside=true;
    for(int i=0;i<4;++i){const auto a=corners[i],b=corners[(i+1)%4];
        const long double cross=(static_cast<long double>(b.x)-a.x)*(static_cast<long double>(p.y)-a.y)-(static_cast<long double>(b.y)-a.y)*(static_cast<long double>(p.x)-a.x);
        if(cross==0&&p.x>=std::min(a.x,b.x)&&p.x<=std::max(a.x,b.x)&&p.y>=std::min(a.y,b.y)&&p.y<=std::max(a.y,b.y))return {};
        const int next=(cross>0)-(cross<0);if(next){if(sign&&sign!=next)inside=false;sign=next;}
    }
    if(inside&&sign)return FrameDragOperation::Translate;
    return camera?std::optional{FrameDragOperation::Rotate}:std::nullopt;
}
namespace {
using numeric_detail::binary64;
using namespace modal_detail;
constexpr std::uint32_t origin_family=0x40001008,camera_family=0x40002008;
int integer(std::int64_t n){if(n<INT32_MIN||n>INT32_MAX)throw std::invalid_argument("Frame coordinate exceeds int32");return int(n);}
int ftol(double n){if(!std::isfinite(n)||std::trunc(n)<INT32_MIN||std::trunc(n)>INT32_MAX)throw std::invalid_argument("Frame arithmetic exceeds int32");return int(std::trunc(n));}
const Plane& node(const DocumentData& d,FrameTarget t){
    const auto& l=d.layers.at(t.layer);const auto& p=l.planes.at(t.plane);
    if(l.opaque||l.family!=0x40001000||p.opaque||p.raster||(p.family!=origin_family&&p.family!=camera_family)||p.specific.size()<24)throw std::invalid_argument("Frame Move requires an ordinary FrameLayer Origin/Camera plane with24-byte payload");
    if(!std::isfinite(d.base.dpi)||d.base.dpi<=0)throw std::invalid_argument("Invalid Frame document DPI");return p;
}
std::span<const std::uint8_t> tail(const Plane& p){return std::span(p.specific).subspan(24);}
FramePoint world_point(const DocumentData& d,FrameTarget t){
    const auto& p=node(d,t);if(p.family==origin_family)return {overlay_detail::pixels(d.base.lengths[2],d.base.dpi),overlay_detail::pixels(d.base.lengths[3],d.base.dpi)};
    const auto q=decode_frame_parameters(p.specific).point;const auto& c=d.layers[t.layer].common;return {integer(std::int64_t(q.x)+c.x),integer(std::int64_t(q.y)+c.y)};
}
FramePoint local_point(const DocumentData& d,FrameTarget t,FramePoint world){const auto& c=d.layers[t.layer].common;return {integer(std::int64_t(world.x)-c.x),integer(std::int64_t(world.y)-c.y)};}
void set_position(DocumentData& d,FrameTarget t,FramePoint world){
    const auto& p=node(d,t);if(p.family==origin_family){d.base.lengths[2]={double(world.x),0,d.base.dpi};d.base.lengths[3]={double(world.y),0,d.base.dpi};}
    else {auto v=decode_frame_parameters(p.specific);v.point=local_point(d,t,world);auto b=encode_frame_parameters(v,tail(p));d.layers[t.layer].planes[t.plane].specific.swap(b);}
}
void scalars(double scale,double angle){if(!std::isfinite(scale)||scale<0||scale>1000||!std::isfinite(angle)||angle< -180||angle>180)throw std::invalid_argument("Frame active scale/angle outside finite0..1000/-180..180 profile");}
Rect centered(FramePoint c,int w,int h){if(w<0||h<0||w>100000000||h>100000000)throw std::invalid_argument("Frame dimensions outside bounded nonnegative domain");const auto x=integer(std::int64_t(c.x)-w/2),y=integer(std::int64_t(c.y)-h/2);integer(std::int64_t(x)+w);integer(std::int64_t(y)+h);return {x,y,w,h};}
FramePoint center(Rect r){
    // RCRect::CenterPoint adds its two signed32 edges BEFORE division.
    // Reject that native unchecked wrap domain, even if a widened midpoint
    // would fit. Signed division by2 matches its CDQ/SUB/SAR toward-zero path.
    const int x=integer(std::int64_t(r.x)*2+r.width),y=integer(std::int64_t(r.y)*2+r.height);
    return {x/2,y/2};
}
FramePoint preset_point(const DocumentData& d,FrameTarget t,FramePreset preset){
    const auto p=frame_parameters(d,t);scalars(p.scale,p.angle);const int w=overlay_detail::pixels(d.base.lengths[4],d.base.dpi),h=overlay_detail::pixels(d.base.lengths[5],d.base.dpi);
    auto r=centered(p.point,ftol(binary64(binary64(p.scale*w)*.01)),ftol(binary64(binary64(p.scale*h)*.01)));
    const auto& b=d.base;const int l=overlay_detail::pixels(b.lengths[6],b.dpi),top=overlay_detail::pixels(b.lengths[7],b.dpi),right=integer(std::int64_t(overlay_detail::pixels(b.lengths[0],b.dpi))-overlay_detail::pixels(b.lengths[8],b.dpi)),bottom=integer(std::int64_t(overlay_detail::pixels(b.lengths[1],b.dpi))-overlay_detail::pixels(b.lengths[9],b.dpi));
    std::int64_t dx{},dy{};
    switch(preset){case FramePreset::TopLeft:dx=std::int64_t(l)-r.x;dy=std::int64_t(top)-r.y;break;
    case FramePreset::BottomLeft:dx=std::int64_t(l)-r.x;dy=std::int64_t(bottom)-r.y-r.height;break;
    case FramePreset::Center:{const auto a=center(r),bcenter=center({l,top,integer(std::int64_t(right)-l),integer(std::int64_t(bottom)-top)});dx=std::int64_t(bcenter.x)-a.x;dy=std::int64_t(bcenter.y)-a.y;break;}
    case FramePreset::TopRight:dx=std::int64_t(right)-r.x-r.width;dy=std::int64_t(top)-r.y;break;
    case FramePreset::BottomRight:dx=std::int64_t(right)-r.x-r.width;dy=std::int64_t(bottom)-r.y-r.height;break;
    default:throw std::invalid_argument("Invalid Frame preset");}
    r.x=integer(std::int64_t(r.x)+dx);r.y=integer(std::int64_t(r.y)+dy);integer(std::int64_t(r.x)+r.width);integer(std::int64_t(r.y)+r.height);return center(r);
}
int heading(FramePoint p,FramePoint pivot){const int x=integer(std::int64_t(p.x)-pivot.x),y=integer(std::int64_t(p.y)-pivot.y);const int n=ftol(binary64(std::atan2(double(y),double(x))*std::bit_cast<double>(0x404ca5dc1a63c1f8ull)));return n<0?-n:360-n;}
double distance(FramePoint a,FramePoint b){const double x=binary64(double(a.x)-b.x),y=binary64(double(a.y)-b.y);return binary64(std::sqrt(binary64(binary64(x*x)+binary64(y*y))));}
}
FrameParameters decode_frame_parameters(std::span<const std::uint8_t> b){if(b.size()<24)throw FormatError(0,"Truncated Frame parameters");Reader r(b);FrameParameters p;p.point={r.i32(),r.i32()};p.scale=r.f64();p.angle=r.f64();return p;}
Bytes encode_frame_parameters(const FrameParameters& p,std::span<const std::uint8_t> suffix){if(suffix.size()>512ull*1024*1024-24)throw std::length_error("Frame payload exceeds512MiB");Writer w;w.data.reserve(24+suffix.size());w.i32(p.point.x);w.i32(p.point.y);w.f64(p.scale);w.f64(p.angle);w.raw(suffix);return std::move(w.data);}
FrameParameters frame_parameters(const DocumentData& d,FrameTarget t,bool world){auto p=decode_frame_parameters(node(d,t).specific);p.point=world_point(d,t);if(!world)p.point=local_point(d,t,p.point);return p;}
FrameSettingPacket frame_settings(const DocumentData& d,FrameTarget t){auto p=frame_parameters(d,t);if(node(d,t).family==origin_family){p.scale=100;p.angle=0;}return {{{{double(p.point.x),0,d.base.dpi},{double(p.point.y),0,d.base.dpi}}},p.scale,p.angle};}
FrameGeometry frame_geometry(const DocumentData& d,FrameTarget t,bool world){
    const auto p=frame_parameters(d,t,world);scalars(p.scale,p.angle);const int w=overlay_detail::pixels(d.base.lengths[4],d.base.dpi),h=overlay_detail::pixels(d.base.lengths[5],d.base.dpi);FrameGeometry g;
    g.unscaled=centered(p.point,w,h);g.scaled=centered(p.point,ftol(binary64(binary64(p.scale*w)*.01)),ftol(binary64(binary64(p.scale*h)*.01)));
    const auto r=g.unscaled;g.corners={FramePoint{r.x,r.y},{r.x,integer(std::int64_t(r.y)+r.height)},{integer(std::int64_t(r.x)+r.width),integer(std::int64_t(r.y)+r.height)},{integer(std::int64_t(r.x)+r.width),r.y}};
    // RCMatrix2D identity -> Rotate about center -> Scale about center. Keep
    // matrix coefficients/stores and final FTOL; no rtDouble2Int corner rounding.
    const double theta=binary64(p.angle*std::bit_cast<double>(0x3f91df46a2529d39ull)),c=binary64(std::cos(theta)),s=binary64(std::sin(theta)),ratio=binary64(p.scale*.01);
    double tx=binary64(binary64(binary64(-double(p.point.x)*c)+binary64(-double(p.point.y)*-s))+p.point.x),ty=binary64(binary64(binary64(-double(p.point.x)*s)+binary64(-double(p.point.y)*c))+p.point.y);
    tx=binary64(binary64(binary64(tx-p.point.x)*ratio)+p.point.x);ty=binary64(binary64(binary64(ty-p.point.y)*ratio)+p.point.y);
    const double a=binary64(c*ratio),b=binary64(-s*ratio),cc=binary64(s*ratio),dd=binary64(c*ratio);
    for(auto& q:g.corners){const auto x=q.x,y=q.y;q.x=ftol(binary64(binary64(binary64(x*a)+binary64(y*b))+tx));q.y=ftol(binary64(binary64(binary64(x*cc)+binary64(y*dd))+ty));}return g;
}
FramePrintGeometry frame_print_geometry(const DocumentData& d,FrameTarget target){
    const auto world=frame_parameters(d,target),local=frame_parameters(d,target,false);const auto geometry=frame_geometry(d,target);FramePrintGeometry out;out.outline=geometry.corners;
    const auto transform=[&](FramePoint q,FramePoint pivot){
        const double theta=binary64(world.angle*std::bit_cast<double>(0x3f91df46a2529d39ull)),c=binary64(std::cos(theta)),s=binary64(std::sin(theta)),k=binary64(world.scale*.01);
        double tx=binary64(binary64(binary64(-double(pivot.x)*c)+binary64(-double(pivot.y)*-s))+pivot.x),ty=binary64(binary64(binary64(-double(pivot.x)*s)+binary64(-double(pivot.y)*c))+pivot.y);
        tx=binary64(binary64(binary64(tx-pivot.x)*k)+pivot.x);ty=binary64(binary64(binary64(ty-pivot.y)*k)+pivot.y);
        return FramePoint{ftol(binary64(binary64(binary64(q.x*binary64(c*k))+binary64(q.y*binary64(-s*k)))+tx)),ftol(binary64(binary64(binary64(q.x*binary64(s*k))+binary64(q.y*binary64(c*k)))+ty))};
    };
    const auto r=geometry.unscaled;const auto& lengths=d.base.lengths;
    const int l=integer(std::int64_t(r.x)+overlay_detail::pixels(lengths[10],d.base.dpi)),t=integer(std::int64_t(r.y)+overlay_detail::pixels(lengths[11],d.base.dpi)),right=integer(std::int64_t(r.x)+r.width-overlay_detail::pixels(lengths[12],d.base.dpi)),bottom=integer(std::int64_t(r.y)+r.height-overlay_detail::pixels(lengths[13],d.base.dpi));
    const auto centimeters=[&](double value){const auto n=legacy_rt_double_to_int(binary64(binary64(d.base.dpi*value)*std::bit_cast<double>(0x3fd93264c993264cull)));if(!n)throw std::invalid_argument("Frame print length overflow");return *n;};
    const int radius=centimeters(.25);
    const std::array<FramePoint,4> centers{{{integer(std::int64_t(l)+radius),integer(std::int64_t(t)+radius)},{integer(std::int64_t(l)+radius),integer(std::int64_t(bottom)-radius)},{integer(std::int64_t(right)-radius),integer(std::int64_t(bottom)-radius)},{integer(std::int64_t(right)-radius),integer(std::int64_t(t)+radius)}}};
    const std::array<FramePoint,4> vectors{{{0,-radius},{-radius,0},{0,radius},{radius,0}}};
    for(int corner=0;corner<4;++corner)for(int i=0;i<10;++i){const auto theta=binary64(double(i*10)*std::bit_cast<double>(0x3f91df46a2529d39ull));const auto c=std::cos(theta),s=std::sin(theta);const auto v=vectors[corner],p=centers[corner];
        const auto x=legacy_rt_double_to_int(p.x+v.x*c+v.y*s),y=legacy_rt_double_to_int(p.y+v.y*c-v.x*s);if(!x||!y)throw std::invalid_argument("Frame safe-area coordinate overflow");out.safe_area[corner*10+i]=transform({*x,*y},local.point);}
    const auto middle=center(r);const int length=centimeters(.5),rr=integer(std::int64_t(r.x)+r.width),bb=integer(std::int64_t(r.y)+r.height-1);
    out.marks={{{{{r.x,middle.y},{integer(std::int64_t(r.x)+length),middle.y}}},{{{middle.x,r.y},{middle.x,integer(std::int64_t(r.y)+length)}}},{{{rr,middle.y},{integer(std::int64_t(rr)-length),middle.y}}},{{{middle.x,bb},{middle.x,integer(std::int64_t(bb)-length)}}},{{{middle.x,integer(std::int64_t(middle.y)-length)},{middle.x,integer(std::int64_t(middle.y)+length)}}},{{{integer(std::int64_t(middle.x)-length),middle.y},{integer(std::int64_t(middle.x)+length),middle.y}}}}};
    for(auto& mark:out.marks)for(auto& point:mark)point=transform(point,world.point);return out;
}
struct FrameMoveSession::Impl {
    const Document* owner{};const Layer* layer{};const Plane* plane{};FrameTarget target;
    std::shared_ptr<const DocumentData> initial,expected;Bytes publication;
    FrameParameters primary;FrameSettingPacket backup;FrameMoveState state;std::optional<FrameDragPacket> drag;
    FrameParameters captured;RegionSelection selection;const BinaryMask* mask_identity{};GridState grid;GuideState guide;
    std::uint64_t generation{},selection_generation{},grid_generation{},guide_generation{},work{},guard_work{};
    bool write{},pending{};
    std::shared_ptr<Impl> next(const Document& d,std::stop_token stop,bool cancellation=false) const {
        poll(stop);const auto visit=cancellation?0:guard_work*4;
        // Three complete source guards plus one conservative metadata copy.
        // Final Cancel/Accept consumes the mutually exclusive reserve charged
        // at Begin, so work exhaustion cannot make cancellation unavailable.
        if(visit>max_work-work)throw std::length_error("Frame cumulative guards exceed128Mi units");
        auto result=std::make_shared<Impl>(*this);result->work+=visit;result->generation=d.state_generation()+1;
        result->write=false;return result;
    }
    void complete(std::shared_ptr<DocumentData> data,bool changed,std::stop_token stop){
        state.settings=frame_settings(*data,target);state.geometry=frame_geometry(*data,target);state.dragging=bool(drag);
        if(changed){++state.notifications;publication=data->layers[target.layer].planes[target.plane].specific;write=true;}
        expected=std::move(data);poll(stop);
    }
};
FrameMoveSession::FrameMoveSession(std::shared_ptr<Impl> p):impl_(std::move(p)){}
FrameMoveState FrameMoveSession::state() const{return impl_->state;}
FrameParameters FrameMoveSession::primary() const{return impl_->primary;}
bool FrameMoveSession::matches(const Document& d) const noexcept {
    const auto& i=*impl_;try{
        if(&d!=i.owner||d.state_generation()!=(i.pending?i.generation-1:i.generation)||d.region_selection_generation()!=i.selection_generation||d.grid_generation()!=i.grid_generation||d.guide_generation()!=i.guide_generation||i.target.layer>=d.data.layers.size()||&d.data.layers[i.target.layer]!=i.layer||i.target.plane>=d.data.layers[i.target.layer].planes.size()||&d.data.layers[i.target.layer].planes[i.target.plane]!=i.plane||d.region_selection.mask.get()!=i.mask_identity)return false;
        // Bit equality keeps inactive NaNs/signed zero stable and detects payload changes.
        if(!bits(d.grid.dpi,i.grid.dpi)||d.grid.origin!=i.grid.origin||d.grid.subdivisions!=i.grid.subdivisions||d.grid.enabled!=i.grid.enabled||!length(d.grid.spacing,i.grid.spacing)||!bits(d.guide.dpi,i.guide.dpi)||d.guide.origin!=i.guide.origin||d.guide.enabled!=i.guide.enabled||d.guide.records.size()!=i.guide.records.size())return false;
        for(std::size_t k=0;k<2;++k)if(!length(d.grid.position[k],i.grid.position[k])||!length(d.guide.position[k],i.guide.position[k]))return false;
        for(std::size_t k=0;k<d.guide.records.size();++k)if(!length(d.guide.records[k].position,i.guide.records[k].position)||d.guide.records[k].orientation!=i.guide.records[k].orientation||d.guide.records[k].marker!=i.guide.records[k].marker)return false;
        return data_same(d.data,*(i.pending?i.initial:i.expected))&&selection_same(d.region_selection,i.selection);
    }catch(...){return false;}
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::prepare(const Document& d,FrameTarget t,std::stop_token stop){
    poll(stop);const auto& p=node(d.data,t);if(d.has_floating_selection())throw std::invalid_argument("Frame Move requires settled artwork");
    std::uint64_t n{};data_charge(d.data,n);charge(n,d.guide.records.size()*sizeof(GuideRecord),max_bytes,"Frame Guide metadata exceeds512MiB");std::uint64_t mask_work{};
    if(d.region_selection.mask){const auto& m=*d.region_selection.mask;if(m.width()<0||m.height()<0)throw std::invalid_argument("Invalid Frame selection carrier");mask_work=std::uint64_t(m.width())*m.height();if(mask_work>max_work)throw std::length_error("Frame mask guard exceeds128Mi units");charge(n,((std::uint64_t(m.width())+31)/32)*m.height()*4+((std::uint64_t(m.width())+255)/256)*((std::uint64_t(m.height())+255)/256),max_bytes,"Frame mask storage exceeds512MiB");}
    if(n>max_bytes/4)throw std::length_error("Frame logical source/frozen/preparation aliases exceed512MiB");
    const auto guard_work=n+mask_work*2+1;if(guard_work>max_work/6)throw std::length_error("Frame admission/finalization guard work exceeds128Mi units");
    auto i=std::make_shared<Impl>();i->owner=&d;i->target=t;i->layer=&d.data.layers[t.layer];i->plane=&p;i->generation=d.state_generation()+1;i->selection_generation=d.region_selection_generation();i->grid_generation=d.grid_generation();i->guide_generation=d.guide_generation();i->grid=d.grid;i->guide=d.guide;i->selection=d.region_selection;i->mask_identity=i->selection.mask.get();if(i->selection.mask)i->selection.mask=std::make_shared<const BinaryMask>(*i->selection.mask);
    i->initial=std::make_shared<const DocumentData>(frozen(d.data,stop));auto expected=std::make_shared<DocumentData>(*i->initial);i->pending=true;i->primary=frame_parameters(*expected,t,false);
    if(p.family==origin_family){auto raw=decode_frame_parameters(p.specific);raw.point=i->primary.point;expected->layers[t.layer].planes[t.plane].specific=encode_frame_parameters(raw,tail(p));}
    i->backup=frame_settings(*expected,t);i->state={t,i->backup,frame_geometry(*expected,t),false,0};expected->layers[t.layer].planes[t.plane].frame_modified=true;
    i->publication=expected->layers[t.layer].planes[t.plane].specific;i->expected=std::move(expected);i->write=true;i->guard_work=guard_work;i->work=guard_work*6;poll(stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::drag(const Document& d,const FrameDragPacket& packet,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Frame source changed");if(impl_->drag)throw std::logic_error("Frame drag already active");
    if(packet.operation!=FrameDragOperation::Translate&&packet.operation!=FrameDragOperation::Scale&&packet.operation!=FrameDragOperation::Rotate)throw std::invalid_argument("Invalid Frame drag operation");
    if(node(d.data,impl_->target).family==origin_family&&packet.operation!=FrameDragOperation::Translate)throw std::invalid_argument("Origin Frame only supports translation");
    if(packet.operation==FrameDragOperation::Scale&&distance(packet.view,packet.pivot)==0)throw std::invalid_argument("Frame scale start has zero pivot distance");
    auto i=impl_->next(d,stop);i->drag=packet;i->captured=frame_parameters(d.data,i->target);scalars(i->captured.scale,i->captured.angle);i->state.dragging=true;poll(stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::update(const Document& d,FramePoint view,FramePoint world,bool shift,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Frame source changed");if(!impl_->drag)throw std::logic_error("No active Frame drag");
    auto i=impl_->next(d,stop);auto data=std::make_shared<DocumentData>(*i->expected);const auto& drag=*i->drag;bool changed{};
    if(drag.operation==FrameDragOperation::Translate){const int dx=integer(std::int64_t(world.x)-drag.world.x),dy=integer(std::int64_t(world.y)-drag.world.y);
        // 547DC0 tests the captured delta, not equality with current live point.
        if(dx||dy){set_position(*data,i->target,{integer(std::int64_t(i->captured.point.x)+dx),integer(std::int64_t(i->captured.point.y)+dy)});changed=true;}}
    else {auto raw=decode_frame_parameters(node(*data,i->target).specific);
        if(drag.operation==FrameDragOperation::Scale){const double r0=distance(drag.view,drag.pivot),r1=distance(view,drag.pivot);
            const double value=binary64(binary64(binary64(binary64(r1/r0)*100.0)*i->captured.scale)*.01);const auto rounded=legacy_rt_double_to_int(value);if(!rounded)throw std::invalid_argument("Frame drag scale arithmetic exceeds int32");
            const double scale=std::clamp(double(*rounded),10.0,1000.0);changed=scale!=raw.scale;raw.scale=scale;
        }else {int delta=heading(view,drag.pivot)-heading(drag.view,drag.pivot);if(shift)delta-=delta%45;
            int angle=integer(std::int64_t(ftol(i->captured.angle))-delta);if(angle<=-180)angle=integer(std::int64_t(angle)+360);else if(angle>180)angle-=360;
            changed=double(angle)!=raw.angle;raw.angle=double(angle);}
        if(changed){auto b=encode_frame_parameters(raw,tail(node(*data,i->target)));data->layers[i->target.layer].planes[i->target.plane].specific.swap(b);}}
    i->complete(std::move(data),changed,stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
namespace {
void apply_settings(DocumentData& d,FrameTarget t,const FrameSettingPacket& p,bool controls){
    const bool origin=node(d,t).family==origin_family;const double scale=origin?100:p.scale,angle=origin?0:p.angle;
    if(controls&&!origin&&(scale<10||scale>1000))throw std::invalid_argument("Camera numeric scale outside10..1000");scalars(scale,angle);
    const FramePoint world{overlay_detail::pixels({p.position[0].value,p.position[0].unit,p.position[0].reference},d.base.dpi),overlay_detail::pixels({p.position[1].value,p.position[1].unit,p.position[1].reference},d.base.dpi)};
    auto raw=decode_frame_parameters(node(d,t).specific);raw.scale=scale;raw.angle=angle;
    if(!origin)raw.point=local_point(d,t,world);
    auto b=encode_frame_parameters(raw,tail(node(d,t)));d.layers[t.layer].planes[t.plane].specific.swap(b);if(origin)set_position(d,t,world);
}
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::settings(const Document& d,const FrameSettingPacket& p,std::stop_token stop,bool controls) const {
    if(!matches(d))throw std::logic_error("Frame source changed");auto i=impl_->next(d,stop,!controls);auto data=std::make_shared<DocumentData>(*i->expected);apply_settings(*data,i->target,p,controls);i->complete(std::move(data),true,stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::preset(const Document& d,FramePreset p,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Frame source changed");auto i=impl_->next(d,stop);auto data=std::make_shared<DocumentData>(*i->expected);set_position(*data,i->target,preset_point(*data,i->target,p));i->complete(std::move(data),true,stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::release(const Document& d,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Frame source changed");auto i=impl_->next(d,stop);i->drag.reset();i->state.dragging=false;poll(stop);return std::shared_ptr<FrameMoveSession>(new FrameMoveSession(std::move(i)));
}
std::shared_ptr<FrameMoveSession> FrameMoveSession::cancellation(const Document& d,std::stop_token stop) const{return settings(d,impl_->backup,stop,false);}
void FrameMoveSession::publish(Document& d) noexcept {
    impl_->pending=false;
    if(impl_->write){auto& p=d.data.layers[impl_->target.layer].planes[impl_->target.plane];p.specific.swap(impl_->publication);p.frame_modified=true;
        if(p.family==origin_family){d.data.base.lengths[2]=impl_->expected->base.lengths[2];d.data.base.lengths[3]=impl_->expected->base.lengths[3];}}
}
void Document::require_current_frame_move() const {if(!frame_move_)throw std::logic_error("No active Frame Move");if(!frame_move_->matches(*this))throw std::logic_error("Frame Move source or runtime changed");}
void Document::begin_frame_move(FrameTarget target,std::stop_token stop){
    require_no_floating_transform();frame_move_preparing_=true;
    struct ClearPreparing{bool& value;~ClearPreparing(){value=false;}} clear{frame_move_preparing_};
    auto session=FrameMoveSession::prepare(*this,target,stop);
    if(!session->matches(*this)||stop.stop_requested())throw std::logic_error("Frame source changed before Bind publication");
    session->publish(*this);frame_move_=std::move(session);++state_generation_;
}
void Document::begin_frame_drag(const FrameDragPacket& p,std::stop_token stop){require_current_frame_move();const auto original=frame_move_;auto next=original->drag(*this,p,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before publication");next->publish(*this);frame_move_=std::move(next);++state_generation_;}
bool Document::update_frame_drag(FramePoint view,FramePoint world,bool shift,std::stop_token stop){require_current_frame_move();const auto original=frame_move_;auto next=original->update(*this,view,world,shift,stop);const bool notified=next->state().notifications!=original->state().notifications;if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before publication");next->publish(*this);frame_move_=std::move(next);++state_generation_;return notified;}
void Document::release_frame_drag(std::stop_token stop){require_current_frame_move();const auto original=frame_move_;auto next=original->release(*this,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before release");frame_move_=std::move(next);++state_generation_;}
void Document::apply_frame_settings(const FrameSettingPacket& p,std::stop_token stop){require_current_frame_move();const auto original=frame_move_;auto next=original->settings(*this,p,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before publication");next->publish(*this);frame_move_=std::move(next);++state_generation_;}
void Document::apply_frame_preset(FramePreset p,std::stop_token stop){require_current_frame_move();const auto original=frame_move_;auto next=original->preset(*this,p,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before publication");next->publish(*this);frame_move_=std::move(next);++state_generation_;}
void Document::accept_frame_move(const std::string& label,std::stop_token stop){
    require_current_frame_move();poll(stop);const auto session=frame_move_;FrameEdit edit{session->state().target,node(data,session->state().target).family,session->primary(),{},false};
    Revision prepared{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)};undo_.reserve(undo_.size()+1);
    if(frame_move_!=session||!session->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before accept publication");
    undo_.push_back(std::move(prepared));if(undo_.size()>100)undo_.erase(undo_.begin());redo_.clear();undo_next_=true;revision_=next_++;frame_move_.reset();++state_generation_;
}
void Document::cancel_frame_move(std::stop_token stop){
    if(frame_move_preparing_)throw std::logic_error("Frame begin is still preparing");if(!frame_move_)return;if(!frame_move_->matches(*this)){frame_move_.reset();throw std::logic_error("Stale Frame Cancel preserves external edits and releases ownership");}
    const auto original=frame_move_;auto next=original->cancellation(*this,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Frame changed before cancellation");next->publish(*this);frame_move_.reset();++state_generation_;
}
void Document::replay_frame(std::vector<Revision>& source,std::vector<Revision>& destination){
    auto edit=std::get<FrameEdit>(source.back().runtime);const auto& p=node(data,edit.target);if(p.family!=edit.family)throw std::logic_error("Frame history target changed");
    // Prepare cache/packet bytes and normalized Origin lengths before allocating
    // the opposite stack. First Undo is the only lazy getter cache refresh.
    if(p.specific.size()>max_bytes/4)throw std::length_error("Frame replay packet aliases exceed512MiB");
    auto raw=decode_frame_parameters(p.specific);const auto& owner=data.layers[edit.target.layer].common;
    if(!edit.secondary){auto opposite=frame_parameters(data,edit.target,false);edit.secondary=opposite;if(p.family==origin_family)raw.point=opposite.point;}
    const auto desired=edit.reverse?*edit.secondary:edit.primary;
    std::array<Length,2> position{data.base.lengths[2],data.base.lengths[3]};
    if(p.family==origin_family){const int x=integer(std::int64_t(desired.point.x)+owner.x),y=integer(std::int64_t(desired.point.y)+owner.y);position={Length{double(x),0,data.base.dpi},Length{double(y),0,data.base.dpi}};}
    else raw=desired;
    auto bytes=encode_frame_parameters(raw,tail(p));edit.reverse=!edit.reverse;
    destination.push_back({DocumentData{},source.back().label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    auto& target=data.layers[std::get<FrameEdit>(destination.back().runtime).target.layer].planes[std::get<FrameEdit>(destination.back().runtime).target.plane];target.specific.swap(bytes);target.frame_modified=true;
    if(target.family==origin_family){data.base.lengths[2]=position[0];data.base.lengths[3]=position[1];}
}
}
