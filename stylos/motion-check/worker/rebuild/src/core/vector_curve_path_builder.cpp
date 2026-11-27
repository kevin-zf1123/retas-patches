#include "retas/vector_curve_path_builder.hpp"
#include "retas/vector_line_curve_path_builder.hpp"
#include "retas/numeric.hpp"
#include "vector_curve_math.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
namespace {
constexpr double raw_limit=1073741824.0;
constexpr std::size_t max_nodes=100000;
constexpr std::uint64_t max_work=128ull*1024*1024;
struct Curve {VectorPoint start,control,end;std::vector<VectorWidthNode> nodes{};};
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector Curve path build cancelled");}
void charge(std::uint64_t& work,std::uint64_t amount){
    if(amount>max_work-work)throw std::length_error("Vector Curve normalization work exceeds limits");work+=amount;
}
void point_gate(VectorPoint p){
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>raw_limit||std::abs(p.y)>raw_limit||std::floor(p.x)!=p.x||std::floor(p.y)!=p.y)
        throw std::invalid_argument("Vector Curve requires bounded integer raw points");
}
std::int64_t floor_div(std::int64_t numerator,std::int64_t divisor){
    const auto quotient=numerator/divisor;return quotient-(numerator<0&&numerator%divisor!=0?1:0);
}
VectorPoint midpoint(VectorPoint a,VectorPoint b){
    const auto component=[](double x,double y){
        const auto sum=std::int64_t(x)+std::int64_t(y);
        if(sum<std::numeric_limits<std::int32_t>::min()||sum>std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("Vector Curve SAR midpoint sum exceeds signed32 domain");
        return double(floor_div(sum,2));
    };
    return {component(a.x,b.x),component(a.y,b.y)};
}
VectorPoint difference(VectorPoint a,VectorPoint b){
    const auto component=[](double x,double y){const double value=x-y;
        if(value<std::numeric_limits<std::int32_t>::min()||value>std::numeric_limits<std::int32_t>::max())
            throw std::invalid_argument("Vector Curve difference exceeds signed32 domain");
        return value;};
    return {component(a.x,b.x),component(a.y,b.y)};
}
float angle(VectorPoint a,VectorPoint b){
    const auto d=difference(b,a);const auto r=numeric_detail::binary64;double value;
    if(d.x>0){value=r(std::atan(r(d.y/d.x))*double(57.295776f));if(value<0)value=r(value+360);}
    else if(d.x<0)value=r(r(std::atan(r(d.y/d.x))*double(57.295776f))+180);
    else value=d.y>=0?90:270;
    return float(value); // 56B600 stores/reloads both returned headings as F32
}
int distance_squared(VectorPoint a,VectorPoint b){
    const auto d=difference(a,b);
    const auto x=std::int64_t(d.x),y=std::int64_t(d.y);
    // Native605A50 saturates at INTMAX; thresholds16/64 need only that domain.
    if(std::abs(x)>46340||std::abs(y)>46340)return std::numeric_limits<int>::max();
    return int(std::min<std::int64_t>(x*x+y*y,std::numeric_limits<int>::max()));
}
int dot(VectorPoint a,VectorPoint b){
    const auto x=std::int64_t(a.x)*std::int64_t(b.x),y=std::int64_t(a.y)*std::int64_t(b.y);
    if(y>0&&x>std::numeric_limits<std::int64_t>::max()-y)return std::numeric_limits<int>::max();
    if(y<0&&x<std::numeric_limits<std::int64_t>::min()-y)return -std::numeric_limits<int>::max();
    return int(std::clamp(x+y,-std::int64_t(std::numeric_limits<int>::max()),std::int64_t(std::numeric_limits<int>::max())));
}
float projected_parameter(VectorPoint start,VectorPoint end,VectorPoint query){
    const auto direction=difference(end,start),offset=difference(query,start);
    const auto numerator=dot(direction,offset);if(numerator<=0)return 0;
    const auto denominator=distance_squared(start,end);if(numerator>=denominator)return 1;
    const auto r=numeric_detail::binary64;
    if(denominator!=std::numeric_limits<int>::max())return float(r(double(numerator)/denominator));
    const double n=r(r(offset.y*direction.y)+r(offset.x*direction.x));
    const double d=r(r(direction.y*direction.y)+r(direction.x*direction.x));
    if(n>=d)return 1;return float(r(n/d));
}
std::optional<VectorPoint> unique_intersection(VectorPoint a,VectorPoint b,VectorPoint c,VectorPoint d,bool require_unique=true,bool path_limit=true){
    // 596CD0(testBounds=0): slope/intercept arithmetic, vertical cases and
    // microgrid quantization. Collinear lines use5961D0's endpoint-in-bbox
    // enumeration rather than a generic infinite-line determinant result.
    if(a==b||c==d)return {};
    const auto first=difference(a,b),second=difference(c,d);const auto r=numeric_detail::binary64;
    const auto collinear=[&]()->std::optional<VectorPoint>{
        const auto contains=[](VectorPoint p,VectorPoint x,VectorPoint y){return p.x>=std::min(x.x,y.x)&&p.x<=std::max(x.x,y.x)&&p.y>=std::min(x.y,y.y)&&p.y<=std::max(x.y,y.y);};
        unsigned count=0;VectorPoint point,first_point;
        const auto include=[&](VectorPoint p){if(!count)first_point=p;point=p;++count;};
        if(contains(a,c,d))include(a);if(contains(b,c,d))include(b);
        if(c!=a&&c!=b&&contains(c,a,b))include(c);if(d!=a&&d!=b&&contains(d,a,b))include(d);
        return require_unique?(count==1?std::optional<VectorPoint>(point):std::nullopt):(count?std::optional<VectorPoint>(first_point):std::nullopt);
    };
    const auto micro=[&](double value){return r(std::floor(r(r(value*1000000.0)+.5))*.000001);};
    VectorPoint point;
    if(first.x==0){
        if(second.x==0)return a.x==c.x?collinear():std::nullopt;
        point={a.x,micro(r(r(difference(a,c).x*r(second.y/second.x))+c.y))};
    }else if(second.x==0){
        point={c.x,micro(r(r(difference(c,a).x*r(first.y/first.x))+a.y))};
    }else{
        const double slope1=r(first.y/first.x),slope2=r(second.y/second.x);
        const double intercept1=r(a.y-r(a.x*slope1)),intercept2=r(c.y-r(c.x*slope2));
        const double denominator=r(slope2-slope1);
        if(denominator==0)return intercept1==intercept2?collinear():std::nullopt;
        const double x=r(r(intercept1-intercept2)/denominator),y=r(r(x*slope1)+intercept1);
        point={micro(x),micro(y)};
    }
    if(!std::isfinite(point.x)||!std::isfinite(point.y)||(path_limit&&(std::abs(point.x)>raw_limit||std::abs(point.y)>raw_limit)))
        throw std::invalid_argument("Vector Curve control intersection exceeds supported domain");
    return point;
}
VectorPoint reattached_control(VectorPoint other,VectorPoint old,VectorPoint replacement,VectorPoint control){
    const auto delta=difference(replacement,old);const auto r=numeric_detail::binary64;
    const VectorPoint shifted{control.x+delta.x,control.y+delta.y};point_gate(shifted);
    if(const auto intersection=unique_intersection(other,control,replacement,shifted)){
        const VectorPoint candidate{std::trunc(intersection->x),std::trunc(intersection->y)};
        const float t=projected_parameter(other,replacement,candidate);
        if(t>.2f&&t<.8f&&distance_squared(other,candidate)<distance_squared(other,old))return candidate;
    }
    const float t=projected_parameter(other,old,control);
    const VectorPoint result{control.x+std::trunc(r(delta.x*double(t))),control.y+std::trunc(r(delta.y*double(t)))};
    point_gate(result);return result;
}
void node_gate(float t,float scale){
    if(!std::isfinite(t)||!std::isfinite(scale)||t<0||t>1||scale<0)
        throw std::invalid_argument("Line/Curve generated knots exceed the bounded native domain");
}
void append_node(std::vector<VectorWidthNode>& nodes,float t,float scale,std::uint64_t& work){
    node_gate(t,scale);charge(work,64);
    if(nodes.size()>=max_nodes)throw std::length_error("Line/Curve knot count exceeds limits");
    nodes.push_back({double(t),double(scale)});
}
std::array<std::vector<VectorWidthNode>,2> split_nodes(std::span<const VectorWidthNode> nodes,float cut,
                                                    std::uint64_t& work,std::stop_token stop){
    // 5A7FD0 retains ordered duplicate parameters. Previous-snap wins over
    // next-snap, even when the following original node is exactly the seam.
    std::array<std::vector<VectorWidthNode>,2> result;auto& left=result[0];auto& right=result[1];
    const auto r=numeric_detail::binary64;const double epsilon=std::bit_cast<float>(std::uint32_t(0x3a83126f));
    const float inverse_left=float(r(1.0/double(cut))),inverse_right=float(r(1.0/r(1.0-double(cut))));
    std::size_t previous=0;
    for(;;){
        poll(stop);const auto& node=nodes[previous];
        append_node(left,float(r(node.parameter*double(inverse_left))),float(node.scale),work);
        if(previous+1>=nodes.size())throw std::invalid_argument("Line/Curve split has no right knot");
        if(nodes[previous+1].parameter>=double(cut))break;
        ++previous;
    }
    std::size_t next=previous+1;const auto& a=nodes[previous];const auto& b=nodes[next];
    float scale;
    if(r(double(cut)-a.parameter)<epsilon&&left.back().parameter>0){
        left.back().parameter=1;scale=float(a.scale);
    }else if(r(b.parameter-double(cut))<epsilon){
        scale=float(b.scale);append_node(left,1,scale,work);++next;
    }else{
        const auto numerator=r(r(r(b.parameter-double(cut))*a.scale)+r(r(double(cut)-a.parameter)*b.scale));
        scale=float(r(numerator/r(b.parameter-a.parameter)));append_node(left,1,scale,work);
    }
    append_node(right,0,scale,work);
    for(;next<nodes.size();++next){poll(stop);const auto& node=nodes[next];
        append_node(right,float(r(r(node.parameter-double(cut))*double(inverse_right))),float(node.scale),work);}
    right.back().parameter=1;
    return result;
}
VectorPoint position(const Curve& curve,float t){
    const auto r=numeric_detail::binary64;const double u=r(1.0-double(t)),uf=double(float(u));
    const double uu=r(u*uf),product=r(uf*double(t)),middle=r(product+product),tt=r(double(t)*double(t));
    const auto coordinate=[&](double a,double c,double e){return r(r(r(e*tt)+r(c*middle))+r(a*uu));};
    return {coordinate(curve.start.x,curve.control.x,curve.end.x),coordinate(curve.start.y,curve.control.y,curve.end.y)};
}
double chord(VectorPoint a,VectorPoint b){const auto r=numeric_detail::binary64;
    const double x=r(a.x-b.x),y=r(a.y-b.y);return r(std::sqrt(r(r(y*y)+r(x*x))));}
double curve_length(const Curve& curve,std::uint64_t& work,std::stop_token stop){
    const auto r=numeric_detail::binary64;double length=0;auto previous=curve.start;
    for(float t:{.25f,.5f,.75f,1.f}){const auto next=t==1?curve.end:position(curve,t);length=r(length+chord(previous,next));previous=next;}
    const double subdivisions=std::trunc(r(length/64.0));
    if(!std::isfinite(subdivisions)||subdivisions>double(max_nodes))throw std::length_error("Line/Curve length sampling exceeds limits");
    if(subdivisions>4){
        const auto n=std::size_t(subdivisions);const float inverse=float(r(1.0/double(n)));length=0;previous=curve.start;
        for(std::size_t i=1;i<=n;++i){poll(stop);charge(work,128);const float t=float(r(double(i)*double(inverse)));
            const auto next=position(curve,t);length=r(length+chord(previous,next));previous=next;}
    }
    return length;
}
std::vector<VectorWidthNode> taper(const Curve& seed,bool linear,float inlet,float outlet,
                                  std::uint64_t& work,std::stop_token stop){
    const auto r=numeric_detail::binary64;const float sum=float(r(double(inlet)+outlet));
    const double length=linear?chord(seed.start,seed.end):curve_length(seed,work,stop);
    float first=0,last=1,effective_inlet=inlet,effective_outlet=outlet;
    if(double(sum)>=length){
        const double inverse=r(1.0/double(sum)),ratio=r(length*inverse);
        first=last=float(r(double(inlet)*inverse));
        effective_inlet=float(r(double(inlet)*ratio));effective_outlet=float(r(ratio*double(outlet)));
    }else{
        if(inlet>0)first=float(r(double(inlet)/length));
        if(outlet>0)last=float(r(1.0-r(double(outlet)/length)));
    }
    const auto count=[&](float effective){const double n=1.0-std::trunc(r(double(effective)*double(-.03125f)));
        if(!std::isfinite(n)||n<1||n>double(max_nodes))throw std::length_error("Line/Curve taper subdivision exceeds limits");return std::size_t(n);};
    std::vector<VectorWidthNode> result;
    const auto append=[&](float t,float scale){if(result.empty()||double(t)>result.back().parameter)append_node(result,t,scale,work);};
    constexpr float half_pi=std::bit_cast<float>(std::uint32_t(0x3fc90fdb));
    if(inlet>0){
        const auto n=count(effective_inlet);const double inverse=r(1.0/double(n));
        const float step=float(r(double(first)*inverse)),angle_step=float(r(inverse*double(half_pi)));
        float parameter=0,angle_value=0;append(0,0);
        for(std::size_t i=1;i<n;++i){poll(stop);parameter=float(r(double(parameter)+step));
            const double angle_next=r(double(angle_value)+angle_step);angle_value=float(angle_next);
            append(parameter,float(std::sin(angle_next)));}
    }
    append(first,1);if(last>first)append(last,1);
    if(outlet>0){
        const auto n=count(effective_outlet);const float step=float(r(r(1.0-double(last))/double(n)));
        const float angle_step=float(r(double(half_pi)/double(n)));float parameter=last,angle_value=half_pi;
        for(std::size_t i=1;i<n;++i){poll(stop);parameter=float(r(double(parameter)+step));
            const double angle_next=r(double(angle_value)-angle_step);angle_value=float(angle_next);
            append(parameter,float(std::sin(angle_next)));}
        append(1,0);
    }
    if(result.size()<2||result.front().parameter!=0||result.back().parameter!=1)
        throw std::invalid_argument("Line/Curve taper did not produce canonical native endpoints");
    return result;
}
std::array<Curve,2> split(const Curve& curve){
    // 5923D0(.5) floors output controls and the unrounded De Casteljau point.
    // At .5 all products are exact binary fractions for this integer domain;
    // int64 rational numerators preserve its floor without an ideal midpoint.
    const auto control=[](VectorPoint a,VectorPoint c){return VectorPoint{
        double(floor_div(std::int64_t(a.x)+std::int64_t(c.x),2)),
        double(floor_div(std::int64_t(a.y)+std::int64_t(c.y),2))};};
    const VectorPoint middle{
        double(floor_div(std::int64_t(curve.start.x)+2*std::int64_t(curve.control.x)+std::int64_t(curve.end.x),4)),
        double(floor_div(std::int64_t(curve.start.y)+2*std::int64_t(curve.control.y)+std::int64_t(curve.end.y),4))};
    return {{{curve.start,control(curve.start,curve.control),middle},
             {middle,control(curve.control,curve.end),curve.end}}};
}
std::vector<Curve> normalize(std::span<const Curve> raw,std::uint64_t& work,std::stop_token stop){
    std::vector<Curve> result;std::size_t live=raw.size(),live_knots=0;
    for(const auto& curve:raw){if(curve.nodes.size()>max_nodes-live_knots)throw std::length_error("Line/Curve aggregate knots exceed limits");live_knots+=curve.nodes.size();}
    for(const auto& original:raw){
        std::vector<Curve> pending{original};
        while(!pending.empty()){
            poll(stop);charge(work,256);auto curve=std::move(pending.back());pending.pop_back();
            const double first=angle(curve.start,curve.control),second=angle(curve.control,curve.end);
            double turn=std::abs(second-first);if(turn>=180)turn=360-turn;
            if(turn<30){result.push_back(curve);continue;}
            auto halves=split(curve);
            if(distance_squared(halves[0].start,halves[0].end)<64||distance_squared(halves[1].start,halves[1].end)<64){
                curve.control=midpoint(curve.start,curve.end);result.push_back(curve);continue;
            }
            if(++live>=max_nodes)throw std::length_error("Vector Curve normalized graph exceeds node limits");
            if(pending.size()>64)throw std::length_error("Vector Curve split stack exceeds limits");
            if(!curve.nodes.empty()){
                auto nodes=split_nodes(curve.nodes,.5f,work,stop);const auto added=nodes[0].size()+nodes[1].size()-curve.nodes.size();
                if(added>max_nodes-live_knots)throw std::length_error("Line/Curve aggregate split knots exceed limits");live_knots+=added;
                halves[0].nodes=std::move(nodes[0]);halves[1].nodes=std::move(nodes[1]);
            }
            pending.push_back(std::move(halves[1]));pending.push_back(std::move(halves[0])); // native left-first recursion
        }
    }
    if(result.empty())return result;
    // 5B9830 is one forward pass. 5B96D0 retains the short span's A vertex,
    // redirects its B adjacency through5A4AF0's control reattachment, then
    // removes the SCurve. Coordinate-equal distinct vertices still reattach.
    std::vector<Curve> pruned;pruned.reserve(result.size());auto retained=result.front().start;bool replaced=false;
    for(auto curve:result){
        poll(stop);charge(work,256);
        if(replaced)curve.control=reattached_control(curve.end,curve.start,retained,curve.control);
        curve.start=retained;
        if(distance_squared(curve.start,curve.end)<=16){replaced=true;continue;}
        pruned.push_back(curve);retained=curve.end;
        replaced=false;
    }
    return pruned;
}
std::vector<std::size_t> partitions(std::span<const Curve> curves,float width,std::uint64_t& work,std::stop_token stop){
    const float threshold=float(std::min(177.0,numeric_detail::binary64(double(width)*.0625+150.0)));
    std::vector<std::size_t> starts{0};
    for(std::size_t i=1;i<curves.size();++i){
        poll(stop);charge(work,128);const auto joint=curves[i].start;
        const auto previous=curves[i-1].control==joint?curves[i-1].start:curves[i-1].control;
        const auto next=curves[i].control==joint?curves[i].end:curves[i].control;
        const double first=angle(joint,previous),second=angle(joint,next);
        double gap=second-first;if(gap<0)gap+=360;if(gap>180)gap=360-gap;
        // 5BA1F0/5B9B70 cut at the existing vertex. Fresh flags0/2, control
        // points and ordering remain unchanged; no second normalization runs.
        if(gap<threshold)starts.push_back(i);
    }
    return starts;
}
bool flat(const Curve& curve){
    // 551C00 ->595960: distance from C to the supporting A/E line <=2,
    // using605A50's saturated squared chord, not a stable Euclidean norm.
    const auto chord=difference(curve.end,curve.start),offset=difference(curve.control,curve.start);
    const auto denominator=distance_squared(curve.start,curve.end);const auto r=numeric_detail::binary64;
    if(denominator<=0)return distance_squared(curve.start,curve.control)<=4;
    const auto cross=r(r(offset.y*chord.x)-r(chord.y*offset.x));
    return r(r(cross*cross)/denominator)<=4;
}
struct CornerHalf {std::uint32_t kind{};VectorPoint first,control0,middle,control1,last;};
VectorPoint floor_offset(VectorPoint center,double x,double y){
    const VectorPoint result{center.x+std::floor(x),center.y+std::floor(y)};point_gate(result);return result;
}
CornerHalf corner_half(VectorPoint center,float first,float second,double first_width,double second_width){
    const auto r=numeric_detail::binary64;
    float gap=float(r(double(second)-first));if(gap<0)gap=float(r(double(gap)+360));
    const float radius=float(std::max(0.0,r(first_width-.0625))),other_radius=float(std::max(0.0,r(second_width-.0625)));
    const auto direction=[&](float degrees){const auto radians=r(double(degrees)*double(0.017453292f));
        return VectorPoint{float(std::cos(radians)),float(std::sin(radians))};};
    const auto a=direction(float(r(double(first)+180))),b=direction(float(r(double(second)+180)));
    const float ax=float(r(a.y*radius)),ay=float(-r(a.x*radius));
    const float bx=float(-r(b.y*other_radius)),by=float(r(b.x*other_radius));
    CornerHalf result;result.first=floor_offset(center,ax,ay);result.last=floor_offset(center,bx,by);
    if(gap<185||(std::abs(ax)<1&&std::abs(ay)<1)||(std::abs(bx)<1&&std::abs(by)<1))return result;
    // Fresh quadratic Models0/2 have both style bits4/8 clear. Every remaining angular
    // gap, including >=210 or >=330, therefore follows native kind2.
    result.kind=2;
    const auto average=r(r(double(radius)+other_radius)*.5);
    const auto x=r(a.x+b.x),y=r(a.y+b.y);
    const auto inverse=r(1/std::sqrt(r(r(x*x)+r(y*y))));
    const float ux=float(r(inverse*x));const auto uy=r(inverse*y);
    const float mx=float(r(double(ux)*average)),my=float(r(uy*average));
    const float d1=float(r(r(double(ux)*a.y)-r(uy*a.x)));
    const float d2=float(r(r(uy*b.x)-r(double(ux)*b.y)));
    const auto control=[&](float px,float py,float d,float local_radius){
        double cx,cy;
        if(d==0||(average>r(double(d)*local_radius)&&average<r(double(local_radius)/d))){
            const auto numerator=r(r(r(double(py)-my)*py)+r(r(double(px)-mx)*px));
            const auto denominator=r(r(double(mx)*py)-r(double(my)*px));
            const auto q=r(numerator/denominator);
            cx=r(double(mx)-r(double(my)*q));cy=r(r(q*mx)+my);
        }else{cx=r(r(double(mx)+px)*.5);cy=r(r(double(my)+py)*.5);}
        return floor_offset(center,float(cx),float(cy));
    };
    result.control0=control(ax,ay,d1,radius);result.middle=floor_offset(center,mx,my);
    result.control1=control(bx,by,d2,other_radius);return result;
}
void insert_first(VectorEndpointProfile& profile,const CornerHalf& half){
    profile.first_kind=half.kind;profile.point0=half.first;
    if(half.kind==2){profile.control0=half.control0;profile.middle=half.middle;}
}
void insert_second(VectorEndpointProfile& profile,const CornerHalf& half){
    profile.second_kind=half.kind;profile.point2=half.last;
    if(half.kind==2){profile.control1=half.control1;profile.middle=half.middle;}
}
std::vector<VectorSpanProfiles> profiles(std::span<const Curve> curves,std::span<const std::size_t> starts,
                                       float width,std::uint64_t& work,std::stop_token stop){
    std::vector<VectorSpanProfiles> result;
    for(std::size_t k=1;k<starts.size();++k){
        poll(stop);charge(work,4096);const auto i=starts[k];const auto center=curves[i].start;
        const auto& previous=curves[i-1];const auto& next=curves[i];
        const float a=angle(center,flat(previous)?previous.start:previous.control);
        const float b=angle(center,flat(next)?next.end:next.control);
        // 5ABF20 inserts map<float,SCurve*> in adjacency order previous,next.
        // Equal keys overwrite with the latter SCurve, leaving count1. The
        // fresh Model descriptor slots then remain null; do not invent a turn.
        if(a==b)continue;
        const bool previous_first=a<b;
        // 59E8C0 leaves H*endpointScale in PC53; 5B5290 subtracts.0625
        // before the F32 radius store. Do not round the product first.
        const auto r=numeric_detail::binary64;
        const double pw=r(double(width)*(previous.nodes.empty()?1:previous.nodes.back().scale));
        const double nw=r(double(width)*(next.nodes.empty()?1:next.nodes.front().scale));
        const auto first=corner_half(center,std::min(a,b),std::max(a,b),previous_first?pw:nw,previous_first?nw:pw);
        const auto last=corner_half(center,std::max(a,b),std::min(a,b),previous_first?nw:pw,previous_first?pw:nw);
        if(!first.kind&&!last.kind)continue;
        if(result.empty())result.resize(curves.size());
        VectorEndpointProfile before,after;
        if(previous_first){insert_first(before,first);insert_second(before,last);insert_first(after,last);insert_second(after,first);}
        else{insert_first(after,first);insert_second(after,last);insert_first(before,last);insert_second(before,first);}
        result[i-1].end=before;result[i].start=after;
    }
    return result;
}
VectorBounds curve_bounds(const Curve& curve,int expansion){
    const double left=std::min({curve.start.x,curve.control.x,curve.end.x})-expansion;
    const double top=std::min({curve.start.y,curve.control.y,curve.end.y})-expansion;
    const double right=std::max({curve.start.x,curve.control.x,curve.end.x})+1+expansion;
    const double bottom=std::max({curve.start.y,curve.control.y,curve.end.y})+1+expansion;
    for(double edge:{left,top,right,bottom})if(std::abs(edge)>raw_limit)throw std::invalid_argument("Vector Curve bounds exceed raw coordinate limits");
    return {int(left),int(top),int(right),int(bottom)};
}
VectorBounds united(VectorBounds a,VectorBounds b){return {std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};}
VectorBounds with_profile(VectorBounds bounds,const std::optional<VectorEndpointProfile>& profile){
    if(!profile)return bounds;
    const auto include=[&](VectorPoint p){point_gate(p);if(p.x+1>raw_limit||p.y+1>raw_limit)
            throw std::invalid_argument("Vector Curve profile bounds exceed raw coordinate limits");
        bounds=united(bounds,{int(p.x),int(p.y),int(p.x)+1,int(p.y)+1});};
    include(profile->point0);include(profile->point2);
    if(profile->first_kind==2){include(profile->control0);include(profile->middle);}
    if(profile->second_kind==2){include(profile->control1);include(profile->middle);}
    return bounds;
}
int checked_inflation(double product,int margin){
    const double value=std::trunc(product)+margin;
    if(!std::isfinite(value)||value<0||value>raw_limit)throw std::invalid_argument("Line/Curve width inflation exceeds raw limits");return int(value);
}
int inflation(const Curve& curve,float width,unsigned flags,int margin){
    double maximum=(flags&2)?1:0;for(const auto& node:curve.nodes)maximum=std::max(maximum,node.scale);
    return checked_inflation(numeric_detail::binary64(double(width)*maximum),margin);
}
VectorBounds expanded(VectorBounds bounds,int radius){
    for(double edge:{double(bounds.left)-radius,double(bounds.top)-radius,double(bounds.right)+radius,double(bounds.bottom)+radius})
        if(std::abs(edge)>raw_limit)throw std::invalid_argument("Line/Curve stroke bounds exceed raw limits");
    return {bounds.left-radius,bounds.top-radius,bounds.right+radius,bounds.bottom+radius};
}
VectorCurvePathContour emit(std::span<const Curve> curves,std::span<const std::size_t> starts,std::vector<VectorSpanProfiles> decorations,
                           float width,std::uint64_t color,unsigned flags,std::uint64_t& work,std::stop_token stop){
    if(curves.size()>=max_nodes)throw std::length_error("Vector Curve final graph exceeds vertex limits");
    VectorCurvePathContour result;auto& geometry=result.geometry;auto& archive=geometry.archive;
    const auto n=curves.size(),model_count=starts.size();archive.high_water={int(n),-1,int(n)-1,int(model_count)-1,-1,0};
    archive.vertices.reserve(n+1);archive.subcurves.reserve(n);archive.models.reserve(model_count);
    VectorLineStroke stroke;stroke.color=color;stroke.spans.reserve(n);
    VectorLine line;line.color=color;line.half_width_bits=std::bit_cast<std::uint32_t>(width);line.members.reserve(model_count);
    for(std::size_t i=0;i<=n;++i){
        poll(stop);charge(work,64);const auto point=i?curves[i-1].end:curves.front().start;
        VectorVertex vertex;vertex.x=int(point.x);vertex.y=int(point.y);
        if(i)vertex.subcurves.push_back(int(i-1));if(i<n)vertex.subcurves.push_back(int(i));
        archive.vertices.push_back({int(i),std::move(vertex),{}});
    }
    for(std::size_t m=0;m<model_count;++m){
        poll(stop);charge(work,128);const auto begin=starts[m],end=m+1<model_count?starts[m+1]:n;
        VectorModelCurve model;model.flags=flags;model.line=0;model.targets={-1,-1};model.crosspoints={-1,-1};model.subcurves.reserve(end-begin);
        auto center_bounds=curve_bounds(curves[begin],0);double maximum=(flags&2)?1:0;
        for(std::size_t i=begin;i<end;++i)for(const auto& node:curves[i].nodes){poll(stop);maximum=std::max(maximum,node.scale);}
        for(std::size_t i=begin;i<end;++i){
            poll(stop);charge(work,64);const auto& curve=curves[i];VectorLineSpan span;
            span.model_index=int(m);span.subcurve_index=int(i);span.start=curve.start;span.control=curve.control;span.end=curve.end;
            span.linear=(flags&1)!=0||curve.control==curve.start||curve.control==curve.end;
            span.fixed_width=(flags&2)!=0;span.width_nodes=curve.nodes;span.half_width=width;span.round_start=i==0;span.round_end=i+1==n;
            span.bounds=curve_bounds(curve,inflation(curve,width,flags,1));
            if(!decorations.empty())span.bounds=with_profile(with_profile(span.bounds,decorations[i].start),decorations[i].end);
            stroke.spans.push_back(span);
            VectorSubcurve subcurve;subcurve.vertex_a=int(i);subcurve.vertex_b=int(i+1);subcurve.control_x=int(curve.control.x);subcurve.control_y=int(curve.control.y);subcurve.model=int(m);
            for(const auto& node:curve.nodes){poll(stop);charge(work,64);if(archive.scales.size()>=max_nodes)throw std::length_error("Line/Curve scale pool exceeds limits");
                const auto id=int(archive.scales.size());archive.scales.push_back({id,{std::bit_cast<std::uint32_t>(float(node.scale)),std::bit_cast<std::uint32_t>(float(node.parameter))},{}});
                subcurve.scales.push_back(id);}
            archive.subcurves.push_back({int(i),std::move(subcurve),{}});model.subcurves.push_back(int(i));
            center_bounds=united(center_bounds,curve_bounds(curve,0));
        }
        // 59E5E0 keeps the center rectangle independent of decoration. Only
        // the stroke rectangle unions active endpoint profile points/controls.
        const auto radius=checked_inflation(numeric_detail::binary64(double(width)*maximum),0);
        auto stroke_bounds=expanded(center_bounds,radius);
        if(!decorations.empty())stroke_bounds=with_profile(with_profile(stroke_bounds,decorations[begin].start),decorations[end-1].end);
        model.scalars={center_bounds.left,center_bounds.top,center_bounds.right,center_bounds.bottom,
                       stroke_bounds.left,stroke_bounds.top,stroke_bounds.right,stroke_bounds.bottom};
        if(m==0)line.archived_bounds=stroke_bounds;else line.archived_bounds=united(line.archived_bounds,stroke_bounds);
        archive.models.push_back({int(m),std::move(model),{}});line.members.push_back({int(m),1});
    }
    archive.high_water[1]=int(archive.scales.size())-1;
    archive.lines.push_back({0,std::move(line),{}});geometry.strokes.push_back(std::move(stroke));result.profiles=std::move(decorations);
    // Identity-open endpoints remain distinct even for coordinate-closed input.
    // Fresh Models have no retained trims, and Curve never invokes5B4470 seam.
    poll(stop);return result;
}
VectorCurvePathContour build_curve(std::span<const VectorPoint> input,float width,std::uint64_t color,bool closed,std::stop_token stop){
    poll(stop);
    if(input.size()>max_nodes)throw std::length_error("Vector Curve input count exceeds limits");
    if(input.size()<2||!std::isfinite(width)||width<0||width>65536)
        throw std::invalid_argument("Vector Curve requires 2..100000 points and bounded native half-width");
    std::uint64_t work=0;for(auto p:input){poll(stop);point_gate(p);charge(work,64);}
    const auto first_mid=midpoint(input[0],input[1]);
    if(closed&&input.size()==2){poll(stop);return {};}
    std::vector<Curve> raw;raw.reserve(input.size());
    auto current=first_mid;
    if(!closed)raw.push_back({input[0],midpoint(input[0],first_mid),first_mid});
    for(std::size_t i=2;i<input.size();++i){
        poll(stop);const auto next=midpoint(input[i-1],input[i]);
        if(next!=current){raw.push_back({current,input[i-1],next});current=next;}
    }
    if(closed){
        const auto last=midpoint(input.back(),input.front());
        if(last!=current){raw.push_back({current,input.back(),last});current=last;}
        if(current!=first_mid)raw.push_back({current,input.front(),first_mid});
    }else if(current!=input.back())raw.push_back({current,midpoint(current,input.back()),input.back()});
    auto curves=normalize(raw,work,stop);if(curves.empty()){poll(stop);return {};}
    const auto starts=partitions(curves,width,work,stop);
    auto decorations=profiles(curves,starts,width,work,stop);
    return emit(curves,starts,std::move(decorations),width,color,2,work,stop);
}
Curve initial_line_curve_seed(const VectorLineCurveSeed& seed,const VectorLineCurveMaterial& material,std::uint64_t& work,std::stop_token stop){
    poll(stop);const bool linear=seed.kind==VectorLineCurveKind::Line;
    if(!linear&&seed.kind!=VectorLineCurveKind::Quadratic)throw std::invalid_argument("Invalid Line/Curve seed kind");
    point_gate(seed.start);point_gate(seed.end);(void)difference(seed.start,seed.end);
    if(!std::isfinite(material.raw_half_width)||material.raw_half_width<0||material.raw_half_width>65536||
       !std::isfinite(material.raw_inlet_length)||material.raw_inlet_length<0||material.raw_inlet_length>raw_limit||
       !std::isfinite(material.raw_outlet_length)||material.raw_outlet_length<0||material.raw_outlet_length>raw_limit)
        throw std::invalid_argument("Line/Curve seed material exceeds bounded native domain");
    Curve initial{seed.start,linear?midpoint(seed.start,seed.end):seed.control,seed.end};
    point_gate(initial.control);(void)difference(initial.control,initial.start);(void)difference(initial.end,initial.control);
    const bool fixed=material.raw_inlet_length==0&&material.raw_outlet_length==0;
    if(!fixed)initial.nodes=taper(initial,linear,material.raw_inlet_length,material.raw_outlet_length,work,stop);
    return initial;
}
} // namespace
VectorCurvePathContour build_vector_curve_path_contour(std::span<const VectorPoint> input,float width,std::uint64_t color,bool closed,std::stop_token stop){
    poll(stop);if(!std::isfinite(width)||width<=0||width>65536)
        throw std::invalid_argument("Vector Curve requires positive bounded native half-width");
    return build_curve(input,width,color,closed,stop);
}
VectorLineGeometry build_vector_curve_path_centerline(std::span<const VectorPoint> input,bool closed,std::stop_token stop){
    return build_curve(input,0,0,closed,stop).geometry;
}
VectorCurvePathContour build_vector_line_curve_contour(const VectorLineCurveSeed& seed,const VectorLineCurveMaterial& material,std::stop_token stop){
    const bool linear=seed.kind==VectorLineCurveKind::Line;
    const bool fixed=material.raw_inlet_length==0&&material.raw_outlet_length==0;std::uint64_t work=0;
    auto initial=initial_line_curve_seed(seed,material,work,stop);
    const std::array<Curve,1> raw{std::move(initial)};
    auto curves=linear?std::vector<Curve>(raw.begin(),raw.end()):normalize(raw,work,stop);
    if(curves.empty()){poll(stop);return {};}
    const auto starts=linear?std::vector<std::size_t>{0}:partitions(curves,material.raw_half_width,work,stop);
    auto decorations=linear?std::vector<VectorSpanProfiles>{}:profiles(curves,starts,material.raw_half_width,work,stop);
    return emit(curves,starts,std::move(decorations),material.raw_half_width,material.runtime_color,
                (linear?1u:0u)|(fixed?2u:0u),work,stop);
}

namespace vector_curve_detail {
VectorLineGeometry build_line_curve_initial_seed(const VectorLineCurveSeed& seed,const VectorLineCurveMaterial& material,std::stop_token stop){
    std::uint64_t work=0;return build_line_curve_initial_seed(seed,material,work,stop);
}
VectorLineGeometry build_line_curve_initial_seed(const VectorLineCurveSeed& seed,const VectorLineCurveMaterial& material,std::uint64_t& work,std::stop_token stop){
    auto initial=initial_line_curve_seed(seed,material,work,stop);
    const std::array<Curve,1> curves{std::move(initial)};const std::array<std::size_t,1> starts{0};
    const auto flags=(seed.kind==VectorLineCurveKind::Line?1u:0u)|((material.raw_inlet_length==0&&material.raw_outlet_length==0)?2u:0u);
    return emit(curves,starts,{},material.raw_half_width,material.runtime_color,flags,work,stop).geometry;
}
float heading(VectorPoint a,VectorPoint b){return angle(a,b);}
std::optional<VectorPoint> first_infinite_intersection(VectorPoint a,VectorPoint b,VectorPoint c,VectorPoint d){return unique_intersection(a,b,c,d,false,false);}
VectorPoint reattach(VectorPoint other,VectorPoint old,VectorPoint replacement,VectorPoint control){return reattached_control(other,old,replacement,control);}
}

}
