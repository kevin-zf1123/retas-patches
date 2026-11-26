#include "retas/vanishing_point_move.hpp"
#include "retas/document.hpp"
#include "retas/numeric.hpp"
#include "modal_document_guard.hpp"
#include <cmath>
#include <limits>

namespace retas { namespace {
using namespace modal_detail;
constexpr std::uint32_t family=0x40000080;
std::uint64_t parameter_charge(const VanishingPointParameters& p){
    if(p.points.size()>100000)throw std::length_error("Vanishing point count exceeds100000");
    std::uint64_t n=sizeof(p);charge(n,p.points.size()*sizeof(VanishingPoint),max_bytes,"Vanishing parameters exceed512MiB");
    bytes_charge(n,p.header_suffix);bytes_charge(n,p.points_suffix);bytes_charge(n,p.suffix);return n;
}
bool signaling(double value){const auto b=std::bit_cast<std::uint64_t>(value);return (b&0x7ff0000000000000ull)==0x7ff0000000000000ull&&(b&0x000fffffffffffffull)&&!(b&0x0008000000000000ull);}
FrameLength effective(FrameLength v){if(signaling(v.value)||signaling(v.reference))throw std::invalid_argument("Signaling NaN in active vanishing parameters");if(v.unit==-2)v.unit=1;if(!(v.reference>0))v.reference=0;return v;}
VanishingPointParameters copied(const VanishingPointParameters& source,bool load){
    if(parameter_charge(source)>max_bytes/6)throw std::length_error("Vanishing copy aliases exceed512MiB");auto p=source;p.legacy_header=false;
    if(load&&p.angle_step!=1&&p.angle_step!=5&&p.angle_step!=10&&p.angle_step!=15&&p.angle_step!=30)p.angle_step=5;
    for(auto& point:p.points){point.x=effective(point.x);point.y=effective(point.y);}return p;
}
int integer(std::int64_t v){if(v<INT32_MIN||v>INT32_MAX)throw std::invalid_argument("Vanishing coordinate exceeds int32");return int(v);}
int pixel(FrameLength v,double dpi){
    using numeric_detail::binary64;
    if((v.unit==0&&v.reference==0)||v.unit==4||v.unit<0||v.unit>6)return 0;
    double n{};
    switch(v.unit){case 0:n=binary64(binary64(v.value/v.reference)*dpi);break;
    case 1:n=binary64(binary64(dpi*v.value)*0.3937007874015748);break;
    case 2:n=binary64(binary64(dpi*v.value)*0.03937007874015748);break;
    case 3:n=binary64(dpi*v.value);break;
    case 5:n=binary64(binary64(dpi*v.value)*0.01388888888888889);break;
    case 6:n=binary64(binary64(dpi*v.value)*0.00984251968503937);break;default:return 0;}
    const auto result=legacy_rt_double_to_int(n);if(!result)throw std::invalid_argument("Vanishing Length conversion outside finite int32 domain");return *result;
}
const Plane& node(const DocumentData& d,VanishingPointTarget t){const auto& l=d.layers.at(t.layer);const auto& p=l.planes.at(t.plane);
    if(l.opaque||l.family!=0x40020000||p.opaque||p.family!=family||p.raster)throw std::invalid_argument("Vanishing Move requires ordinary VanishingPointLayer/Plane");
    if(!std::isfinite(d.base.dpi)||d.base.dpi<=0)throw std::invalid_argument("Invalid vanishing document DPI");return p;}
void validate(const VanishingPointParameters& p,double dpi){(void)vanishing_point_pixels(p,dpi);}
}
VanishingPointParameters decode_vanishing_point_parameters(std::span<const std::uint8_t> bytes){
    if(bytes.size()>max_bytes/2)throw std::length_error("Vanishing raw input/retained storage exceeds512MiB");Reader r(bytes);auto h=r.section();
    if(h.remaining()!=12&&h.remaining()<16)throw FormatError(h.offset(),"Invalid vanishing header length");
    const auto count=h.u32();VanishingPointParameters p;p.angle_step=h.i32();p.color=h.u32();p.legacy_header=h.remaining()==0;if(!p.legacy_header)p.opacity=h.i32();
    auto points=r.section();if(count>100000||count>points.remaining()/44)throw FormatError(points.offset(),"Invalid vanishing count");
    std::uint64_t charge_bytes=bytes.size();charge(charge_bytes,std::uint64_t(count)*sizeof(VanishingPoint),max_bytes,"Vanishing decoded storage exceeds512MiB");charge(charge_bytes,h.remaining()+points.remaining()-std::uint64_t(count)*44+r.remaining(),max_bytes,"Vanishing tails exceed512MiB");
    p.header_suffix=h.rest();p.points.reserve(count);
    for(std::uint32_t i=0;i<count;++i){VanishingPoint q;q.x={points.f64(),points.i32(),points.f64()};q.y={points.f64(),points.i32(),points.f64()};q.selected=points.i32();p.points.push_back(q);}
    p.points_suffix=points.rest();p.suffix=r.rest();return p;
}
Bytes encode_vanishing_point_parameters(const VanishingPointParameters& p){
    auto n=parameter_charge(p);const std::uint64_t header=(p.legacy_header&&p.opacity==50&&p.header_suffix.empty()?12:16)+p.header_suffix.size(),points=p.points.size()*44ull+p.points_suffix.size();
    charge(n,8+header+points+p.suffix.size(),max_bytes,"Vanishing input/output storage exceeds512MiB");if(header>UINT32_MAX||points>UINT32_MAX)throw std::length_error("Vanishing section exceeds32bits");
    Writer w;w.data.reserve(std::size_t(8+header+points+p.suffix.size()));w.u32(std::uint32_t(header));w.u32(std::uint32_t(p.points.size()));w.i32(p.angle_step);w.u32(p.color);if(!(p.legacy_header&&p.opacity==50&&p.header_suffix.empty()))w.i32(p.opacity);w.raw(p.header_suffix);w.u32(std::uint32_t(points));
    for(const auto& q:p.points){w.f64(q.x.value);w.i32(q.x.unit);w.f64(q.x.reference);w.f64(q.y.value);w.i32(q.y.unit);w.f64(q.y.reference);w.i32(q.selected);}w.raw(p.points_suffix);w.raw(p.suffix);return std::move(w.data);
}
VanishingPointParameters materialize_vanishing_point_parameters(const VanishingPointParameters& p){return copied(p,true);}
std::vector<VanishingPointPixel> vanishing_point_pixels(const VanishingPointParameters& p,double dpi){parameter_charge(p);if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("Invalid vanishing DPI");std::vector<VanishingPointPixel> result;result.reserve(p.points.size());for(const auto& q:p.points)result.push_back({pixel(q.x,dpi),pixel(q.y,dpi)});return result;}
VanishingPointParameters vanishing_point_parameters(const DocumentData& d,VanishingPointTarget t){return decode_vanishing_point_parameters(node(d,t).specific);}
VanishingPointHit hit_vanishing_points(std::span<const VanishingPointPixel> centers,VanishingPointPixel sample,bool alt){if(centers.size()>100000)throw std::length_error("Vanishing hit count exceeds100000");for(std::size_t i=0;i<centers.size();++i){const auto p=centers[i];integer(std::int64_t(p.x)+8);integer(std::int64_t(p.y)+8);integer(std::int64_t(p.x)-4);integer(std::int64_t(p.y)-4);if(std::int64_t(sample.x)>=std::int64_t(p.x)-4&&std::int64_t(sample.x)<std::int64_t(p.x)+4&&std::int64_t(sample.y)>=std::int64_t(p.y)-4&&std::int64_t(sample.y)<std::int64_t(p.y)+4)return {alt?VanishingPointHitKind::Delete:VanishingPointHitKind::Move,i};}return {centers.size()<10?VanishingPointHitKind::Add:VanishingPointHitKind::Neutral,{}};}
struct VanishingPointMoveSession::Impl {
    const Document* owner{};const Layer* layer{};const Plane* plane{};VanishingPointTarget target;
    std::shared_ptr<const DocumentData> initial,expected;std::shared_ptr<const VanishingPointParameters> primary;
    VanishingPointMoveState state;Bytes publication;RegionSelection selection;const BinaryMask* mask_identity{};GridState grid;GuideState guide;
    std::uint64_t generation{},selection_generation{},grid_generation{},guide_generation{},work{},guard_work{},base_charge{},parameter_cost{};
    std::optional<std::size_t> index;VanishingPointPixel press_world,saved;bool first{},gesture{},write{},pending{};
    std::shared_ptr<Impl> next(const Document& d,std::stop_token stop,bool cancel=false) const {poll(stop);const auto visit=cancel?0:guard_work*4;if(visit>max_work-work)throw std::length_error("Vanishing cumulative work exceeds128Mi units");auto i=std::make_shared<Impl>(*this);i->work+=visit;i->generation=d.state_generation()+1;i->write=false;return i;}
    void admit_parameter(std::uint64_t amount){const auto cost=std::max(parameter_cost,amount);if(base_charge+cost*6>max_bytes/4)throw std::length_error("Vanishing growing parameter aliases exceed512MiB");const auto extra=(cost-parameter_cost)*6;if(extra>max_work/6||extra*6>max_work-work)throw std::length_error("Vanishing growing parameter work exceeds128Mi");guard_work+=extra;work+=extra*6;parameter_cost=cost;}
    void complete(std::stop_token stop,bool changed){state.dragging=gesture;if(changed){publication=encode_vanishing_point_parameters(state.parameters);auto data=std::make_shared<DocumentData>(*expected);data->layers[target.layer].planes[target.plane].specific=publication;expected=std::move(data);write=true;}poll(stop);}
};
VanishingPointMoveSession::VanishingPointMoveSession(std::shared_ptr<Impl> i):impl_(std::move(i)){}
VanishingPointMoveState VanishingPointMoveSession::state() const{return impl_->state;}
std::shared_ptr<const VanishingPointParameters> VanishingPointMoveSession::primary() const{return impl_->primary;}
bool VanishingPointMoveSession::matches(const Document& d) const noexcept {const auto& i=*impl_;try{
    if(&d!=i.owner||d.state_generation()!=(i.pending?i.generation-1:i.generation)||d.region_selection_generation()!=i.selection_generation||d.grid_generation()!=i.grid_generation||d.guide_generation()!=i.guide_generation||i.target.layer>=d.data.layers.size()||&d.data.layers[i.target.layer]!=i.layer||i.target.plane>=d.data.layers[i.target.layer].planes.size()||&d.data.layers[i.target.layer].planes[i.target.plane]!=i.plane||d.region_selection.mask.get()!=i.mask_identity)return false;
    if(!bits(d.grid.dpi,i.grid.dpi)||d.grid.origin!=i.grid.origin||d.grid.subdivisions!=i.grid.subdivisions||d.grid.enabled!=i.grid.enabled||!length(d.grid.spacing,i.grid.spacing)||!bits(d.guide.dpi,i.guide.dpi)||d.guide.origin!=i.guide.origin||d.guide.enabled!=i.guide.enabled||d.guide.records.size()!=i.guide.records.size())return false;
    for(std::size_t k=0;k<2;++k)if(!length(d.grid.position[k],i.grid.position[k])||!length(d.guide.position[k],i.guide.position[k]))return false;
    for(std::size_t k=0;k<d.guide.records.size();++k)if(!length(d.guide.records[k].position,i.guide.records[k].position)||d.guide.records[k].orientation!=i.guide.records[k].orientation||d.guide.records[k].marker!=i.guide.records[k].marker)return false;
    return data_same(d.data,*(i.pending?i.initial:i.expected))&&selection_same(d.region_selection,i.selection);
}catch(...){return false;}}
std::shared_ptr<VanishingPointMoveSession> VanishingPointMoveSession::prepare(const Document& d,VanishingPointTarget t,std::stop_token stop){
    poll(stop);const auto& p=node(d.data,t);if(d.has_floating_selection())throw std::invalid_argument("Vanishing Move requires settled artwork");
    std::uint64_t n{};data_charge(d.data,n);charge(n,d.guide.records.size()*sizeof(GuideRecord),max_bytes,"Vanishing Guide exceeds512MiB");std::uint64_t mask_work{};
    if(d.region_selection.mask){const auto& m=*d.region_selection.mask;if(m.width()<0||m.height()<0)throw std::invalid_argument("Invalid selection carrier");mask_work=std::uint64_t(m.width())*m.height();if(mask_work>max_work)throw std::length_error("Vanishing mask work exceeds128Mi");charge(n,((std::uint64_t(m.width())+31)/32)*m.height()*4+((std::uint64_t(m.width())+255)/256)*((std::uint64_t(m.height())+255)/256),max_bytes,"Vanishing mask storage exceeds512MiB");}
    if(n>max_bytes/4||n+mask_work*2+1>max_work/6)throw std::length_error("Vanishing source preflight exceeds snapshot/work budget");
    auto params=materialize_vanishing_point_parameters(decode_vanishing_point_parameters(p.specific));validate(params,d.data.base.dpi);const auto param_n=parameter_charge(params);charge(n,param_n*6,max_bytes,"Vanishing parameter aliases exceed512MiB");
    if(n>max_bytes/4)throw std::length_error("Vanishing logical snapshots exceed512MiB");const auto guard=n+mask_work*2+1;if(guard>max_work/6)throw std::length_error("Vanishing admission/finalization work exceeds128Mi");
    auto i=std::make_shared<Impl>();i->owner=&d;i->target=t;i->layer=&d.data.layers[t.layer];i->plane=&p;i->generation=d.state_generation()+1;i->selection_generation=d.region_selection_generation();i->grid_generation=d.grid_generation();i->guide_generation=d.guide_generation();i->grid=d.grid;i->guide=d.guide;i->selection=d.region_selection;i->mask_identity=i->selection.mask.get();if(i->selection.mask)i->selection.mask=std::make_shared<const BinaryMask>(*i->selection.mask);
    i->initial=std::make_shared<const DocumentData>(frozen(d.data,stop));auto expected=std::make_shared<DocumentData>(*i->initial);i->primary=std::make_shared<const VanishingPointParameters>(params);i->state={t,std::move(params),false,0,0};i->publication=encode_vanishing_point_parameters(i->state.parameters);expected->layers[t.layer].planes[t.plane].specific=i->publication;expected->layers[t.layer].planes[t.plane].vanishing_modified=true;i->expected=std::move(expected);i->write=true;i->pending=true;i->guard_work=guard;i->work=guard*6;i->base_charge=n-param_n*6;i->parameter_cost=param_n;poll(stop);return std::shared_ptr<VanishingPointMoveSession>(new VanishingPointMoveSession(std::move(i)));
}
std::shared_ptr<VanishingPointMoveSession> VanishingPointMoveSession::press(const Document& d,VanishingPointSample s,std::span<const VanishingPointPixel> centers,bool alt,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Vanishing source changed");if(impl_->gesture)throw std::logic_error("Vanishing press already active");if(centers.size()!=impl_->state.parameters.points.size())throw std::invalid_argument("Projected center count mismatch");const auto hit=hit_vanishing_points(centers,s.view,alt);auto i=impl_->next(d,stop);i->press_world=s.world;i->first=true;i->index=hit.index;i->gesture=hit.kind!=VanishingPointHitKind::Neutral;bool changed=false;
    if(hit.kind==VanishingPointHitKind::Add){i->admit_parameter(parameter_charge(i->state.parameters)+sizeof(VanishingPoint));i->state.parameters.points.push_back({{double(s.world.x),0,d.data.base.dpi},{double(s.world.y),0,d.data.base.dpi},0});i->index=i->state.parameters.points.size()-1;changed=true;}
    else if(hit.kind==VanishingPointHitKind::Delete){i->state.parameters.points.erase(i->state.parameters.points.begin()+std::ptrdiff_t(*hit.index));i->index.reset();changed=true;}
    if(changed){++i->state.notifications;++i->state.redraws;}i->complete(stop,changed);return std::shared_ptr<VanishingPointMoveSession>(new VanishingPointMoveSession(std::move(i)));
}
std::shared_ptr<VanishingPointMoveSession> VanishingPointMoveSession::move(const Document& d,VanishingPointSample s,std::stop_token stop) const {
    if(!matches(d))throw std::logic_error("Vanishing source changed");auto i=impl_->next(d,stop);bool changed=false;
    if(i->gesture&&i->index){auto& points=i->state.parameters.points;const auto index=*i->index;if(index>=points.size())throw std::logic_error("Vanishing drag index changed");
        if(i->first){i->saved={pixel(points[index].x,d.data.base.dpi),pixel(points[index].y,d.data.base.dpi)};for(auto& q:points)q.selected=0;points[index].selected=1;++i->state.notifications;changed=true;}
        const int dx=integer(std::int64_t(s.world.x)-i->press_world.x),dy=integer(std::int64_t(s.world.y)-i->press_world.y);
        if(dx||dy){const auto x=integer(std::int64_t(i->saved.x)+dx),y=integer(std::int64_t(i->saved.y)+dy);const auto cx=pixel(points[index].x,d.data.base.dpi),cy=pixel(points[index].y,d.data.base.dpi);integer(std::int64_t(x)-cx);integer(std::int64_t(y)-cy);points[index].x={double(x),0,d.data.base.dpi};points[index].y={double(y),0,d.data.base.dpi};++i->state.redraws;changed=true;}i->first=false;
    }i->complete(stop,changed);return std::shared_ptr<VanishingPointMoveSession>(new VanishingPointMoveSession(std::move(i)));
}
std::shared_ptr<VanishingPointMoveSession> VanishingPointMoveSession::release(const Document& d,std::stop_token stop) const {if(!matches(d))throw std::logic_error("Vanishing source changed");auto i=impl_->next(d,stop);i->gesture=false;i->index.reset();if(impl_->gesture)++i->state.redraws;i->complete(stop,false);return std::shared_ptr<VanishingPointMoveSession>(new VanishingPointMoveSession(std::move(i)));}
std::shared_ptr<VanishingPointMoveSession> VanishingPointMoveSession::apply(const Document& d,const VanishingPointParameters& p,std::stop_token stop,bool cancel) const {if(!matches(d))throw std::logic_error("Vanishing source changed");if(impl_->gesture&&!cancel)throw std::logic_error("Release vanishing gesture before settings");const auto cost=parameter_charge(p);auto i=impl_->next(d,stop,cancel);i->admit_parameter(cost);auto params=copied(p,false);validate(params,d.data.base.dpi);i->state.parameters=std::move(params);i->gesture=false;i->index.reset();++i->state.notifications;++i->state.redraws;i->complete(stop,true);return std::shared_ptr<VanishingPointMoveSession>(new VanishingPointMoveSession(std::move(i)));}
void VanishingPointMoveSession::publish(Document& d) noexcept {impl_->pending=false;if(impl_->write){auto& p=d.data.layers[impl_->target.layer].planes[impl_->target.plane];p.specific.swap(impl_->publication);p.vanishing_modified=true;}}
void Document::require_current_vanishing_point_move() const {if(!vanishing_move_)throw std::logic_error("No active Vanishing Move");if(!vanishing_move_->matches(*this))throw std::logic_error("Vanishing Move source changed");}
void Document::begin_vanishing_point_move(VanishingPointTarget t,std::stop_token stop){require_no_floating_transform();vanishing_move_preparing_=true;struct Clear{bool& b;~Clear(){b=false;}} clear{vanishing_move_preparing_};auto s=VanishingPointMoveSession::prepare(*this,t,stop);if(!s->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before Bind");s->publish(*this);vanishing_move_=std::move(s);++state_generation_;}
bool Document::begin_vanishing_point_drag(VanishingPointSample sample,bool alt,std::stop_token stop){require_current_vanishing_point_move();auto centers=vanishing_point_pixels(vanishing_move_->impl_->state.parameters,data.base.dpi);return begin_vanishing_point_drag_projected(sample,centers,alt,stop);}
bool Document::begin_vanishing_point_drag_projected(VanishingPointSample sample,std::span<const VanishingPointPixel> centers,bool alt,std::stop_token stop){require_current_vanishing_point_move();const auto original=vanishing_move_;auto next=original->press(*this,sample,centers,alt,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before press");const bool active=next->impl_->gesture;next->publish(*this);vanishing_move_=std::move(next);++state_generation_;return active;}
bool Document::update_vanishing_point_drag(VanishingPointSample sample,std::stop_token stop){require_current_vanishing_point_move();const auto original=vanishing_move_;auto next=original->move(*this,sample,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before move");const bool changed=next->impl_->state.redraws!=original->impl_->state.redraws;next->publish(*this);vanishing_move_=std::move(next);++state_generation_;return changed;}
void Document::release_vanishing_point_drag(std::stop_token stop){require_current_vanishing_point_move();const auto original=vanishing_move_;auto next=original->release(*this,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before release");vanishing_move_=std::move(next);++state_generation_;}
void Document::apply_vanishing_point_parameters(const VanishingPointParameters& p,std::stop_token stop){require_current_vanishing_point_move();const auto original=vanishing_move_;auto next=original->apply(*this,p,stop);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before settings");next->publish(*this);vanishing_move_=std::move(next);++state_generation_;}
void Document::accept_vanishing_point_move(const std::string& label,std::stop_token stop){require_current_vanishing_point_move();poll(stop);const auto session=vanishing_move_;VanishingEdit edit{session->impl_->target,session->primary(),{},false};Revision prepared{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)};undo_.reserve(undo_.size()+1);if(vanishing_move_!=session||!session->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before accept");undo_.push_back(std::move(prepared));if(undo_.size()>100)undo_.erase(undo_.begin());redo_.clear();undo_next_=true;revision_=next_++;vanishing_move_.reset();++state_generation_;}
void Document::cancel_vanishing_point_move(std::stop_token stop){if(vanishing_move_preparing_)throw std::logic_error("Vanishing Bind preparing");if(!vanishing_move_)return;if(!vanishing_move_->matches(*this)){vanishing_move_.reset();throw std::logic_error("Stale Vanishing Cancel preserves external changes");}const auto original=vanishing_move_;auto next=original->apply(*this,*original->primary(),stop,true);if(!original->matches(*this)||stop.stop_requested())throw std::logic_error("Vanishing changed before Cancel");next->publish(*this);vanishing_move_.reset();++state_generation_;}
void Document::replay_vanishing(std::vector<Revision>& source,std::vector<Revision>& destination){auto edit=std::get<VanishingEdit>(source.back().runtime);const auto& p=node(data,edit.target);if(p.specific.size()>max_bytes/6)throw std::length_error("Vanishing replay aliases exceed512MiB");if(!edit.secondary){auto opposite=copied(decode_vanishing_point_parameters(p.specific),false);validate(opposite,data.base.dpi);edit.secondary=std::make_shared<const VanishingPointParameters>(std::move(opposite));}const auto desired=edit.reverse?edit.secondary:edit.primary;auto applied=*desired;const auto live=decode_vanishing_point_parameters(p.specific);applied.header_suffix=live.header_suffix;applied.points_suffix=live.points_suffix;applied.suffix=live.suffix;auto bytes=encode_vanishing_point_parameters(applied);edit.reverse=!edit.reverse;destination.push_back({DocumentData{},source.back().label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});auto& target=data.layers[std::get<VanishingEdit>(destination.back().runtime).target.layer].planes[std::get<VanishingEdit>(destination.back().runtime).target.plane];target.specific.swap(bytes);target.vanishing_modified=true;}
}
