#include "vector_curve_query.hpp"
#include "vector_graph_edit.hpp"

namespace retas::vector_query_detail {
namespace {
using namespace vector_graph_detail;
int sar(int a,int b){const auto n=std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b));return n>=0?n/2:-int((-(std::int64_t)n+1)/2);}
VectorBounds box(const Curve& c){VectorBounds b{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),integer(double(std::max(c.start.x,c.end.x))+1),integer(double(std::max(c.start.y,c.end.y))+1)};if(!c.linear){b.left=std::min(b.left,c.control.x);b.top=std::min(b.top,c.control.y);b.right=std::max(b.right,integer(double(c.control.x)+1));b.bottom=std::max(b.bottom,integer(double(c.control.y)+1));}return b;}
bool contains(VectorBounds b,P p){return p.x>=b.left&&p.x<b.right&&p.y>=b.top&&p.y<b.bottom;}
bool coarse(const Curve& a,const Curve& b){if(!overlap(box(a),box(b)))return false;
    const auto side=[](P a,P b,P p){return (std::int64_t(p.y)-a.y)*(std::int64_t(a.x)-b.x)-(std::int64_t(p.x)-a.x)*(std::int64_t(a.y)-b.y);};const auto same=[](std::int64_t a,std::int64_t b){return (a<0&&b<0)||(a>0&&b>0);};
    if(a.start.x==a.end.x){if((std::int64_t(b.start.x)-a.start.x)*(std::int64_t(b.end.x)-a.start.x)>0)return false;}else if(same(side(a.start,a.end,b.start),side(a.start,a.end,b.end)))return false;
    if(b.start.x==b.end.x){const auto x=std::uint32_t(a.start.x)-std::uint32_t(b.start.x),y=std::uint32_t(a.end.x)-std::uint32_t(b.start.x);if(std::bit_cast<std::int32_t>(x*y)>0)return false;}else if(same(side(b.start,b.end,a.start),side(b.start,b.end,a.end)))return false;return true;
}
std::vector<VectorPoint> line_points(const Curve& a,const Curve& b){if(a.start==a.end||b.start==b.end||!coarse(a,b))return {};const auto collinear=[&](){std::vector<VectorPoint> out;const auto add=[&](P p){out.push_back({double(p.x),double(p.y)});};if(contains(box(b),a.start))add(a.start);if(contains(box(b),a.end))add(a.end);if(b.start!=a.start&&b.start!=a.end&&contains(box(a),b.start))add(b.start);if(b.end!=a.start&&b.end!=a.end&&contains(box(a),b.end))add(b.end);return out;};
    const double dx=double(std::int64_t(a.start.x)-a.end.x),dy=double(std::int64_t(a.start.y)-a.end.y),ex=double(std::int64_t(b.start.x)-b.end.x),ey=double(std::int64_t(b.start.y)-b.end.y);const auto micro=[](double v){return binary64(std::floor(binary64(binary64(v*1000000)+.5))*.000001);};VectorPoint p;
    if(dx==0){if(ex==0)return a.start.x==b.start.x?collinear():std::vector<VectorPoint>{};p={double(a.start.x),micro(binary64(binary64(binary64(ey/ex)*double(std::int64_t(a.start.x)-b.start.x))+b.start.y))};}
    else if(ex==0)p={double(b.start.x),micro(binary64(binary64(binary64(dy/dx)*double(std::int64_t(b.start.x)-a.start.x))+a.start.y))};
    else{const auto first=binary64(dy/dx),second=binary64(ey/ex),ai=binary64(a.start.y-binary64(a.start.x*first)),bi=binary64(b.start.y-binary64(b.start.x*second)),d=binary64(second-first);if(d==0)return ai==bi?collinear():std::vector<VectorPoint>{};const auto x=binary64(binary64(ai-bi)/d),y=binary64(binary64(x*first)+ai);p={micro(x),micro(y)};}return {p};
}
std::optional<float> parameter(const Curve& c,VectorPoint p){const auto dx=std::int64_t(c.end.x)-c.start.x,dy=std::int64_t(c.end.y)-c.start.y;const bool horizontal=std::abs(dx)>std::abs(dy);const auto d=horizontal?dx:dy;if(!d)return {};const float t=f32(binary64(binary64((horizontal?p.x:p.y)-(horizontal?c.start.x:c.start.y))/double(d)));return t>=0&&t<=1?std::optional<float>{t}:std::nullopt;}
std::vector<Hit> line_cross(const Curve& a,const Curve& b,Budget budget){std::vector<Hit> out;for(auto p:line_points(a,b)){budget.charge(128);const auto x=parameter(a,p),y=parameter(b,p);if(!x||!y)return {};out.push_back({{integer(std::floor(p.x)),integer(std::floor(p.y))},*x,*y});}return out;}
std::array<Curve,2> split(const Curve& c,float q){const auto u=binary64(1-double(q));const auto mix=[&](double a,double z){return binary64(binary64(a*u)+binary64(z*q));};const VectorPoint l{mix(c.start.x,c.control.x),mix(c.start.y,c.control.y)},r{mix(c.control.x,c.end.x),mix(c.control.y,c.end.y)};const P p{integer(std::floor(mix(l.x,r.x))),integer(std::floor(mix(l.y,r.y)))};return {{{c.start,{integer(std::floor(l.x)),integer(std::floor(l.y))},p,false},{p,{integer(std::floor(r.x)),integer(std::floor(r.y))},c.end,false}}};}
bool flat(const Curve& c){const int d=sq(c.start,c.end);if(!d)return sq(c.start,c.control)<=4;const auto x=double(std::int64_t(c.end.x)-c.start.x),y=double(std::int64_t(c.end.y)-c.start.y),u=double(std::int64_t(c.control.x)-c.start.x),v=double(std::int64_t(c.control.y)-c.start.y);const auto z=binary64(binary64(v*x)-binary64(u*y));return binary64(binary64(z*z)/d)<=4;}
struct Piece {Curve curve;float lo{},hi{1};VectorBounds bounds;bool straight{};};
Piece piece(Curve c,float lo,float hi,bool force=false){const auto b=box(c);return {c,lo,hi,b,force||flat(c)};}
std::vector<Piece> pieces(const Curve& c,Budget budget,bool native=false){if(c.linear){const Curve q{c.start,{sar(c.start.x,c.end.x),sar(c.start.y,c.end.y)},c.end,false};return {piece(q,0,1,true)};}
    const auto root=[&](const Curve& q,bool x)->std::optional<float>{const auto a=x?q.start.x:q.start.y,b=x?q.end.x:q.end.y,m=x?q.control.x:q.control.y;const auto word=[](std::int64_t n){return std::bit_cast<std::int32_t>(std::uint32_t(n));};const auto den=native?std::int64_t(word(std::int64_t(a)+b-2*std::int64_t(m))):std::int64_t(a)+b-2*std::int64_t(m);if(!den)return {};const auto numerator=native?std::int64_t(word(std::int64_t(a)-m)):std::int64_t(a)-m;const auto t=binary64(double(numerator)/double(den));const auto saved=f32(t);if(t<=0||(x?t>=1:saved>=1))return {};return saved;};
    struct Initial{Curve c;float width;};std::vector<Initial> initial;if(const auto x=root(c,true)){const auto two=split(c,*x);initial={{two[0],*x},{two[1],f32(binary64(1-double(*x)))}};}else initial={{c,1}};
    std::vector<Piece> out;float previous{};for(auto p:initial){budget.charge(256);const auto end=f32(binary64(double(p.width)+previous));if(const auto y=root(p.c,false)){const auto two=split(p.c,*y);const auto at=f32(binary64(binary64(double(*y)*p.width)+previous));out.push_back(piece(two[0],previous,at));out.push_back(piece(two[1],at,end));}else out.push_back(piece(p.c,previous,end));previous=end;}return out;
}
float remap(const Piece& p,float t){return f32(binary64(binary64(binary64(double(p.hi)-p.lo)*t)+p.lo));}
//596730 is a temporary linear projection. Unlike the general persisted-raw
// sampler, it admits the already certified x4 operands used by593A70 mode1.
Nearest native_line_nearest(const Curve& c,P p,Budget budget){
    budget.charge(128);const auto t=project(c.start,c.end,p);
    if(t<=0)return {c.start,t};if(t>=1)return {c.end,t};
    budget.charge(16);const auto u=binary64(1-double(t));
    const auto lane=[&](int a,int z){return integer(std::floor(binary64(binary64(a*u)+binary64(z*double(t)))));};
    return {{lane(c.start.x,c.end.x),lane(c.start.y,c.end.y)},t};
}
std::vector<Hit> quad_cross(const Curve& target,const Curve& probe,Budget budget,bool native=false){auto stack=pieces(target,budget,native),peers=pieces(probe,budget,native);std::vector<Hit> out;std::uint64_t allocated=stack.size()+peers.size();const auto add=[&](Piece p,std::vector<Piece>& v){budget.charge(256);if(++allocated>262144)throw std::length_error("Vector CROSS piece allocation limit");v.push_back(std::move(p));};
    while(!stack.empty()){budget.charge(128);const auto a=stack.back();stack.pop_back();bool split_a{},split_b{};
        for(std::size_t i=0;i<peers.size();++i){budget.charge(128);const auto b=peers[i];if(!overlap(a.bounds,b.bounds))continue;
            if(a.straight&&b.straight){const Curve al{a.curve.start,{},a.curve.end,true},bl{b.curve.start,{},b.curve.end,true};const auto hits=line_cross(al,bl,budget);for(auto h:hits){if(out.size()>=8192)throw std::length_error("Vector CROSS hit limit");out.push_back({h.point,remap(a,h.target_parameter),remap(b,h.probe_parameter)});}if(!out.empty())continue;
                const auto near=[&](const Curve& c,P p){return native?native_line_nearest(c,p,budget):nearest(c,p,budget);};const auto first=near(bl,al.start);if(sq(first.point,al.start)<=4){out.push_back({al.start,a.lo,remap(b,first.parameter)});continue;}const auto last=near(bl,al.end);if(sq(last.point,al.end)<=4){out.push_back({al.end,a.hi,remap(b,last.parameter)});continue;}const auto bf=near(al,bl.start);if(sq(bf.point,bl.start)<=4){out.push_back({bf.point,remap(a,bf.parameter),b.lo});continue;}const auto be=near(al,bl.end);if(sq(be.point,bl.end)<=4)out.push_back({be.point,remap(a,be.parameter),b.hi});continue;
            }
            if(!a.straight){split_a=true;if(b.straight)continue;}split_b=true;const auto two=split(b.curve,.5f);const float seam=f32(binary64(binary64(double(b.lo)+b.hi)*.5));peers.erase(peers.begin()+std::ptrdiff_t(i));add(piece(two[0],b.lo,seam),peers);add(piece(two[1],seam,b.hi),peers);break;
        }
        if(split_a){const auto two=split(a.curve,.5f);const float seam=f32(binary64(binary64(double(a.lo)+a.hi)*.5));add(piece(two[0],a.lo,seam),stack);add(piece(two[1],seam,a.hi),stack);}else if(split_b)add(a,stack);
    }return out;
}
// Certification for recovered numeric leaves. Scale first with native DWORD
// semantics, then bound consumed operands so signed products remain defined.
constexpr std::int64_t native_limit=536870908;
void admit(const Curve& c,Budget budget){
    const auto point=[&](P p){budget.charge(2);if(std::abs(std::int64_t(p.x))>native_limit||std::abs(std::int64_t(p.y))>native_limit)throw std::invalid_argument("Vector native CROSS coordinate exceeds bounded domain");};
    point(c.start);point(c.end);if(!c.linear)point(c.control);
}
Curve scaled(Curve c){
    const auto point=[](P p){const auto shl=[](int n){return std::bit_cast<std::int32_t>(std::uint32_t(n)<<2);};return P{shl(p.x),shl(p.y)};};
    c.start=point(c.start);c.end=point(c.end);if(!c.linear)c.control=point(c.control);return c;
}
int sar2(int n){return n>=0?n/4:-int((-(std::int64_t)n+3)/4);}
}
Nearest nearest(const Curve& c,P p,Budget budget){budget.charge(128);if(c.linear){const auto t=project(c.start,c.end,p);return {node_position(c,t,budget),t};}
    if(dot(c.start,c.control,p)<=0)return {c.start,0};if(dot(c.end,c.control,p)<=0)return {c.end,1};const auto n=std::max(2.,std::trunc(binary64(four_chord_length(c,budget)*.015625)));if(n>1000000)throw std::length_error("Vector closest point sample limit");const auto h=f32(binary64(1/n));P previous=c.start;float previous_t{};Nearest out{c.start,0};int best=INT32_MAX;
    for(std::size_t i=1;i<=std::size_t(n);++i){const auto t=f32(binary64(double(i)*h));const auto next=integer_position(c,t,budget);const auto hit=nearest({previous,{},next,true},p,budget);const int distance=sq(p,hit.point);if(distance<best){best=distance;out={hit.point,f32(std::min(binary64(binary64(double(hit.parameter)*h)+previous_t),1.))};}previous=next;previous_t=t;}return out;
}
std::vector<Hit> cross(const Curve& a,const Curve& b,Budget budget){budget.charge(128);if(a.linear&&b.linear)return line_cross(a,b,budget);if(a.linear){auto out=quad_cross(b,a,budget);for(auto& h:out)std::swap(h.target_parameter,h.probe_parameter);return out;}return quad_cross(a,b,budget);}
std::vector<Hit> cross_obstacle(const Curve& target,const Curve& probe,Budget budget){
    budget.charge(128);auto obstacle=target;obstacle.linear=false;
    admit(obstacle,budget);admit(probe,budget);
    // Native5A43C0 produces RVBezier even for a linear MC: the saved control
    // is active here. Only the probe's +14 kind selects forcedStraight.
    return quad_cross(obstacle,probe,budget,true);
}
std::vector<Hit> cross_mode0(const Curve& target,const Curve& probe,Budget budget){
    budget.charge(128);admit(target,budget);admit(probe,budget);
    if(target.linear&&probe.linear)return line_cross(target,probe,budget);
    if(target.linear){auto out=quad_cross(probe,target,budget,true);for(auto& hit:out){budget.charge(1);std::swap(hit.target_parameter,hit.probe_parameter);}return out;}
    return quad_cross(target,probe,budget,true);
}
std::vector<Hit> cross_mode1(const Curve& target,const Curve& probe,Budget budget){
    budget.charge(128);
    //596F40 ignores mode for two RVLinearLines, including inactive controls.
    if(target.linear&&probe.linear){admit(target,budget);admit(probe,budget);return line_cross(target,probe,budget);}
    //596F40 delegates a Bezier peer to593A70 then exchanges the two t words.
    const bool exchange=target.linear;
    auto a=scaled(exchange?probe:target),b=scaled(exchange?target:probe);
    auto out=cross_obstacle(a,b,budget);
    for(auto& hit:out){budget.charge(4);hit.point={sar2(hit.point.x),sar2(hit.point.y)};if(exchange)std::swap(hit.target_parameter,hit.probe_parameter);}
    return out;
}
}
