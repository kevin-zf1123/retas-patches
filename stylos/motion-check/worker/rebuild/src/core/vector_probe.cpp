#include "vector_probe.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas::vector_probe_detail {
namespace {
constexpr std::uint64_t max_work=128ull*1024*1024;
using numeric_detail::binary64;
int integer(double n){
    if(!std::isfinite(n)||n<std::numeric_limits<std::int32_t>::min()||n>std::numeric_limits<std::int32_t>::max())
        throw std::invalid_argument("Vector probe conversion outside signed32 domain");
    return int(n);
}
void validate(VectorFaceVertex p){
    if(std::abs(std::int64_t(p.x))>134217727||std::abs(std::int64_t(p.y))>134217727)
        throw std::invalid_argument("Vector probe point outside bounded raw domain");
}
void validate(const Curve& c){validate(c.start);validate(c.end);if(!c.linear)validate(c.control);}
VectorBounds bounds(const Curve& c){
    auto b=VectorBounds{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),std::max(c.start.x,c.end.x),std::max(c.start.y,c.end.y)};
    if(!c.linear){b.left=std::min(b.left,c.control.x);b.right=std::max(b.right,c.control.x);b.top=std::min(b.top,c.control.y);b.bottom=std::max(b.bottom,c.control.y);}
    ++b.right;++b.bottom;return b;
}
double chord(VectorPoint a,VectorPoint b){const auto x=binary64(a.x-b.x),y=binary64(a.y-b.y);return binary64(std::sqrt(binary64(binary64(x*x)+binary64(y*y))));}
template<class Map> auto live(const Map& m,int id){const auto it=m.find(id);if(it==m.end())throw std::invalid_argument("Dead vector probe graph reference");return it->second;}
}
void Budget::charge(std::uint64_t n) const {
    if(stop.stop_requested())throw std::runtime_error("Vector probe cancelled");
    if(work>max_work||n>max_work-work)throw std::length_error("Vector probe aggregate work limit");work+=n;
}
bool overlap(VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;}
Scope Scope::rectangle(VectorBounds b){Scope out;out.configured=true;out.bounds=b;return out;}
Scope Scope::masked(const BinaryMask& m,double scale,std::array<int,2> origin,const std::optional<Rect>& clip,Budget budget){
    if(!std::isfinite(scale)||scale<=0)throw std::invalid_argument("Invalid vector probe mask scale");
    const auto inverse=binary64(1/scale);if(!std::isfinite(inverse)||inverse<=0)throw std::invalid_argument("Invalid vector probe inverse mask scale");
    if(m.width()<0||m.height()<0||m.width()>100000||m.height()>100000)throw std::invalid_argument("Invalid vector probe mask dimensions");
    budget.charge(((std::uint64_t(m.width())+31)/32)*std::uint64_t(m.height()));
    auto extent=m.extent({0,0,m.width(),m.height()});
    if(clip){if(clip->width<0||clip->height<0)throw std::invalid_argument("Invalid vector probe mask clip");
        const auto left=std::max<std::int64_t>(clip->x,extent.x),top=std::max<std::int64_t>(clip->y,extent.y);
        const auto right=std::min<std::int64_t>(std::int64_t(clip->x)+clip->width,std::int64_t(extent.x)+extent.width),bottom=std::min<std::int64_t>(std::int64_t(clip->y)+clip->height,std::int64_t(extent.y)+extent.height);
        extent=left<right&&top<bottom?Rect{int(left),int(top),int(right-left),int(bottom-top)}:Rect{};}
    Scope out;out.configured=true;out.mask=&m;out.mask_bounds=extent;out.forward=scale;out.inverse=inverse;out.offset=origin;
    const auto edge=[&](std::int64_t n,int axis){return integer(binary64(std::floor(binary64(double(n)*scale))+origin[std::size_t(axis)]));};
    out.bounds={edge(extent.x,0),edge(extent.y,1),edge(std::int64_t(extent.x)+extent.width,0),edge(std::int64_t(extent.y)+extent.height,1)};return out;
}
bool Scope::point(VectorFaceVertex p,Budget budget) const {
    budget.charge();if(!configured)return true;
    if(p.x<bounds.left||p.x>=bounds.right||p.y<bounds.top||p.y>=bounds.bottom)return false;
    if(!mask)return true;
    const auto x=integer(std::floor(binary64(double(std::int64_t(p.x)-offset[0])*inverse)));
    const auto y=integer(std::floor(binary64(double(std::int64_t(p.y)-offset[1])*inverse)));
    return mask->test(x,y); // An absent/out-of-carrier cursor is false.
}
VectorPoint position(const Curve& c,float t,Budget budget){
    budget.charge(16);validate(c);if(!std::isfinite(t))throw std::invalid_argument("Nonfinite vector probe parameter");
    const auto u=binary64(1.0-double(t));
    if(c.linear)return {binary64(binary64(c.start.x*u)+binary64(c.end.x*double(t))),binary64(binary64(c.start.y*u)+binary64(c.end.y*double(t)))};
    const auto uf=double(float(u)),aa=binary64(u*uf),cc=binary64(2*binary64(uf*double(t))),ee=binary64(double(t)*double(t));
    const auto lane=[&](int a,int m,int e){return binary64(binary64(binary64(e*ee)+binary64(m*cc))+binary64(a*aa));};
    return {lane(c.start.x,c.control.x,c.end.x),lane(c.start.y,c.control.y,c.end.y)};
}
VectorFaceVertex integer_position(const Curve& c,float t,Budget budget){const auto p=position(c,t,budget);return {integer(std::floor(p.x)),integer(std::floor(p.y))};}
VectorFaceVertex node_position(const Curve& c,float t,Budget budget){budget.charge();if(!std::isfinite(t))throw std::invalid_argument("Nonfinite vector node parameter");if(t<=0){validate(c.start);return c.start;}if(t>=1){validate(c.end);return c.end;}return integer_position(c,t,budget);}
double four_chord_length(const Curve& c,Budget budget){
    budget.charge(16);validate(c);VectorPoint last{double(c.start.x),double(c.start.y)};
    if(c.linear)return chord(last,{double(c.end.x),double(c.end.y)});
    double sum{};for(float t:{.25f,.5f,.75f,1.f}){const auto p=position(c,t,budget);sum=binary64(sum+chord(last,p));last=p;}return sum;
}
double insertion_length(const Curve& c,Budget budget){
    auto length=four_chord_length(c,budget);if(c.linear)return length;
    const auto n=std::trunc(binary64(length/64));if(n>100000)throw std::length_error("Vector insertion length sampling limit");
    if(n>4){const float h=float(binary64(1/n));VectorPoint last{double(c.start.x),double(c.start.y)};length=0;
        for(std::size_t i=1;i<=std::size_t(n);++i){const auto p=position(c,float(binary64(double(i)*h)),budget);length=binary64(length+chord(last,p));last=p;}}
    return length;
}
std::vector<float> axis_roots(const Curve& c,bool horizontal,int query,bool bias,Budget budget){
    budget.charge(32);validate(c);const auto lane=[&](VectorFaceVertex p){return horizontal?p.y:p.x;};
    const double start=lane(c.start),control=lane(c.control),end=lane(c.end);std::array<double,2> raw{};int count{};
    const auto q=binary64(double(query)+(bias?.125:0));
    if(c.linear){const auto delta=binary64(end-start);if(delta==0){if(start==query){raw={0,1};count=2;}}else{raw[0]=binary64(binary64(q-start)/delta);count=1;}}
    else{const auto a=binary64(binary64(end-2*control)+start),b=binary64(2*binary64(control-start)),k=binary64(start-q);
        if(a==0){if(b!=0){raw[0]=binary64(-k/b);count=1;}}
        else{const auto d=binary64(binary64(b*b)-binary64(4*binary64(a*k)));if(d>=0){
            if(d==0){raw[0]=raw[1]=binary64(-b/binary64(a+a));}
            else{const auto z=binary64(std::sqrt(d)),scale=binary64(.5/a);raw={binary64(binary64(z-b)*scale),binary64(binary64(-b-z)*scale)};}count=2;}}}
    std::vector<float> out;for(int i=0;i<count;++i)if(raw[std::size_t(i)]>=0&&raw[std::size_t(i)]<=1)out.push_back(float(raw[std::size_t(i)]));return out;
}
std::vector<Crossing> rectangle_intersections(const Curve& c,VectorBounds roi,Budget budget){
    budget.charge(32);validate(c);std::vector<Crossing> out;if(c.start==c.end)return out;
    const auto box=bounds(c);if(!overlap(box,roi))return out;
    if(c.linear){
        const auto append=[&](int x,int y){out.push_back({{x,y},0});};
        const auto dx=double(std::int64_t(c.start.x)-c.end.x),dy=double(std::int64_t(c.start.y)-c.end.y);
        if(dx==0){for(auto y:{roi.top,roi.bottom})if(box.top<=y&&box.bottom>y)append(c.start.x,y);}
        else if(dy==0){for(auto x:{roi.left,roi.right})if(box.left<=x&&box.right>x)append(x,c.start.y);}
        else{const auto slope=binary64(dy/dx),inverse=binary64(1/slope);
            for(auto x:{roi.left,roi.right})if(x==roi.left?box.left<x:box.right>x){const auto y=binary64(binary64(double(std::int64_t(x)-c.start.x)*slope)+c.start.y);if(y>=roi.top&&y<=roi.bottom)append(x,integer(std::floor(y)));}
            for(auto y:{roi.top,roi.bottom})if(y==roi.top?box.top<y:box.bottom>y){const auto x=binary64(binary64(double(std::int64_t(y)-c.start.y)*inverse)+c.start.x);if((x>roi.left&&x<roi.right)||((x==roi.left||x==roi.right)&&out.empty()))append(integer(std::floor(x)),y);}}
        return out;
    }
    const auto side=[&](bool horizontal,int query){for(auto t:axis_roots(c,horizontal,query,false,budget)){auto p=integer_position(c,t,budget);
        if(horizontal){if(p.x<=roi.left||p.x>=roi.right)continue;p.y=query;}else{if(p.y<roi.top||p.y>roi.bottom)continue;p.x=query;}out.push_back({p,t});}};
    if(box.left<=roi.left)side(false,roi.left);if(box.right>roi.right)side(false,roi.right);
    if(box.top<=roi.top)side(true,roi.top);if(box.bottom>roi.bottom)side(true,roi.bottom);
    // Native595300 selects the last equal minimum and swaps whole records.
    for(std::size_t i=0;i+1<out.size();++i){auto lowest=i;for(auto j=i+1;j<out.size();++j)if(out[j].parameter<=out[lowest].parameter)lowest=j;if(lowest!=i)std::swap(out[i],out[lowest]);}
    return out;
}
bool curve_matches(const Curve& c,const Scope& scope,bool all,Budget budget){
    budget.charge();if(!scope.configured)return true;validate(c);if(!overlap(bounds(c),scope.bounds))return false;
    for(auto p:{c.start,c.end}){const auto hit=scope.point(p,budget);if(all&&!hit)return false;if(!all&&hit)return true;}
    if(scope.mask){const auto raw=std::trunc(binary64(four_chord_length(c,budget)*scope.inverse));
        if(!std::isfinite(raw)||raw<0||raw>1000000)throw std::length_error("Vector mask probe sample limit");
        const auto n=std::size_t(raw);if(n>1){const float h=float(binary64(1/double(n)));float t{};
            for(std::size_t j=1;j<n;++j){t=float(binary64(double(t)+h));const auto hit=scope.point(integer_position(c,t,budget),budget);if(all&&!hit)return false;if(!all&&hit)return true;}}
        return all;
    }
    if(c.linear&&all)return true;
    if(!c.linear&&all&&scope.point(c.control,budget))return true;
    const auto hits=rectangle_intersections(c,scope.bounds,budget);return all?hits.empty():!hits.empty();
}
Graph::Graph(const VectorLineArchive& a,Budget budget){
    const auto count=a.vertices.size()+a.subcurves.size()+a.models.size();budget.charge(count*(1+std::bit_width(count))*8);
    const auto add=[](auto& dst,const auto& pool){if(pool.size()>100000)throw std::length_error("Vector probe graph count limit");for(const auto& r:pool)if(r.index<0||!dst.emplace(r.index,&r.value).second)throw std::invalid_argument("Ambiguous vector probe graph identity");};
    add(vertices,a.vertices);add(subcurves,a.subcurves);add(models,a.models);
}
Curve Graph::curve(const VectorModelCurve& m,const VectorSubcurve& s,Budget budget) const {budget.charge();const auto a=live(vertices,s.vertex_a),b=live(vertices,s.vertex_b);return {{a->x,a->y},{s.control_x,s.control_y},{b->x,b->y},bool(m.flags&1)};}
bool Graph::model_matches(const VectorModelCurve& m,const Scope& scope,bool all,Budget budget) const {
    budget.charge();if(!scope.configured)return true;const auto& b=m.scalars;if(!overlap({b[0],b[1],b[2],b[3]},scope.bounds))return false;
    for(auto id:m.subcurves){const auto hit=curve_matches(curve(m,*live(subcurves,id),budget),scope,all,budget);if(all&&!hit)return false;if(!all&&hit)return true;}return all;
}
bool Graph::line_matches(const VectorLine& line,const Scope& scope,bool all,Budget budget) const {
    budget.charge();if(!scope.configured)return true;if(!overlap(line.archived_bounds,scope.bounds)||line.members.empty())return false;
    for(auto member:line.members){const auto hit=model_matches(*live(models,member.model),scope,all,budget);if(all&&!hit)return false;if(!all&&hit)return true;}return all;
}
}
