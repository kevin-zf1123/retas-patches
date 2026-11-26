#include "retas/vector_render.hpp"
#include "retas/vector_ruler_render.hpp"
#include "retas/vector_shape_render.hpp"
#include "vector_face_program.hpp"
#include "vector_shape_texture.hpp"
#include "vector_stored_line.hpp"
#include "selection_vector_render.hpp"
#include "vector_clipboard_context.hpp"
#include "vector_stored_render_costs.hpp"
#include "vector_mask_render.hpp"
#include "vector_byte_sink.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <set>
#include <tuple>

namespace retas {
namespace {
constexpr std::uint64_t max_work=128ull*1024*1024;
constexpr std::uint64_t max_points=1024*1024;
constexpr double raw_limit=1073741824.0;
constexpr double scratch_limit=1048576.0;
struct IPoint {int x{},y{};bool operator==(const IPoint&)const=default;};
struct WalkerGates {VectorBounds line,model;};
struct Circle {VectorPoint center;float radius{};VectorBounds gate;};
struct Outline {
    std::vector<IPoint> points;Pixel color;VectorBounds bounds;const VectorLineSpan* span{};
    std::vector<VectorBounds> full_blocks;
    const VectorSpanProfiles* profiles{};
    std::optional<WalkerGates> walker;
    std::optional<Circle> circle;
    float minimum{}; //Transient descriptor634/638; original span/CP remain borrowed.
};
struct Admission {
    VectorBounds clip;int curve_step;std::uint64_t work{},points{};
    std::map<const VectorLineSpan*,WalkerGates> walker;
    std::uint64_t preparation_work{}; // Actual validation probes, even without draw commands.
};
enum class ContextProfile {Color,BitMask,BitCircle1x,BitSelect1x,ReferenceOutput,DirectOutput};
bool bit_profile(ContextProfile profile){return profile==ContextProfile::BitMask||profile==ContextProfile::BitCircle1x||profile==ContextProfile::BitSelect1x;}
float effective_base(const VectorLineSpan& s,float minimum){return std::max(float(s.half_width),minimum);}
double deferred_radius(const VectorLineSpan& s,float minimum){
    if(s.fixed_width)return effective_base(s,minimum);
    double scale{};for(const auto& n:s.width_nodes)scale=std::max(scale,n.scale);
    return numeric_detail::binary64(double(effective_base(s,minimum))*scale);
}
float width_at(const VectorLineSpan&,float,double=-1);
float emitted_width(const VectorLineSpan& s,float t,float minimum){
    if(!minimum)return width_at(s,t);
    //5A4320 and the body/end inline arms compare the storedF32 product.
    return std::max(width_at(s,t,effective_base(s,minimum)),minimum);
}
float knot_width(const VectorLineSpan& s,const VectorWidthNode& node,float minimum){
    if(!minimum)return float(s.half_width*node.scale);
    return std::max(float(numeric_detail::binary64(double(effective_base(s,minimum))*node.scale)),minimum);
}
VectorBounds emission_bounds(const VectorLineSpan&,const VectorSpanProfiles* profiles=nullptr);
void cancelled(std::stop_token stop) {
    if(stop.stop_requested())throw std::runtime_error("Vector rendering cancelled");
}
void charge(std::uint64_t& work,std::uint64_t n) {
    if(n>max_work-work)throw std::length_error("Vector render work exceeds supported limits");
    work+=n;
}
bool finite_integer(double v) {return std::isfinite(v)&&std::abs(v)<=raw_limit&&std::floor(v)==v;}
bool point_ok(VectorPoint p) {return finite_integer(p.x)&&finite_integer(p.y);}
bool is_linear(const VectorLineSpan& s) {return s.linear||s.control==s.start||s.control==s.end;}
bool decorated(const VectorSpanProfiles* profiles){return profiles&&(profiles->start||profiles->end);}
bool fresh_profile(const std::optional<VectorEndpointProfile>& endpoint){
    return !endpoint||((endpoint->first_kind==0||endpoint->first_kind==2)&&
                      (endpoint->second_kind==0||endpoint->second_kind==2)&&
                      endpoint->first_trim==0&&endpoint->second_trim==0);
}
bool stored_profile_fields(const std::optional<VectorEndpointProfile>& p){
    return !p||((p->first_kind<=4&&p->second_kind<=4)&&
                (p->first_kind==1||p->first_trim==0)&&
                (p->second_kind==1||p->second_trim==0));
}
bool direct_middle(std::uint32_t kind){return kind==1||kind==4;}
template<class F> void profile_bound_points(const VectorEndpointProfile& profile,F emit){
    emit(profile.point0);
    if(profile.first_kind==2)emit(profile.control0);
    if(profile.second_kind==2)emit(profile.control1);
    if(direct_middle(profile.first_kind)||direct_middle(profile.second_kind)||
       profile.first_kind==2||profile.second_kind==2)emit(profile.middle);
    emit(profile.point2);
}
struct ProfileHalf {VectorPoint start,control,end;};
VectorPoint profile_position(const ProfileHalf& half,float t){
    // 551E50 under the executable's default CRT PC53 precision. Keep each
    // operation rounded to binary64, including the retained F32 complement;
    // a profile RVBezier never takes the body's forced-linear alias path.
    const auto round=numeric_detail::binary64;
    const double u=round(1.0-double(t)),uf=double(float(u));
    const double uu=round(u*uf),product=round(uf*double(t));
    const double middle=round(product+product),tt=round(double(t)*double(t));
    const auto coordinate=[&](double a,double c,double e){
        return round(round(round(e*tt)+round(c*middle))+round(a*uu));
    };
    return {coordinate(half.start.x,half.control.x,half.end.x),
            coordinate(half.start.y,half.control.y,half.end.y)};
}
std::size_t profile_steps(const ProfileHalf& half,const VectorRenderContext& ctx){
    const auto round=numeric_detail::binary64;double length=0;auto previous=half.start;
    for(float t:{0.25f,0.5f,0.75f,1.0f}){
        const auto next=t==1?half.end:profile_position(half,t);
        const double dx=round(previous.x-next.x),dy=round(previous.y-next.y);
        const double chord=round(std::sqrt(round(round(dx*dx)+round(dy*dy))));
        length=round(length+chord);previous=next;
    }
    // 597F60 uses trunc(2*ctx+4B0), not the DPI-dependent body curve step.
    const double denominator=std::trunc(round(2.0*ctx.base_scale));
    const double count=std::max(2.0,std::trunc(round(length/denominator)));
    if(!std::isfinite(count)||count>double(max_points))
        throw std::length_error("Vector endpoint curve subdivision exceeds limits");
    return std::size_t(count);
}
template<class F> void profile_samples(const ProfileHalf& half,const VectorRenderContext& ctx,
                                      std::stop_token stop,F emit){
    const auto count=profile_steps(half,ctx);
    const float delta=float(numeric_detail::binary64(1.0/double(count)));
    for(std::size_t i=1;i<count;++i){
        if(!(i%64))cancelled(stop);
        const float t=float(numeric_detail::binary64(double(i)*double(delta)));
        const auto p=profile_position(half,t);emit(VectorPoint{std::floor(p.x),std::floor(p.y)});
    }
}
VectorPoint position(const VectorLineSpan& s,float t) {
    const double u=1.0-double(t);
    if(is_linear(s))return {s.start.x*u+s.end.x*t,s.start.y*u+s.end.y*t};
    const double uu=u*float(u),middle=2.0*double(float(u))*t,tt=double(t)*t;
    return {(s.end.x*tt+s.control.x*middle)+s.start.x*uu,
            (s.end.y*tt+s.control.y*middle)+s.start.y*uu};
}
VectorPoint tangent(const VectorLineSpan& s,float t) {
    VectorPoint p;
    if(is_linear(s))p={s.end.x-s.start.x,s.end.y-s.start.y};
    else p={(s.end.x*t+s.control.x*(1.0-2.0*t))+s.start.x*(double(t)-1.0),
            (s.end.y*t+s.control.y*(1.0-2.0*t))+s.start.y*(double(t)-1.0)};
    const auto length=std::sqrt(p.x*p.x+p.y*p.y);
    // Native polynomial evaluation can cancel to zero at a sampled float32
    // parameter even when the endpoint determinant is nonzero.
    if(!std::isfinite(length)||length<=0)
        throw std::invalid_argument("Unsupported vector sampled tangent");
    return {double(float(p.x/length)),double(float(p.y/length))};
}
double curve_length(const VectorLineSpan& s) {
    const auto round=numeric_detail::binary64;
    double length=0;auto p=s.start;
    for(float t:{0.25f,0.5f,0.75f,1.0f}){
        const auto q=position(s,t);
        const double dx=round(q.x-p.x),dy=round(q.y-p.y);
        const double chord=round(std::sqrt(round(round(dx*dx)+round(dy*dy))));
        length=round(length+chord);p=q;
    }
    return length;
}
double native_angle(double x,double y){
    if(x>0){double a=std::atan(y/x)*double(57.295776f);return a<0?a+360:a;}
    if(x<0)return std::atan(y/x)*double(57.295776f)+180.0;
    return y>=0?90.0:270.0;
}
bool deferred_outline(const VectorLineSpan& s,double radius){
    if(is_linear(s))return false;
    const auto start=tangent(s,0),end=tangent(s,1);
    // 5A7080 stores only the first 605B90 result to F32. The second
    // heading remains at default-PC53 precision for the subtraction.
    const float a=float(native_angle(start.x,start.y));
    const double b=native_angle(end.x,end.y);
    const double v=std::cos((b-a)*double(0.0087266462f))*(radius+1.0);
    const double threshold=std::trunc(v*v);
    if(threshold>std::numeric_limits<int>::max())throw std::invalid_argument("Unsupported vector deferred-width integer overflow");
    const auto distance=[&](VectorPoint p){const double dx=p.x-s.control.x,dy=p.y-s.control.y;return std::min(double(std::numeric_limits<int>::max()),dx*dx+dy*dy);};
    return distance(s.start)<=threshold||distance(s.end)<=threshold;
}
std::size_t steps(const VectorLineSpan& s,int curve_step) {
    return std::size_t(std::max(2.0,std::trunc(curve_length(s)/curve_step)));
}
Admission admit(const VectorLineGeometry& geometry,const VectorRenderContext& ctx,bool fractional4=false,bool sparse_executor=false,bool stored_nodes=false,ContextProfile profile=ContextProfile::Color,float minimum=0) {
    const bool bit_mask=bit_profile(profile);
    const bool reference=profile==ContextProfile::ReferenceOutput||profile==ContextProfile::DirectOutput;
    if(ctx.width<=0||ctx.height<=0||ctx.width>100000||ctx.height>100000||
       std::uint64_t(ctx.width)*std::uint64_t(ctx.height)>268435456||
       (bit_mask?ctx.depth!=1:(ctx.depth!=32&&ctx.depth!=64))||!std::isfinite(ctx.resolution)||ctx.resolution<=0||
       (ctx.clipboard_affine||reference||profile==ContextProfile::BitMask?(!std::isfinite(ctx.base_scale)||ctx.base_scale<=0):ctx.base_scale!=(profile==ContextProfile::BitMask?4.0:16.0))||
       (reference&&(ctx.converter_fine_step||(!ctx.clipboard_affine&&ctx.origin!=VectorPoint{})))||
       (profile==ContextProfile::ReferenceOutput&&(ctx.depth!=32||ctx.oversample>2))||
       (bit_mask&&(ctx.oversample!=1||(profile!=ContextProfile::BitSelect1x&&ctx.origin!=VectorPoint{})))||
       (profile==ContextProfile::BitMask&&(ctx.width%4||ctx.height%4))||
       (ctx.clipboard_affine?(!std::isfinite(ctx.origin.x)||!std::isfinite(ctx.origin.y)):!point_ok(ctx.origin))||(ctx.oversample!=1&&ctx.oversample!=2&&ctx.oversample!=4))
        throw std::invalid_argument("Unsupported vector render context");
    // Empty geometry still constructs owner-local block rectangles. Check
    // their raw endpoints independently of span bounds before raw_block's
    // integer conversion, including the largest AA right/bottom padding.
    const double padding=std::trunc(ctx.base_scale*(ctx.oversample==4?0.75:ctx.oversample==2?0.5:0.0));
    const double raw_left=ctx.origin.x*ctx.base_scale,raw_top=ctx.origin.y*ctx.base_scale;
    const double raw_right=(ctx.origin.x+ctx.width)*ctx.base_scale+padding;
    const double raw_bottom=(ctx.origin.y+ctx.height)*ctx.base_scale+padding;
    const auto int_min=double(std::numeric_limits<std::int32_t>::min()),int_max=double(std::numeric_limits<std::int32_t>::max());
    if(raw_left<int_min||raw_top<int_min||raw_right>int_max||raw_bottom>int_max)
        throw std::length_error("Vector raw block coordinates exceed supported limits");
    const double step=std::trunc(ctx.resolution*ctx.base_scale*(1.0/31.75)*(ctx.oversample==4||ctx.converter_fine_step?0.25:1.0));
    if(step<1||step>std::numeric_limits<int>::max())
        throw std::invalid_argument("Unsupported vector curve sampling scale");
    VectorBounds clip{0,0,ctx.width,ctx.height};
    if(ctx.clip){
        if(ctx.clip->right<ctx.clip->left||ctx.clip->bottom<ctx.clip->top)
            throw std::invalid_argument("Invalid vector clip rectangle");
        clip={std::clamp(ctx.clip->left,0,ctx.width),std::clamp(ctx.clip->top,0,ctx.height),
              std::clamp(ctx.clip->right,0,ctx.width),std::clamp(ctx.clip->bottom,0,ctx.height)};
        clip.right=std::max(clip.left,clip.right);clip.bottom=std::max(clip.top,clip.bottom);
    }
    if(geometry.strokes.size()>100000)throw std::length_error("Too many vector strokes");
    const VectorBounds work_clip{clip.left/256*256,clip.top/256*256,
                                std::min(ctx.width,(clip.right+255)/256*256),
                                std::min(ctx.height,(clip.bottom+255)/256*256)};
    std::uint64_t points=0,work=0,span_count=0;
    for(const auto& stroke:geometry.strokes)for(const auto& s:stroke.spans){
        if(++span_count>100000)throw std::length_error("Too many vector spans");
        if(!point_ok(s.start)||!point_ok(s.control)||!point_ok(s.end)||
           !std::isfinite(s.half_width)||s.half_width<0||s.half_width>65536||s.start==s.end)
            throw std::invalid_argument("Unsupported vector line geometry");
        if(!is_linear(s)){
            const double ax=s.control.x-s.start.x,ay=s.control.y-s.start.y;
            const double bx=s.end.x-s.control.x,by=s.end.y-s.control.y;
            if(ax*by-ay*bx==0&&ax*bx+ay*by<=0)
                throw std::invalid_argument("Unsupported vector cusp tangent");
        }
        const double base=minimum?effective_base(s,minimum):s.half_width;
        double width=s.fixed_width?base:minimum,prev=-1;
        if(!s.fixed_width&&s.width_nodes.empty())
            throw std::invalid_argument("Variable vector width needs endpoint nodes");
        for(const auto& n:s.width_nodes){
            if(!std::isfinite(n.parameter)||n.parameter<0||n.parameter>1||(stored_nodes?n.parameter<prev:n.parameter<=prev)||
               !std::isfinite(n.scale)||n.scale<0||n.scale>65536)
                throw std::invalid_argument("Invalid vector width node");
            prev=n.parameter;if(!s.fixed_width)width=std::max(width,base*n.scale);
        }
        if(!s.fixed_width&&(s.width_nodes.back().parameter!=1||
           (s.width_nodes.size()>1&&s.width_nodes.front().parameter!=0)))
            throw std::invalid_argument("Unbounded vector width interpolation");
        const double margin=1.1*width+3;
        const double left=std::min({s.start.x,s.control.x,s.end.x})-margin;
        const double right=std::max({s.start.x,s.control.x,s.end.x})+margin;
        const double top=std::min({s.start.y,s.control.y,s.end.y})-margin;
        const double bottom=std::max({s.start.y,s.control.y,s.end.y})+margin;
        if(width>65536||std::max({std::abs(left),std::abs(right),std::abs(top),std::abs(bottom)})>raw_limit)
            throw std::length_error("Vector outline exceeds coordinate limits");
        const auto q=[&](double v,double origin){return v*ctx.oversample/ctx.base_scale-origin*ctx.oversample;};
        if(std::max({std::abs(q(left,ctx.origin.x)),std::abs(q(right,ctx.origin.x)),
                     std::abs(q(top,ctx.origin.y)),std::abs(q(bottom,ctx.origin.y))})>scratch_limit)
            throw std::length_error("Vector fixed-point coordinates exceed supported limits");
        const auto interior=is_linear(s)?s.width_nodes.size():steps(s,int(step));
        const auto count=2*interior+std::uint64_t(width/ctx.base_scale+2)*2+8;
        if(count>max_points-points)throw std::length_error("Vector point count exceeds supported limits");
        points+=count;
        if(deferred_outline(s,minimum?deferred_radius(s,minimum):width))charge(work,count*count);
        const double x0=std::max(double(work_clip.left),left/ctx.base_scale-ctx.origin.x);
        const double y0=std::max(double(work_clip.top),top/ctx.base_scale-ctx.origin.y);
        const double x1=std::min(double(work_clip.right),right/ctx.base_scale-ctx.origin.x);
        const double y1=std::min(double(work_clip.bottom),bottom/ctx.base_scale-ctx.origin.y);
        if(x1>x0&&y1>y0){
            const auto columns=std::uint64_t(std::ceil(x1-x0)+2),rows=std::uint64_t(std::ceil(y1-y0)+2);
            charge(work,rows*columns*ctx.oversample*ctx.oversample);
            charge(work,rows*ctx.oversample*count*std::uint64_t(std::bit_width(count)));
        }
    }
    const auto tile_columns=clip.right>clip.left?std::uint64_t((clip.right-1)/256-clip.left/256+1):0;
    const auto tile_rows=clip.bottom>clip.top?std::uint64_t((clip.bottom-1)/256-clip.top/256+1):0;
    if(tile_columns*tile_rows*(bit_mask?8192ull:65536*std::uint64_t(ctx.depth/8))>512ull*1024*1024)
        throw std::length_error("Vector raster storage exceeds supported limits");
    charge(work,tile_columns*tile_rows*points); // Boundary admission probes every physical block.
    if(span_count&&!sparse_executor){
        const auto resolve_cost=ctx.oversample==4&&!fractional4?256u:ctx.oversample*ctx.oversample+1;
        charge(work,std::uint64_t(clip.right-clip.left)*std::uint64_t(clip.bottom-clip.top)*resolve_cost);
    }
    return {clip,int(step),work,points,{}};
}
Admission admit(std::span<const VectorLineGeometry> geometries,const VectorRenderContext& ctx){
    if(geometries.size()>100000)throw std::length_error("Too many vector planes");
    const VectorLineGeometry empty;auto result=admit(empty,ctx);
    for(const auto& g:geometries){
        const auto a=admit(g,ctx);charge(result.work,a.work);
        if(a.points>max_points-result.points)throw std::length_error("Vector owner point count exceeds supported limits");
        result.points+=a.points;
    }
    return result;
}
enum class ProfileSource {Fresh,Stored};
Admission admit_profiled(const VectorLineGeometry& geometry,std::span<const VectorSpanProfiles> profiles,
                         const VectorRenderContext& ctx,std::stop_token stop={},ProfileSource source=ProfileSource::Fresh,bool fractional4=false,bool sparse_executor=false,ContextProfile context_profile=ContextProfile::Color,float minimum=0){
    auto result=admit(geometry,ctx,fractional4,sparse_executor,source==ProfileSource::Stored,context_profile,minimum);
    if(profiles.empty())return result;
    std::size_t count=0;for(const auto& stroke:geometry.strokes)count+=stroke.spans.size();
    if(profiles.size()!=count)throw std::invalid_argument("Vector endpoint profile count does not match spans");
    const auto validate_point=[&](VectorPoint p){
        if(!point_ok(p))throw std::length_error("Vector endpoint curve sample exceeds raw limits");
        const double x=p.x*ctx.oversample/ctx.base_scale-ctx.origin.x*ctx.oversample;
        const double y=p.y*ctx.oversample/ctx.base_scale-ctx.origin.y*ctx.oversample;
        if(std::max(std::abs(x),std::abs(y))>scratch_limit)
            throw std::length_error("Vector endpoint profile exceeds fixed-point limits");
    };
    const auto validate=[&](const VectorEndpointProfile& profile){
        const auto kind_ok=[](std::uint32_t kind){return kind<=4;};
        if(!kind_ok(profile.first_kind)||!kind_ok(profile.second_kind)||
           !point_ok(profile.point0)||!point_ok(profile.control0)||!point_ok(profile.middle)||
           !point_ok(profile.control1)||!point_ok(profile.point2)||
           (profile.first_kind!=2&&profile.control0!=VectorPoint{})||
           (profile.second_kind!=2&&profile.control1!=VectorPoint{})||
           !std::isfinite(profile.first_trim)||!std::isfinite(profile.second_trim)||
           profile.first_trim<0||profile.first_trim>1||profile.second_trim<0||profile.second_trim>1)
            throw std::invalid_argument("Unsupported vector endpoint profile");
        profile_bound_points(profile,validate_point);
    };
    const auto c=result.clip;
    const VectorBounds work_clip{c.left/256*256,c.top/256*256,
                                std::min(ctx.width,(c.right+255)/256*256),
                                std::min(ctx.height,(c.bottom+255)/256*256)};
    std::map<std::int32_t,const VectorModelCurve*> original_models;
    bool models_indexed=false;
    const auto fresh_quadratic=[&](const VectorLineSpan& span,const VectorSpanProfiles& p){
        if(!models_indexed){
            if(geometry.archive.models.size()>100000)throw std::length_error("Too many original vector models");
            charge(result.work,geometry.archive.models.size());
            for(const auto& model:geometry.archive.models)
                if(!original_models.emplace(model.index,&model.value).second)
                    throw std::invalid_argument("Ambiguous original vector model identity");
            models_indexed=true;
        }
        const auto model=original_models.find(span.model_index);
        return span.model_index>=0&&model!=original_models.end()&&model->second->flags==2&&
               fresh_profile(p.start)&&fresh_profile(p.end);
    };
    std::size_t index=0;std::uint64_t added_points=0;
    for(const auto& stroke:geometry.strokes)for(const auto& span:stroke.spans){
        cancelled(stop);
        const auto& p=profiles[index++];if(!decorated(&p))continue;
        const bool quadratic=!is_linear(span);
        if(source==ProfileSource::Stored&&(!stored_profile_fields(p.start)||!stored_profile_fields(p.end)))throw std::invalid_argument("Stored Line trims require kind1 halves");
        if(source==ProfileSource::Fresh&&(!span.fixed_width||(quadratic&&!fresh_quadratic(span,p))))
            throw std::invalid_argument("Endpoint profiles require fixed linear or fresh Model2 quadratic spans");
        if(source==ProfileSource::Stored&&!span.fixed_width&&span.width_nodes.size()<2)
            throw std::invalid_argument("Stored variable endpoint profiles need two width nodes");
        if(p.start)validate(*p.start);if(p.end)validate(*p.end);
        std::uint64_t samples=0;
        const auto count_samples=[&](const VectorEndpointProfile& profile){
            if(profile.first_kind==2)samples+=profile_steps({profile.point0,profile.control0,profile.middle},ctx)-1;
            if(profile.second_kind==2)samples+=profile_steps({profile.middle,profile.control1,profile.point2},ctx)-1;
        };
        if(p.start)count_samples(*p.start);if(p.end)count_samples(*p.end);
        const auto extra=8+samples;
        if(extra>max_points-result.points)throw std::length_error("Vector endpoint profile point count exceeds limits");
        result.points+=extra;added_points+=extra;
        charge(result.work,samples*64);
        charge(result.preparation_work,samples*64);
        double max_width=std::max(double(minimum),minimum?deferred_radius(span,minimum):span.fixed_width?span.half_width:0);
        if(!span.fixed_width&&!minimum)for(const auto& node:span.width_nodes)max_width=std::max(max_width,span.half_width*node.scale);
        const auto interior=quadratic?steps(span,result.curve_step):span.width_nodes.size();
        const auto body_points=2*std::uint64_t(interior)+std::uint64_t(max_width/ctx.base_scale+2)*2+8;
        if(quadratic&&deferred_outline(span,minimum?deferred_radius(span,minimum):max_width))
            charge(result.work,(body_points+extra)*(body_points+extra)-body_points*body_points);
        // F32 complement rounding can move actual integer samples outside
        // the control hull. Charge their real extent without altering the
        // separate native 597BF0 bound union used by the analytic probe.
        auto resource_bounds=emission_bounds(span,&p);
        const auto validate_sample=[&](VectorPoint sample){
            validate_point(sample);
            resource_bounds.left=std::min(resource_bounds.left,int(sample.x));
            resource_bounds.top=std::min(resource_bounds.top,int(sample.y));
            resource_bounds.right=std::max(resource_bounds.right,int(sample.x)+1);
            resource_bounds.bottom=std::max(resource_bounds.bottom,int(sample.y)+1);
        };
        const auto validate_samples=[&](const VectorEndpointProfile& profile){
            if(profile.first_kind==2)profile_samples({profile.point0,profile.control0,profile.middle},ctx,stop,validate_sample);
            if(profile.second_kind==2)profile_samples({profile.middle,profile.control1,profile.point2},ctx,stop,validate_sample);
        };
        if(p.start)validate_samples(*p.start);if(p.end)validate_samples(*p.end);
        if(source==ProfileSource::Stored&&!span.fixed_width){
            // Original F32 body evaluation can leave the control hull too.
            // Probe emitted sides using the exact drawing kernels; trims
            // never reduce original counts, and analytic bounds stay intact.
            charge(result.work,std::uint64_t(interior)*64);
            charge(result.preparation_work,std::uint64_t(interior)*64);
            const auto probe=[&](float t,float width){const auto point=position(span,t),normal=tangent(span,t);
                if((!p.start||p.start->second_kind!=1||t>p.start->second_trim)&&
                   (!p.end||p.end->first_kind!=1||t<p.end->first_trim))
                    validate_sample({std::floor(point.x+normal.y*width),std::floor(point.y-normal.x*width)});
                if((!p.start||p.start->first_kind!=1||t>p.start->first_trim)&&
                   (!p.end||p.end->second_kind!=1||t<p.end->second_trim))
                    validate_sample({std::floor(point.x-normal.y*width),std::floor(point.y+normal.x*width)});
            };
            if(quadratic){const float delta=float(1.0/double(interior));for(std::size_t i=1;i<interior;++i){if(!(i%64))cancelled(stop);const float t=float(double(i)*delta);probe(t,emitted_width(span,t,minimum));}}
            else for(const auto& node:span.width_nodes)if(node.parameter>0&&node.parameter<1){cancelled(stop);probe(float(node.parameter),knot_width(span,node,minimum));}
        }
        const auto bounds=resource_bounds;
        const double left=std::max(double(work_clip.left),bounds.left/ctx.base_scale-ctx.origin.x);
        const double top=std::max(double(work_clip.top),bounds.top/ctx.base_scale-ctx.origin.y);
        const double right=std::min(double(work_clip.right),bounds.right/ctx.base_scale-ctx.origin.x);
        const double bottom=std::min(double(work_clip.bottom),bounds.bottom/ctx.base_scale-ctx.origin.y);
        if(right>left&&bottom>top){
            const auto rows=std::uint64_t(std::ceil(bottom-top)+2),columns=std::uint64_t(std::ceil(right-left)+2);
            charge(result.work,rows*columns*ctx.oversample*ctx.oversample);
            const auto route_points=quadratic||!span.fixed_width?body_points+extra:16+samples;
            charge(result.work,rows*ctx.oversample*route_points*std::uint64_t(std::bit_width(route_points-1)));
        }
    }
    const auto tile_columns=c.right>c.left?std::uint64_t((c.right-1)/256-c.left/256+1):0;
    const auto tile_rows=c.bottom>c.top?std::uint64_t((c.bottom-1)/256-c.top/256+1):0;
    charge(result.work,tile_columns*tile_rows*added_points);
    // The native walkers test LC and MC stroke bounds before the private
    // subcurve bound. Rebuild them from the complete fresh graph and active
    // descriptors, never from persistent cached rectangles.
    if(models_indexed){
        if(geometry.archive.lines.size()>100000||geometry.archive.subcurves.size()>100000||geometry.archive.vertices.size()>100000)
            throw std::length_error("Too many original vector graph records");
        std::map<std::int32_t,const VectorLine*> original_lines;
        for(const auto& line:geometry.archive.lines)
            if(line.index<0||!original_lines.emplace(line.index,&line.value).second)
                throw std::invalid_argument("Ambiguous original vector line identity");
        std::map<std::int32_t,const VectorSubcurve*> original_subcurves;
        for(const auto& subcurve:geometry.archive.subcurves)
            if(subcurve.index<0||!original_subcurves.emplace(subcurve.index,&subcurve.value).second)
                throw std::invalid_argument("Ambiguous original vector subcurve identity");
        std::map<std::int32_t,const VectorVertex*> original_vertices;
        for(const auto& vertex:geometry.archive.vertices)
            if(vertex.index<0||!original_vertices.emplace(vertex.index,&vertex.value).second)
                throw std::invalid_argument("Ambiguous original vector vertex identity");
        charge(result.work,geometry.archive.lines.size()+geometry.archive.subcurves.size()+geometry.archive.vertices.size());
        using Owner=std::pair<std::int32_t,std::size_t>;
        std::map<std::int32_t,Owner> model_owners,subcurve_owners;
        for(const auto& line:geometry.archive.lines){
            charge(result.work,line.value.members.size());
            for(const auto& member:line.value.members)if(original_models.contains(member.model)){
                const auto [it,inserted]=model_owners.emplace(member.model,Owner{line.index,1});
                if(!inserted)++it->second.second;
            }
        }
        for(const auto& model:geometry.archive.models){
            charge(result.work,model.value.subcurves.size());
            for(auto subcurve:model.value.subcurves)if(original_subcurves.contains(subcurve)){
                const auto [it,inserted]=subcurve_owners.emplace(subcurve,Owner{model.index,1});
                if(!inserted)++it->second.second;
            }
        }
        std::map<std::int32_t,std::vector<std::int32_t>> incidences;
        for(const auto& subcurve:geometry.archive.subcurves)
            for(auto vertex:{subcurve.value.vertex_a,subcurve.value.vertex_b})if(original_vertices.contains(vertex))
                incidences[vertex].push_back(subcurve.index);
        std::map<std::int32_t,bool> used_models,used_lines,used_subcurves;
        const auto unite=[](VectorBounds a,VectorBounds b){
            return VectorBounds{std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};
        };
        const auto body_bounds=[](const VectorLineSpan& span,bool original_linear){
            const int h=int(std::trunc(span.half_width));
            const auto a=span.start,c=original_linear?span.start:span.control,e=span.end;
            return VectorBounds{int(std::min({a.x,c.x,e.x}))-h,int(std::min({a.y,c.y,e.y}))-h,
                                int(std::max({a.x,c.x,e.x}))+1+h,int(std::max({a.y,c.y,e.y}))+1+h};
        };
        std::size_t profile_start=0;
        for(const auto& stroke:geometry.strokes){
            bool fresh=false;
            for(std::size_t i=0;i<stroke.spans.size();++i)
                fresh|=!is_linear(stroke.spans[i])&&decorated(&profiles[profile_start+i]);
            if(!fresh){profile_start+=stroke.spans.size();continue;}
            const auto line_it=original_lines.find(stroke.line_index);
            if(stroke.line_index<0||line_it==original_lines.end()||!used_lines.emplace(stroke.line_index,true).second)
                throw std::invalid_argument("Fresh quadratic needs one complete original line owner");
            const auto& line=*line_it->second;
            if(line.color!=stroke.color)throw std::invalid_argument("Fresh quadratic line material mismatch");
            const double h=double(std::bit_cast<float>(line.half_width_bits));
            const auto n=stroke.spans.size();
            if(profiles[profile_start].start||profiles[profile_start+n-1].end)
                throw std::invalid_argument("Fresh identity-open line needs null outer profiles");
            struct Group {std::size_t begin,end;VectorBounds bounds;};
            std::vector<Group> groups;
            std::vector<std::int32_t> path_vertices;
            std::map<std::int32_t,bool> path_unique;
            std::size_t begin=0;
            while(begin<n){
                cancelled(stop);std::size_t end=begin+1;
                while(end<n&&stroke.spans[end].model_index==stroke.spans[begin].model_index)++end;
                const auto model_it=original_models.find(stroke.spans[begin].model_index);
                if(model_it==original_models.end()||!used_models.emplace(model_it->first,true).second)
                    throw std::invalid_argument("Fresh quadratic needs complete isolated original models");
                const auto& model=*model_it->second;
                if(model.line!=stroke.line_index||!(model.flags&2)||(model.flags&0x30)||
                   model.target_kinds!=std::array<std::int32_t,2>{0,0}||
                   model.targets!=std::array<std::int32_t,2>{-1,-1}||
                   model.crosspoints!=std::array<std::int32_t,2>{-1,-1}||
                   !model_owners.contains(model_it->first)||model_owners.at(model_it->first)!=Owner{stroke.line_index,1}||
                   model.subcurves.size()!=end-begin||groups.size()>=line.members.size()||
                   line.members[groups.size()].model!=model_it->first||line.members[groups.size()].selector!=1)
                    throw std::invalid_argument("Fresh quadratic original owner membership mismatch");
                VectorBounds bounds{};
                for(std::size_t i=begin;i<end;++i){
                    const auto& span=stroke.spans[i];const auto& p=profiles[profile_start+i];
                    if(!span.fixed_width||span.half_width!=h||!span.width_nodes.empty()||
                       model.subcurves[i-begin]!=span.subcurve_index||
                       span.subcurve_index<0||!used_subcurves.emplace(span.subcurve_index,true).second||
                       span.linear!=bool((model.flags&1)||span.control==span.start||span.control==span.end)||
                       (i&&stroke.spans[i-1].end!=span.start)||
                       span.round_start!=(i==0)||span.round_end!=(i+1==n)||
                       (i!=begin&&p.start)||(i+1!=end&&p.end)||
                       (model.flags==2&&(!fresh_profile(p.start)||!fresh_profile(p.end)))||
                       (!(model.flags&1)&&model.flags!=2))
                        throw std::invalid_argument("Unsupported fresh quadratic group or cap contract");
                    const auto raw_it=original_subcurves.find(span.subcurve_index);
                    if(raw_it==original_subcurves.end()||raw_it->second->model!=model_it->first||
                       !raw_it->second->scales.empty()||
                       !subcurve_owners.contains(span.subcurve_index)||
                       subcurve_owners.at(span.subcurve_index)!=Owner{model_it->first,1})
                        throw std::invalid_argument("Fresh quadratic original subcurve ownership mismatch");
                    const auto& raw=*raw_it->second;
                    const auto va=original_vertices.find(raw.vertex_a),vb=original_vertices.find(raw.vertex_b);
                    if(va==original_vertices.end()||vb==original_vertices.end()||
                       VectorPoint{double(va->second->x),double(va->second->y)}!=span.start||
                       VectorPoint{double(vb->second->x),double(vb->second->y)}!=span.end||
                       VectorPoint{double(raw.control_x),double(raw.control_y)}!=span.control||
                       (i&&path_vertices.back()!=raw.vertex_a))
                        throw std::invalid_argument("Fresh quadratic original control or endpoint identity mismatch");
                    if(i==0){path_vertices.push_back(raw.vertex_a);path_unique.emplace(raw.vertex_a,true);}
                    if(!path_unique.emplace(raw.vertex_b,true).second)
                        throw std::invalid_argument("Fresh quadratic needs distinct identity-open path vertices");
                    path_vertices.push_back(raw.vertex_b);
                    const auto b=body_bounds(span,(model.flags&1)!=0);
                    bounds=i==begin?b:unite(bounds,b);
                }
                const auto include=[&](VectorPoint p){bounds=unite(bounds,{int(p.x),int(p.y),int(p.x)+1,int(p.y)+1});};
                if(profiles[profile_start+begin].start)profile_bound_points(*profiles[profile_start+begin].start,include);
                if(profiles[profile_start+end-1].end)profile_bound_points(*profiles[profile_start+end-1].end,include);
                groups.push_back({begin,end,bounds});begin=end;
            }
            if(groups.size()!=line.members.size())throw std::invalid_argument("Incomplete fresh quadratic line members");
            for(std::size_t i=0;i<path_vertices.size();++i){
                const auto vertex=path_vertices[i];const auto& raw=*original_vertices.at(vertex);
                std::vector<std::int32_t> expected;
                if(i)expected.push_back(stroke.spans[i-1].subcurve_index);
                if(i<n)expected.push_back(stroke.spans[i].subcurve_index);
                if(raw.subcurves.size()!=expected.size()||incidences[vertex].size()!=expected.size())
                    throw std::invalid_argument("Fresh quadratic original vertex degree mismatch");
                auto listed=raw.subcurves,physical=incidences[vertex];
                std::sort(expected.begin(),expected.end());std::sort(listed.begin(),listed.end());std::sort(physical.begin(),physical.end());
                if(listed!=expected||physical!=expected)
                    throw std::invalid_argument("Fresh quadratic original reciprocal vertex adjacency mismatch");
            }
            auto line_bounds=groups.front().bounds;
            for(const auto& group:groups)line_bounds=unite(line_bounds,group.bounds);
            for(const auto& group:groups)for(std::size_t i=group.begin;i<group.end;++i)
                result.walker.emplace(&stroke.spans[i],WalkerGates{line_bounds,group.bounds});
            charge(result.work,n);profile_start+=n;
        }
    }
    return result;
}
Pixel color(std::uint64_t v,int depth) {
    Pixel p{std::uint16_t(v),std::uint16_t(v>>16),std::uint16_t(v>>32),std::uint16_t(v>>48)};
    if(depth==32)p=Pixel::rgba8(p.r>>8,p.g>>8,p.b>>8,p.a>>8);
    return p;
}
void append(std::vector<IPoint>& points,VectorPoint p,const VectorRenderContext& ctx) {
    (void)ctx;
    const double low=double(std::numeric_limits<int>::min()),high=double(std::numeric_limits<int>::max());
    if(!std::isfinite(p.x)||!std::isfinite(p.y)||p.x<low||p.x>high||p.y<low||p.y>high)
        throw std::invalid_argument("Unsupported vector emitted raw coordinate");
    IPoint q{int(p.x),int(p.y)};
    if(points.empty()||points.back()!=q)points.push_back(q);
}
void cap(std::vector<IPoint>& points,VectorPoint center,VectorPoint t,float radius,bool round,
         const VectorRenderContext& ctx,std::stop_token stop) {
    const auto raw=[&](double x,double y){append(points,{center.x+std::floor(x),center.y+std::floor(y)},ctx);};
    if(!round){
        // sub_5A6730 stores the four products as float32 before flooring.
        const float rx=float(radius*t.x),ry=float(radius*t.y);
        const double extend=double(radius)*double(0.1f)+2.0;
        const float ex=float(extend*t.x),ey=float(extend*t.y);
        raw(double(ex)+ry,double(ey)-rx);raw(double(ex)-ry,double(ey)+rx);return;
    }
    const int n=int(std::trunc((1.0/ctx.base_scale)*radius))+1;
    const double angle=native_angle(t.x,t.y);
    const float base=float(angle),step=float(180.0/n);
    for(int i=0;i<=n;++i){
        if(!(i%64))cancelled(stop);
        const double radians=(double(i)*step+base-90.0)*double(0.017453292f);
        raw(std::cos(radians)*radius,double(float(std::sin(radians)*radius)));
    }
}
float width_at(const VectorLineSpan& s,float t,double base) {
    if(base<0)base=s.half_width;
    if(s.fixed_width)return float(base);
    if(s.width_nodes.size()==1)return float(base*s.width_nodes.front().scale);
    if(t<=0)return float(base*s.width_nodes.front().scale);
    if(t>=1)return float(base*s.width_nodes.back().scale);
    auto upper=std::upper_bound(s.width_nodes.begin(),s.width_nodes.end(),t,
        [](double v,const VectorWidthNode& n){return v<n.parameter;});
    const auto& b=*upper;const auto& a=*(upper-1);
    // 5A7080 interpolates the scale in PC53 before multiplying H and
    // finally storing F32. Reordering H across the division changes pixels.
    const auto numerator=numeric_detail::binary64(
        numeric_detail::binary64(numeric_detail::binary64(b.parameter-t)*a.scale)+
        numeric_detail::binary64(numeric_detail::binary64(t-a.parameter)*b.scale));
    const auto scale=numeric_detail::binary64(numerator/numeric_detail::binary64(b.parameter-a.parameter));
    return float(numeric_detail::binary64(scale*base));
}
bool cap_intersects(VectorPoint p,float radius,const VectorBounds& clip){
    const auto extent=std::trunc(double(radius)+1.0);
    return p.x+extent>clip.left&&p.x-extent<clip.right&&p.y+extent>clip.top&&p.y-extent<clip.bottom;
}
void reorder_deferred(std::vector<IPoint>& input,std::stop_token stop){
    // sub_606380 is an angular walk over the original list, including its
    // first-input tie rule. It is not a conventional convex-hull primitive.
    if(input.size()>1&&input.front()==input.back())input.pop_back();
    auto start=input.front();
    for(auto p:input)if(p.y>start.y||(p.y==start.y&&p.x<start.x))start=p;
    std::vector<std::uint8_t> alive(input.size(),1);std::vector<IPoint> result;
    auto current=start;float previous_angle=180;bool first_pass=true;
    for(std::size_t iteration=0;iteration<=input.size();++iteration){
        cancelled(stop);std::optional<std::size_t> selected;float best=1000,selected_angle=0;
        for(std::size_t i=0;i<input.size();++i){
            if(!(i%256))cancelled(stop);if(!alive[i])continue;
            if(input[i]==current){if(!first_pass)alive[i]=0;continue;}
            const float angle=float(native_angle(double(input[i].x)-current.x,double(input[i].y)-current.y));
            double delta=double(angle)-previous_angle;if(delta<0)delta+=360;
            if(delta<best){best=float(delta);selected=i;selected_angle=angle;}
        }
        if(!selected)throw std::invalid_argument("Unsupported collapsed deferred vector outline");
        current=input[*selected];result.push_back(current);
        if(current==start){input=std::move(result);return;}
        alive[*selected]=0;previous_angle=selected_angle;first_pass=false;
    }
    throw std::invalid_argument("Unsupported non-closing deferred vector outline");
}
Outline outline(const VectorLineSpan& s,Pixel pigment,const VectorRenderContext& ctx,int curve_step,
                std::stop_token stop,const VectorBounds* raw_clip=nullptr,const VectorSpanProfiles* profiles=nullptr,float minimum=0) {
    Outline result;result.color=pigment;result.span=&s;result.profiles=profiles;result.minimum=minimum;std::vector<IPoint> reverse;
    const bool round_start=s.round_start&&(!raw_clip||cap_intersects(s.start,emitted_width(s,0,minimum),*raw_clip));
    const bool round_end=s.round_end&&(!raw_clip||cap_intersects(s.end,emitted_width(s,1,minimum),*raw_clip));
    const auto emit_profile=[&](const VectorEndpointProfile& profile){
        // sub_598040 preserves the stored profile order at both endpoints.
        // Half-curve endpoints are explicit descriptor points, never n*h.
        const auto emit=[&](VectorPoint p){cancelled(stop);append(result.points,p,ctx);};
        emit(profile.point0);
        if(profile.first_kind==2){
            profile_samples({profile.point0,profile.control0,profile.middle},ctx,stop,emit);
            emit(profile.middle);
        }else if(direct_middle(profile.first_kind))emit(profile.middle);
        if(profile.second_kind==2){
            emit(profile.middle);
            profile_samples({profile.middle,profile.control1,profile.point2},ctx,stop,emit);
        }else if(direct_middle(profile.second_kind))emit(profile.middle);
        emit(profile.point2);
    };
    auto t=tangent(s,0);
    if(profiles&&profiles->start)emit_profile(*profiles->start);
    else cap(result.points,s.start,{-t.x,-t.y},emitted_width(s,0,minimum),round_start,ctx,stop);
    const auto pair=[&](float parameter,float width){
        auto p=position(s,parameter),v=tangent(s,parameter);
        // 5A7080 clips original body samples independently on each side.
        // Comparisons are strict; no trim endpoint is inserted, and equal or
        // reversed windows do not remove the separately emitted descriptors.
        const auto start=profiles&&profiles->start?&*profiles->start:nullptr;
        const auto end=profiles&&profiles->end?&*profiles->end:nullptr;
        if((!start||start->second_kind!=1||parameter>start->second_trim)&&
           (!end||end->first_kind!=1||parameter<end->first_trim))
            append(result.points,{std::floor(p.x+v.y*width),std::floor(p.y-v.x*width)},ctx);
        if((!start||start->first_kind!=1||parameter>start->first_trim)&&
           (!end||end->second_kind!=1||parameter<end->second_trim))
            append(reverse,{std::floor(p.x-v.y*width),std::floor(p.y+v.x*width)},ctx);
    };
    if(is_linear(s)){
        if(!s.fixed_width)for(const auto& node:s.width_nodes)if(node.parameter>0&&node.parameter<1){
            cancelled(stop);pair(float(node.parameter),knot_width(s,node,minimum));
        }
    }else{
        const auto n=steps(s,curve_step);const float delta=float(1.0/double(n));
        for(std::size_t i=1;i<n;++i){if(!(i%64))cancelled(stop);float parameter=float(double(i)*delta);pair(parameter,emitted_width(s,parameter,minimum));}
    }
    t=tangent(s,1);
    if(profiles&&profiles->end)emit_profile(*profiles->end);
    else cap(result.points,s.end,t,emitted_width(s,1,minimum),round_end,ctx,stop);
    for(auto i=reverse.rbegin();i!=reverse.rend();++i)if(result.points.empty()||result.points.back()!=*i)result.points.push_back(*i);
    double maximum=s.fixed_width?s.half_width:0;
    if(!s.fixed_width)for(const auto& n:s.width_nodes)maximum=std::max(maximum,s.half_width*n.scale);
    if(deferred_outline(s,minimum?deferred_radius(s,minimum):maximum))reorder_deferred(result.points,stop);
    if(result.points.size()>1&&result.points.back()!=result.points.front())result.points.push_back(result.points.front());
    result.bounds={std::numeric_limits<int>::max(),std::numeric_limits<int>::max(),std::numeric_limits<int>::min(),std::numeric_limits<int>::min()};
    const double quantize_scale=(1.0/ctx.base_scale)*ctx.oversample;
    for(auto raw:result.points){
        const auto converted=ctx.clipboard_affine?vector_render_detail::clipboard_quantize({double(raw.x),double(raw.y)},ctx,ctx.oversample):VectorPoint{std::floor(raw.x*quantize_scale)-ctx.origin.x*ctx.oversample,std::floor(raw.y*quantize_scale)-ctx.origin.y*ctx.oversample};const IPoint p{int(converted.x),int(converted.y)};
        result.bounds.left=std::min(result.bounds.left,p.x);result.bounds.top=std::min(result.bounds.top,p.y);result.bounds.right=std::max(result.bounds.right,p.x+1);result.bounds.bottom=std::max(result.bounds.bottom,p.y+1);
    }
    return result;
}

// Native full-pixel compose places a new color below existing alpha. The
// fractional writer instead attenuates the existing alpha by the new coverage.
Pixel compose(Pixel d,Pixel s,int depth,bool fractional=false,double coverage=1) {
    const unsigned bits=depth==32?8:16,unit=1u<<bits,maximum=unit-1;
    auto lane=[&](std::uint16_t v){return depth==32?unsigned(v/257):unsigned(v);};
    const auto da0=lane(d.a),sa0=lane(s.a);
    std::uint64_t sa=sa0,da=da0,source_weight=0;
    if(fractional){
        const auto k=std::min(unit,unsigned(std::trunc(coverage*unit+1.0)));
        source_weight=std::uint64_t(sa0)*k;sa=source_weight>>bits;
        if(!sa)return d;
        da=std::uint64_t(da0)*(maximum-sa)>>bits;
    }else{
        if(!sa||da==maximum)return d;
        if(!da)return s;
        sa=std::uint64_t(maximum-da)*sa/maximum;
    }
    if(!da0){s.a=std::uint16_t(sa*(depth==32?257:1));return s;}
    const auto total=da+sa;
    if(!total)return {};
    const auto dw=da*unit/total,sw=fractional?source_weight/total:sa*unit/total;
    const auto ch=[&](std::uint16_t dc,std::uint16_t sc){
        auto weighted=lane(dc)*dw+lane(sc)*sw;
        // sub_6483C0 keeps the low DWORD after its IMUL/ADD pair. Narrow
        // unsigned arithmetic explicitly, before the logical word shift.
        if(depth==64&&fractional)weighted=std::uint32_t(weighted);
        auto n=std::min<std::uint64_t>(maximum,weighted>>bits);
        // sub_648000 explicitly retains the low byte after full-pixel compose.
        if(depth==64&&!fractional)n&=255;
        return std::uint16_t(n*(depth==32?257:1));
    };
    auto alpha=std::min<std::uint64_t>(maximum,total);
    if(depth==64&&!fractional)alpha&=255;
    return {ch(d.r,s.r),ch(d.g,s.g),ch(d.b,s.b),std::uint16_t(alpha*(depth==32?257:1))};
}
Pixel compose_byte(Pixel destination,Pixel source){
    const unsigned d=destination.r>>8,s=source.r>>8;
    const auto value=std::uint8_t(d+(255-d)*s/255);
    return Pixel::rgba8(value,value,value,value);
}
struct Scratch {
    VectorBounds bounds;int width{},height{},depth{};unsigned mode{};std::vector<Pixel> pixels;
    std::vector<std::uint8_t> marked;std::vector<std::size_t> dirty;
    bool fractional4{}; // Private output policy; public mode4 stays modal.
    bool overwrite1x{};
    bool byte_output{};
    bool primitive_drawn{},pixel_written{};
    void write(int x,int y,Pixel pigment,bool partial=false,double coverage=1){
        if(x<bounds.left||x>=bounds.right||y<bounds.top||y>=bounds.bottom)return;
        const auto index=std::size_t(y-bounds.top)*std::size_t(width)+std::size_t(x-bounds.left);
        pixel_written=true;auto& d=pixels[index];
        if(!marked.empty()&&!marked[index]){marked[index]=1;dirty.push_back(index);}
        if(partial)d=compose(d,pigment,depth,true,coverage);
        else if(mode==1&&!overwrite1x)d=compose(d,pigment,depth);
        else d=pigment;
    }
    void span(int y,std::int64_t left,std::int64_t right,Pixel pigment){
        if(mode==2||fractional4){
            const double x0=std::max(double(bounds.left),double(left)/256);
            const double x1=std::min(double(bounds.right),double(right)/256+1.0);
            if(x0>=x1)return;
            const auto first=int(std::floor(x0)),last=int(std::floor(x1));
            write(first,y,pigment,true,first+1.0-x0);
            int x=first+1;for(;x<last;++x)write(x,y,pigment);
            // sub_649BD0 advances its pointer after the first partial even
            // when both fractional endpoints floor to the same integer.
            write(x,y,pigment,true,x1-last);return;
        }
        const auto floor_fixed=[](std::int64_t n){return n>=0?n/256:-((-n+255)/256);};
        const int begin=int(std::max<std::int64_t>(bounds.left,floor_fixed(left)));
        const int end=int(std::min<std::int64_t>(bounds.right,floor_fixed(right)+1));
        for(int x=begin;x<end;++x)write(x,y,pigment);
    }
};
struct PointRouter {
    VectorBounds raw_clip,destination_block;
    const VectorRenderContext& context;
    std::vector<IPoint> points;
    std::array<std::map<int,bool>,4> boundary; // left, top, right, bottom
    IPoint last{},first{};
    bool started{},inside_run{},last_valid{};
    PointRouter(VectorBounds raw,VectorBounds destination,const VectorRenderContext& ctx)
        :raw_clip(raw),destination_block(destination),context(ctx){}
    bool inside(IPoint p)const{return p.x>=raw_clip.left&&p.x<raw_clip.right&&p.y>=raw_clip.top&&p.y<raw_clip.bottom;}
    IPoint quantize(IPoint p,bool oversampled=true)const{
        const double factor=(1.0/context.base_scale)*(oversampled?context.oversample:1);
        const double m=oversampled?context.oversample:1;
        if(context.clipboard_affine){const auto q=vector_render_detail::clipboard_quantize({double(p.x),double(p.y)},context,unsigned(m));return {int(q.x),int(q.y)};}
        return {int(std::floor(p.x*factor)-context.origin.x*m),int(std::floor(p.y*factor)-context.origin.y*m)};
    }
    void append(IPoint raw){auto p=quantize(raw);if(points.empty()||points.back()!=p)points.push_back(p);}
    void sentinel(){points.push_back({0,std::numeric_limits<int>::max()});}
    std::vector<IPoint> intersections(IPoint a,IPoint b)const{
        // sub_5963C0 evaluates L,R,T,B against the original raw segment.
        std::vector<IPoint> result;result.reserve(2);
        const VectorBounds box{std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x)+1,std::max(a.y,b.y)+1};
        if(a==b||raw_clip.left>=box.right||raw_clip.right<=box.left||raw_clip.top>=box.bottom||raw_clip.bottom<=box.top)return result;
        if(a.x==b.x){
            if(box.top<=raw_clip.top&&box.bottom>raw_clip.top)result.push_back({a.x,raw_clip.top});
            if(box.top<=raw_clip.bottom&&box.bottom>raw_clip.bottom)result.push_back({a.x,raw_clip.bottom});
        }else if(a.y==b.y){
            if(box.left<=raw_clip.left&&box.right>raw_clip.left)result.push_back({raw_clip.left,a.y});
            if(box.left<=raw_clip.right&&box.right>raw_clip.right)result.push_back({raw_clip.right,a.y});
        }else{
            const double slope=double(a.y-b.y)/double(a.x-b.x),inverse=1.0/slope;
            const auto vertical=[&](int x){const double y=double(x-a.x)*slope+a.y;if(y>=raw_clip.top&&y<=raw_clip.bottom)result.push_back({x,int(std::floor(y))});};
            if(box.left<raw_clip.left)vertical(raw_clip.left);
            if(box.right>raw_clip.right)vertical(raw_clip.right);
            const auto horizontal=[&](int y){
                const double x=double(y-a.y)*inverse+a.x;
                if((x>raw_clip.left&&x<raw_clip.right)||((x==raw_clip.left||x==raw_clip.right)&&result.empty()))result.push_back({int(std::floor(x)),y});
            };
            if(box.top<raw_clip.top)horizontal(raw_clip.top);
            if(box.bottom>raw_clip.bottom)horizontal(raw_clip.bottom);
        }
        if(result.size()>2)throw std::invalid_argument("Unsupported ambiguous vector boundary intersection");
        return result;
    }
    void event(IPoint p){
        // sub_5DFDB0 snaps to and removes the first opposite-kind boundary
        // event within one raw unit, before sub_597670 quantization.
        const bool kind=!inside_run;bool matched=false;
        for(unsigned side:{0u,2u,1u,3u}){
            const bool nearby=side==0?p.x<=raw_clip.left+1:side==2?p.x>=raw_clip.right-1:
                              side==1?p.y<=raw_clip.top+1:p.y>=raw_clip.bottom-1;
            if(!nearby)continue;
            const int coordinate=side==0||side==2?p.y:p.x;
            auto& bucket=boundary[side];auto i=bucket.lower_bound(coordinate-1);
            for(;i!=bucket.end()&&i->first<=coordinate+1;++i)if(i->second!=kind){
                if(side==0||side==2){p.x=side==0?raw_clip.left:raw_clip.right;p.y=i->first;}
                else{p.y=side==1?raw_clip.top:raw_clip.bottom;p.x=i->first;}
                bucket.erase(i);matched=true;break;
            }
            if(matched)break;
        }
        if(!matched){
            if(p.x==raw_clip.left)boundary[0][p.y]=kind;
            else if(p.x==raw_clip.right)boundary[2][p.y]=kind;
            else if(p.y==raw_clip.top)boundary[1][p.x]=kind;
            else if(p.y==raw_clip.bottom)boundary[3][p.x]=kind;
        }
        append(p);const bool old=inside_run;inside_run=!inside_run;if(old)sentinel();
    }
    void transition(IPoint p){
        // sub_5E0E60 deliberately constructs new -> previous, then selects
        // the non-endpoint crossing when the helper returns two candidates.
        auto hit=intersections(p,last);
        if(hit.size()==1)event(hit[0]);
        else if(hit.size()==2){
            if(hit[0]==p||hit[0]==last)event(hit[1]);
            else if(hit[1]==p||hit[1]==last)event(hit[0]);
        }
    }
    void outside_segment(IPoint p){
        auto hit=intersections(last,p);if(hit.size()!=2)return;
        const auto a=quantize(hit[0],false),b=quantize(hit[1],false);const auto d=destination_block;
        if(a.x==b.x&&(a.x==d.left||a.x==d.right)&&!((a.y==d.top&&b.y==d.bottom)||(a.y==d.bottom&&b.y==d.top)))return;
        if(a.y==b.y&&(a.y==d.top||a.y==d.bottom)&&!((a.x==d.left&&b.x==d.right)||(a.x==d.right&&b.x==d.left)))return;
        const int dx=p.x-last.x,dy=p.y-last.y;
        const auto parameter=[&](IPoint q){return float(std::abs(dx)>std::abs(dy)?double(q.x-last.x)/dx:double(q.y-last.y)/dy);};
        const float ta=parameter(hit[0]),tb=parameter(hit[1]);
        if(ta<tb){event(hit[0]);event(hit[1]);}else if(ta>tb){event(hit[1]);event(hit[0]);}
    }
    void route(IPoint p){
        if(!started){started=true;last_valid=true;first=last=p;inside_run=inside(p);if(inside_run)append(p);return;}
        if(p==last)return;
        if(inside(p)){if(!inside_run)transition(p);append(p);}
        else if(inside_run)transition(p);
        else if(last_valid)outside_segment(p);
        last=p;last_valid=true;
    }
    void close(){
        if(!started)return;
        if(inside(last)){
            if(inside(first)){append(first);sentinel();}else transition(first);
        }else if(inside(first)){sentinel();transition(first);append(first);sentinel();}
        else if(last_valid&&last!=first)outside_segment(first);
        inside_run=false;started=false;last_valid=false;
    }
    void flatten(unsigned side){
        const bool vertical=side==0||side==2,flag=side==1||side==2;
        const int fixed=side==0?raw_clip.left:side==2?raw_clip.right:side==1?raw_clip.top:raw_clip.bottom;
        const auto point=[&](int coordinate){return vertical?IPoint{fixed,coordinate}:IPoint{coordinate,fixed};};
        auto& bucket=boundary[side];auto i=bucket.begin();if(i==bucket.end())return;
        bool extend=flag?i->second:!i->second;
        while(i!=bucket.end()){
            if(extend){append(point(vertical?raw_clip.top:raw_clip.left));extend=false;}
            else{if(i->second!=flag)append(point(i->first));++i;}
            if(i==bucket.end())append(point(vertical?raw_clip.bottom:raw_clip.right));
            else{if(i->second==flag)append(point(i->first));++i;}
            sentinel();
        }
    }
    void synthesize(unsigned side){
        const auto nonempty=[&](unsigned s){return !boundary[s].empty();};
        const auto first_kind=[&](unsigned s){return boundary[s].begin()->second;};
        const auto last_kind=[&](unsigned s){return boundary[s].rbegin()->second;};
        bool emit=false;
        if(side==0||side==2){
            const bool left=side==0;
            if(nonempty(1))emit=left?first_kind(1):!last_kind(1);
            else if(nonempty(3))emit=left?!first_kind(3):last_kind(3);
            else if(nonempty(0))emit=!first_kind(0);
            else if(nonempty(2))emit=first_kind(2);
            if(emit){const int x=left?raw_clip.left:raw_clip.right;append({x,raw_clip.top});append({x,raw_clip.bottom});sentinel();}
        }else{
            const bool top=side==1;
            if(nonempty(0))emit=top?!first_kind(0):last_kind(0);
            else if(nonempty(2))emit=top?first_kind(2):!last_kind(2);
            else if(nonempty(1))emit=first_kind(1);
            else if(nonempty(3))emit=!first_kind(3);
            if(emit){const int y=top?raw_clip.top:raw_clip.bottom;append({raw_clip.left,y});append({raw_clip.right,y});sentinel();}
        }
    }
    void finish(){
        close();if(points.empty())return;
        if(std::all_of(boundary.begin(),boundary.end(),[](const auto& b){return b.empty();}))return;
        for(unsigned side=0;side<4;++side){if(boundary[side].empty())synthesize(side);else flatten(side);}
    }
};
std::vector<IPoint> clipped_points(const Outline& shape,const VectorBounds& raw_clip,
                                  const VectorBounds& destination_block,const VectorRenderContext& ctx,std::stop_token stop) {
    PointRouter router{raw_clip,destination_block,ctx};router.points.reserve(shape.points.size()+8);
    for(std::size_t i=0;i<shape.points.size();++i){if(!(i%256))cancelled(stop);router.route(shape.points[i]);}
    router.finish();return std::move(router.points);
}
VectorBounds raw_block(VectorBounds block,const VectorRenderContext& ctx){
    const double pad=std::trunc(ctx.base_scale*(ctx.oversample==4?0.75:ctx.oversample==2?0.5:0.0));
    if(ctx.clipboard_affine)return vector_render_detail::clipboard_raw_block(block,ctx);
    return {int(std::floor((block.left+ctx.origin.x)*ctx.base_scale)),
            int(std::floor((block.top+ctx.origin.y)*ctx.base_scale)),
            int(std::floor((block.right+ctx.origin.x)*ctx.base_scale)+pad),
            int(std::floor((block.bottom+ctx.origin.y)*ctx.base_scale)+pad)};
}
bool needs_cap_fallback(const Outline& shape,const VectorBounds& raw_clip){
    if(shape.circle)return false;
    const auto& s=*shape.span;
    return (s.round_start&&!(shape.profiles&&shape.profiles->start)&&!cap_intersects(s.start,emitted_width(s,0,shape.minimum),raw_clip))||
           (s.round_end&&!(shape.profiles&&shape.profiles->end)&&!cap_intersects(s.end,emitted_width(s,1,shape.minimum),raw_clip));
}
VectorBounds emission_bounds(const VectorLineSpan& s,const VectorSpanProfiles* profiles){
    const bool linear=is_linear(s);double radius=s.half_width;
    if(!s.fixed_width){double maximum=0;for(const auto& n:s.width_nodes)maximum=std::max(maximum,n.scale);radius*=maximum;}
    const int inflate=int(std::trunc(radius))+1;
    const double left=linear?std::min(s.start.x,s.end.x):std::min({s.start.x,s.control.x,s.end.x});
    const double top=linear?std::min(s.start.y,s.end.y):std::min({s.start.y,s.control.y,s.end.y});
    const double right=linear?std::max(s.start.x,s.end.x):std::max({s.start.x,s.control.x,s.end.x});
    const double bottom=linear?std::max(s.start.y,s.end.y):std::max({s.start.y,s.control.y,s.end.y});
    VectorBounds result{int(left)-inflate,int(top)-inflate,int(right)+1+inflate,int(bottom)+1+inflate};
    const auto extend=[&](VectorPoint p){
        result.left=std::min(result.left,int(p.x));result.top=std::min(result.top,int(p.y));
        result.right=std::max(result.right,int(p.x)+1);result.bottom=std::max(result.bottom,int(p.y)+1);
    };
    if(profiles){if(profiles->start)profile_bound_points(*profiles->start,extend);if(profiles->end)profile_bound_points(*profiles->end,extend);}
    return result;
}
VectorBounds raw_emission_bounds(const Outline& shape){return shape.circle?shape.circle->gate:emission_bounds(*shape.span,shape.profiles);}
bool intersects(VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;}
bool walkers_intersect(const Outline& shape,VectorBounds raw){
    return !shape.walker||(intersects(shape.walker->line,raw)&&intersects(shape.walker->model,raw));
}
VectorPoint native_delta(VectorPoint a,VectorPoint b){
    const auto checked=[](double v){
        if(!std::isfinite(v)||v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())
            throw std::invalid_argument("Unsupported vector analytic integer difference");
        return v;
    };
    return {checked(a.x-b.x),checked(a.y-b.y)};
}
int native_dot(VectorPoint a,VectorPoint b){
    const auto x=std::int64_t(a.x)*std::int64_t(b.x),y=std::int64_t(a.y)*std::int64_t(b.y);
    if(y>0&&x>std::numeric_limits<std::int64_t>::max()-y)return std::numeric_limits<int>::max();
    if(y<0&&x<std::numeric_limits<std::int64_t>::min()-y)return -std::numeric_limits<int>::max();
    const auto value=x+y;
    return int(std::clamp(value,-std::int64_t(std::numeric_limits<int>::max()),std::int64_t(std::numeric_limits<int>::max())));
}
int native_distance_squared(VectorPoint a,VectorPoint b){
    const auto d=native_delta(a,b);return native_dot(d,d);
}
struct Nearest {VectorPoint point;float parameter{};};
Nearest nearest_line(VectorPoint a,VectorPoint b,VectorPoint p){
    const auto direction=native_delta(b,a),offset=native_delta(p,a);
    const auto dot=native_dot(direction,offset);
    if(dot<=0)return {a,0};
    const auto square=native_distance_squared(a,b);
    // Saturation happens before the native endpoint gate, even when a later
    // double recomputation could otherwise place the projection inside.
    if(dot>=square)return {b,1};
    double t=double(dot)/square;
    if(square==std::numeric_limits<int>::max()){
        const double numerator=offset.y*direction.y+offset.x*direction.x;
        const double denominator=direction.y*direction.y+direction.x*direction.x;
        if(numerator>denominator)return {b,1};
        t=numerator/denominator;
    }
    const float parameter=float(t);const double u=1.0-parameter;
    return {{std::floor(a.x*u+b.x*parameter),std::floor(a.y*u+b.y*parameter)},parameter};
}
Nearest nearest_curve(const VectorLineSpan& s,VectorPoint p,std::uint64_t& work,std::stop_token stop){
    if(native_dot(native_delta(s.control,s.start),native_delta(p,s.start))<=0)return {s.start,0};
    if(native_dot(native_delta(s.control,s.end),native_delta(p,s.end))<=0)return {s.end,1};
    const auto n=steps(s,64);charge(work,std::uint64_t(n)*64);
    const float h=float(1.0/double(n));float previous_parameter=0;
    auto previous=s.start;Nearest best{s.start,0};int distance=std::numeric_limits<int>::max();
    for(std::size_t i=1;i<=n;++i){
        if(!(i%64))cancelled(stop);
        const float parameter=float(double(i)*h);const auto continuous=position(s,parameter);
        const VectorPoint sample{std::floor(continuous.x),std::floor(continuous.y)};
        const auto candidate=nearest_line(previous,sample,p);
        const int square=native_distance_squared(candidate.point,p);
        if(square<distance){
            distance=square;best.point=candidate.point;
            const double t=double(candidate.parameter)*h+previous_parameter;
            best.parameter=t>1.0?1.0f:float(t);
        }
        previous=sample;previous_parameter=parameter;
    }
    return best;
}
double analytic_width_scale(const VectorLineSpan& s,float t){
    if(s.fixed_width||s.width_nodes.empty())return 1;
    if(t<=0)return s.width_nodes.front().scale;
    if(t>=1)return s.width_nodes.back().scale;
    if(s.width_nodes.size()==1)return 1;
    const auto right=std::upper_bound(s.width_nodes.begin(),s.width_nodes.end(),t,
        [](double v,const VectorWidthNode& n){return v<n.parameter;});
    if(right==s.width_nodes.end())return 1;
    const auto& a=*(right-1);const auto& b=*right;
    return ((b.parameter-t)*a.scale+(t-a.parameter)*b.scale)/(b.parameter-a.parameter);
}
bool no_point_fallback(const Outline& shape,const VectorBounds& clip,std::uint64_t& work,std::stop_token stop){
    if(shape.circle){const auto p=shape.circle->center;return p.x>=clip.left&&p.x<clip.right&&p.y>=clip.top&&p.y<clip.bottom;}
    const auto& s=*shape.span;const auto bounds=emission_bounds(s,shape.profiles);
    const double base=shape.minimum?effective_base(s,shape.minimum):s.half_width;
    if(bounds.left>clip.left||bounds.right<clip.right||bounds.top>clip.top||bounds.bottom<clip.bottom||
       (clip.right-clip.left>2*base&&clip.bottom-clip.top>2*base))return false;
    charge(work,64);cancelled(stop);
    // RCRect::CenterPoint adds DWORDs, reinterprets the wrapped signed sum,
    // then truncates division toward zero (rcwCmn export 915).
    const auto midpoint=[](int a,int b){
        const auto sum=std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b));
        return double(std::int64_t(sum)/2);
    };
    const VectorPoint center{midpoint(clip.left,clip.right),midpoint(clip.top,clip.bottom)};
    const auto nearest=is_linear(s)?nearest_line(s.start,s.end,center):nearest_curve(s,center,work,stop);
    const auto d=native_delta(center,nearest.point);
    if(std::sqrt(d.x*d.x+d.y*d.y)>base*analytic_width_scale(s,nearest.parameter))return false;
    if(nearest.parameter==0&&native_dot(native_delta(s.control,s.start),native_delta(center,s.start))<=0)return false;
    if(nearest.parameter==1&&native_dot(native_delta(s.control,s.end),native_delta(center,s.end))<=0)return false;
    return true;
}
bool fills_block(const Outline& shape,VectorBounds block){
    return std::any_of(shape.full_blocks.begin(),shape.full_blocks.end(),[&](VectorBounds b){
        return b.left==block.left&&b.top==block.top&&b.right==block.right&&b.bottom==block.bottom;
    });
}
void check_boundary_admission(std::vector<Outline>& shapes,const VectorRenderContext& ctx,
                              Admission& admission,std::stop_token stop,ContextProfile context_profile=ContextProfile::Color){
    const auto c=admission.clip;if(c.right<=c.left||c.bottom<=c.top)return;
    for(int ty=c.top/256;ty*256<c.bottom;++ty)for(int tx=c.left/256;tx*256<c.right;++tx){
        cancelled(stop);const VectorBounds block=bit_profile(context_profile)?
            VectorBounds{std::max(c.left,tx*256),std::max(c.top,ty*256),std::min(c.right,(tx+1)*256),std::min(c.bottom,(ty+1)*256)}:
            VectorBounds{tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)};
        const auto raw=raw_block(block,ctx);
        for(auto& shape:shapes){
            if(!walkers_intersect(shape,raw))continue;
            if(!intersects(raw_emission_bounds(shape),raw))continue;
            const auto* active=&shape;std::optional<Outline> replacement;
            if(needs_cap_fallback(shape,raw)){replacement=outline(*shape.span,shape.color,ctx,admission.curve_step,stop,&raw,shape.profiles,shape.minimum);replacement->walker=shape.walker;active=&*replacement;}
            if(clipped_points(*active,raw,block,ctx,stop).empty()&&no_point_fallback(*active,raw,admission.work,stop))
                shape.full_blocks.push_back(block);
        }
    }
}
std::vector<Outline> outlines(const VectorLineGeometry& geometry,const VectorRenderContext& ctx,
                              const Admission& admission,std::stop_token stop,std::span<const VectorSpanProfiles> profiles={}){
    std::vector<Outline> result;
    std::vector<std::size_t> starts;
    if(!profiles.empty()){
        starts.reserve(geometry.strokes.size());std::size_t count=0;
        for(const auto& stroke:geometry.strokes){starts.push_back(count);count+=stroke.spans.size();}
    }
    const auto emit=[&](const VectorLineStroke& stroke){
        std::size_t index=profiles.empty()?0:starts[std::size_t(&stroke-geometry.strokes.data())];
        for(const auto& s:stroke.spans){
            cancelled(stop);const auto* p=profiles.empty()?nullptr:&profiles[index++];
            result.push_back(outline(s,color(stroke.color,ctx.depth),ctx,admission.curve_step,stop,nullptr,p));
            const auto gate=admission.walker.find(&s);if(gate!=admission.walker.end())result.back().walker=gate->second;
        }
    };
    // sub_6453C0 walks the saved line chain backwards only for the direct
    // compose context (mode 1 and ctx+630=1), and preserves span order.
    if(ctx.oversample==1)for(auto i=geometry.strokes.rbegin();i!=geometry.strokes.rend();++i)emit(*i);
    else for(const auto& stroke:geometry.strokes)emit(stroke);
    return result;
}
template<class Surface> void fill(const Outline& shape,Surface& scratch,const VectorBounds& raw_clip,const VectorBounds& destination_block,
          const VectorRenderContext& ctx,std::stop_token stop,std::optional<Pixel> override_color={}) {
    const Pixel pigment=override_color.value_or(shape.color);
    if(!walkers_intersect(shape,raw_clip))return;
    if(fills_block(shape,destination_block)){
        if constexpr(requires{scratch.primitive_drawn;})scratch.primitive_drawn=true;
        // sub_5DFD30 paints the entire scratch clip and marks it dirty. This
        // bypasses outline bounds and retains normal direct/AA composition.
        for(int y=scratch.bounds.top;y<scratch.bounds.bottom;++y){
            if(!(y%32))cancelled(stop);
            for(int x=scratch.bounds.left;x<scratch.bounds.right;++x)scratch.write(x,y,pigment);
        }
        return;
    }
    if(!intersects(raw_emission_bounds(shape),raw_clip))return;
    if(!ctx.clipboard_affine&&(shape.bounds.left>=scratch.bounds.right||shape.bounds.right<=scratch.bounds.left||
       shape.bounds.top>=scratch.bounds.bottom||shape.bounds.bottom<=scratch.bounds.top))return;
    const auto points=clipped_points(shape,raw_clip,destination_block,ctx,stop);
    if constexpr(requires{scratch.primitive_drawn;})scratch.primitive_drawn|=!points.empty();
    struct Edge {int y0{},y1{};std::int64_t x{},slope{};};std::vector<Edge> edges;
    for(std::size_t i=1;i<points.size();++i){
        auto a=points[i-1],b=points[i];if(a.y==b.y||a.y==std::numeric_limits<int>::max()||b.y==std::numeric_limits<int>::max())continue;
        if(a.y>b.y)std::swap(a,b);
        edges.push_back({a.y,b.y,std::int64_t(a.x)*256,std::int64_t(b.x-a.x)*256/(b.y-a.y)});
    }
    std::vector<std::int64_t> active;active.reserve(edges.size());
    int scan_begin=std::max(scratch.bounds.top,shape.bounds.top),scan_end=std::min(scratch.bounds.bottom,shape.bounds.bottom);
    if constexpr(requires{scratch.begin_scanlines(0);}){
        //5DEB60 direct low-depth establishes its cursor before scanning,
        //visits the maximum edge Y once even with no active edges, then
        //advances the cursor. No edge records bypass equal-Y cleanup too.
        if(edges.empty())return;
        int first_y=edges.front().y0,last_y=edges.front().y1;
        for(const auto& edge:edges){first_y=std::min(first_y,edge.y0);last_y=std::max(last_y,edge.y1);}
        scratch.begin_scanlines(first_y);
        scan_begin=std::max(scratch.bounds.top,first_y);scan_end=std::min(scratch.bounds.bottom,last_y+1);
    }
    for(int y=scan_begin;y<scan_end;++y){
        if(!(y%32))cancelled(stop);active.clear();
        for(const auto& e:edges)if(y>=e.y0&&y<e.y1)active.push_back(e.x+std::int64_t(y-e.y0)*e.slope);
        std::sort(active.begin(),active.end());
        for(std::size_t i=1;i<active.size();i+=2)scratch.span(y,active[i-1],active[i],pigment);
        if constexpr(requires{scratch.next_scanline();})scratch.next_scanline();
    }
    // The emitter flushes with inclusive-edge flag 1; its scanline backend
    // therefore performs the separate equal-Y cleanup after active edges.
    for(std::size_t i=1;i<points.size();++i){
        auto a=points[i-1],b=points[i];
        if(a.y==b.y&&a.y!=std::numeric_limits<int>::max()&&a.y>=scratch.bounds.top&&a.y<scratch.bounds.bottom){
            if constexpr(requires{scratch.horizontal_scanline(0);})scratch.horizontal_scanline(a.y);
            if(a.x!=b.x)scratch.span(a.y,std::int64_t(std::min(a.x,b.x))*256,std::int64_t(std::max(a.x,b.x))*256,pigment);
        }
    }
}
Pixel resolve(const Scratch& scratch,int x,int y) {
    const auto m=scratch.mode;
    if(m==1)return scratch.pixels[std::size_t(y)*std::size_t(scratch.width)+std::size_t(x)];
    std::array<Pixel,16> cell{};unsigned count=0;
    for(unsigned j=0;j<m;++j)for(unsigned i=0;i<m;++i)
        cell[count++]=scratch.pixels[(std::size_t(y)*m+j)*std::size_t(scratch.width)+std::size_t(x)*m+i];
    if(m==4&&!scratch.fractional4){
        unsigned best=0,best_count=0;
        for(unsigned i=0;i<count;++i){unsigned n=0;for(unsigned j=0;j<count;++j)n+=cell[j]==cell[i];if(n>best_count){best=i;best_count=n;}}
        return cell[best];
    }
    std::uint64_t alpha=0,weight=0,r=0,g=0,b=0;
    for(unsigned i=0;i<count;++i){
        const auto& p=cell[i];const unsigned w=p.a>>8;
        alpha+=scratch.depth==32?p.a/257:p.a;weight+=w;
        r+=std::uint64_t(scratch.depth==32?p.r/257:p.r)*w;
        g+=std::uint64_t(scratch.depth==32?p.g/257:p.g)*w;
        b+=std::uint64_t(scratch.depth==32?p.b/257:p.b)*w;
    }
    if(!weight)return {};
    const unsigned expand=scratch.depth==32?257:1;
    return {std::uint16_t((r/weight)*expand),std::uint16_t((g/weight)*expand),
            std::uint16_t((b/weight)*expand),std::uint16_t((alpha>>m)*expand)};
}
}
bool can_render_vector_line_geometries(std::span<const VectorLineGeometry> geometries,const VectorRenderContext& ctx) {
    try{
        auto admission=admit(geometries,ctx);
        for(const auto& geometry:geometries){auto shapes=outlines(geometry,ctx,admission,{});check_boundary_admission(shapes,ctx,admission,{});}
        return true;
    }catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}
}
namespace {
template<class Surface> void draw_outline(const Outline& shape,Surface& scratch,VectorBounds raw,VectorBounds block,const VectorRenderContext& ctx,const Admission& admission,std::stop_token stop,std::optional<Pixel> pigment={}){
    cancelled(stop);
    if(!fills_block(shape,block)&&needs_cap_fallback(shape,raw)){
        auto replacement=outline(*shape.span,shape.color,ctx,admission.curve_step,stop,&raw,shape.profiles,shape.minimum);replacement.walker=shape.walker;
        fill(replacement,scratch,raw,block,ctx,stop,pigment);
    }else fill(shape,scratch,raw,block,ctx,stop,pigment);
}
template<class Draw> Raster render_outlines(const std::vector<std::vector<Outline>>& planes,const VectorRenderContext& ctx,const Admission& admission,std::stop_token stop,Draw draw,bool fractional4=false,bool overwrite1x=false,bool byte_output=false,std::set<TileCoord>* resolved=nullptr,std::set<TileCoord>* touched_blocks=nullptr,bool fresh_word_cache=false){
    cancelled(stop);
    const bool retain_a0=fresh_word_cache&&ctx.depth==64&&ctx.oversample==1;
    Raster result(ctx.width,ctx.height,ctx.depth,ctx.resolution);const auto c=admission.clip;
    if(planes.empty()||c.right<=c.left||c.bottom<=c.top)return result;
    for(int ty=c.top/256;ty*256<c.bottom;++ty)for(int tx=c.left/256;tx*256<c.right;++tx){
        cancelled(stop);
        // Pixel queries and output clips use the same native 256-pixel block
        // router as a full render; they restrict publication after resolve.
        VectorBounds block{tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)};
        const auto destination_block=block;const auto raw_clip=raw_block(block,ctx);
        const int m=int(ctx.oversample);
        VectorBounds touched{block.right,block.bottom,block.left,block.top};
        for(const auto& shapes:planes)for(const auto& shape:shapes){
            if(fills_block(shape,destination_block)){touched=destination_block;continue;}
            const auto& b=shape.bounds;
            const auto floor_div=[&](int v){return v>=0?v/m:-((-v+m-1)/m);};
            const int left=std::max(block.left,floor_div(b.left)),top=std::max(block.top,floor_div(b.top));
            const int right=std::min(block.right,floor_div(b.right-1)+1),bottom=std::min(block.bottom,floor_div(b.bottom-1)+1);
            if(left<right&&top<bottom){touched.left=std::min(touched.left,left);touched.top=std::min(touched.top,top);touched.right=std::max(touched.right,right);touched.bottom=std::max(touched.bottom,bottom);}
        }
        if(!ctx.clipboard_affine)block=touched;
        const int w=block.right-block.left,h=block.bottom-block.top;if(w<=0||h<=0)continue;
        Scratch scratch{{block.left*m,block.top*m,block.right*m,block.bottom*m},w*m,h*m,ctx.depth,ctx.oversample,{},{},{},fractional4};
        // Temp's actual WORD normal1 cache is a present white/A0 surface.
        //648000 can write RGB while narrowing combined alpha to zero.
        //Retain those samples for the later opaque/LUT print terminal.
        scratch.overwrite1x=overwrite1x;scratch.byte_output=byte_output;scratch.pixels.resize(std::size_t(w*m)*std::size_t(h*m),retain_a0?Pixel{65535,65535,65535,0}:Pixel{0,0,0,0});
        std::vector<Pixel> companion;
        if(m>1){companion.resize(scratch.pixels.size(),Pixel{0,0,0,0});scratch.marked.resize(scratch.pixels.size());}
        for(std::size_t plane=0;plane<planes.size();++plane){
            draw(plane,scratch,raw_clip,destination_block);
            if(m>1){
                for(std::size_t n=0;n<scratch.dirty.size();++n){
                    if(!(n%256))cancelled(stop);const auto index=scratch.dirty[n];
                    companion[index]=byte_output?compose_byte(companion[index],scratch.pixels[index]):compose(companion[index],scratch.pixels[index],ctx.depth);
                    scratch.pixels[index]={0,0,0,0};scratch.marked[index]=0;
                }
                scratch.dirty.clear();
            }
        }
        if(resolved&&scratch.primitive_drawn)resolved->insert({tx,ty});if(touched_blocks&&scratch.pixel_written)touched_blocks->insert({tx,ty});
        if(m>1)scratch.pixels=std::move(companion);
        for(int y=0;y<h;++y){if(!(y%32))cancelled(stop);for(int x=0;x<w;++x){
            if(block.left+x<c.left||block.left+x>=c.right||block.top+y<c.top||block.top+y>=c.bottom)continue;
            auto p=resolve(scratch,x,y);if(p.a||retain_a0)result.set_pixel(block.left+x,block.top+y,p);
        }}
    }
    cancelled(stop);return result;
}
Raster render_geometries(std::span<const VectorLineGeometry> geometries,std::span<const VectorSpanProfiles> profiles,
                         const VectorRenderContext& ctx,std::stop_token stop) {
    cancelled(stop);auto admission=profiles.empty()?admit(geometries,ctx):admit_profiled(geometries.front(),profiles,ctx,stop);cancelled(stop);
    std::vector<std::vector<Outline>> planes;
    for(auto i=geometries.rbegin();i!=geometries.rend();++i){
        planes.push_back(outlines(*i,ctx,admission,stop,profiles));
        check_boundary_admission(planes.back(),ctx,admission,stop);
    }
    return render_outlines(planes,ctx,admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){
        for(const auto& shape:planes[p])draw_outline(shape,scratch,raw,block,ctx,admission,stop);
    });
}
}
Raster render_vector_line_geometries(std::span<const VectorLineGeometry> geometries,const VectorRenderContext& ctx,std::stop_token stop) {
    return render_geometries(geometries,{},ctx,stop);
}
Pixel vector_line_geometries_pixel(std::span<const VectorLineGeometry> geometries,const VectorRenderContext& ctx,int x,int y,std::stop_token stop) {
    cancelled(stop);admit(geometries,ctx);
    if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds clip{x,y,x+1,y+1};
    if(ctx.clip)clip={std::max(clip.left,ctx.clip->left),std::max(clip.top,ctx.clip->top),std::min(clip.right,ctx.clip->right),std::min(clip.bottom,ctx.clip->bottom)};
    if(clip.right<=clip.left||clip.bottom<=clip.top)return {};
    local.clip=clip;return render_vector_line_geometries(geometries,local,stop).pixel(x,y);
}
bool can_render_vector_line_geometry(const VectorLineGeometry& geometry,const VectorRenderContext& ctx){return can_render_vector_line_geometries({&geometry,1},ctx);}
Raster render_vector_line_geometry(const VectorLineGeometry& geometry,const VectorRenderContext& ctx,std::stop_token stop){return render_vector_line_geometries({&geometry,1},ctx,stop);}
Pixel vector_line_geometry_pixel(const VectorLineGeometry& geometry,const VectorRenderContext& ctx,int x,int y,std::stop_token stop){return vector_line_geometries_pixel({&geometry,1},ctx,x,y,stop);}
bool can_render_vector_line_geometry(const VectorLineGeometry& geometry,std::span<const VectorSpanProfiles> profiles,const VectorRenderContext& ctx){
    if(profiles.empty())return can_render_vector_line_geometry(geometry,ctx);
    try{
        auto admission=admit_profiled(geometry,profiles,ctx);auto shapes=outlines(geometry,ctx,admission,{},profiles);
        check_boundary_admission(shapes,ctx,admission,{});return true;
    }catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}
}
Raster render_vector_line_geometry(const VectorLineGeometry& geometry,std::span<const VectorSpanProfiles> profiles,
                                  const VectorRenderContext& ctx,std::stop_token stop){
    if(profiles.empty())return render_vector_line_geometry(geometry,ctx,stop);
    return render_geometries({&geometry,1},profiles,ctx,stop);
}
Pixel vector_line_geometry_pixel(const VectorLineGeometry& geometry,std::span<const VectorSpanProfiles> profiles,
                                 const VectorRenderContext& ctx,int x,int y,std::stop_token stop){
    if(profiles.empty())return vector_line_geometry_pixel(geometry,ctx,x,y,stop);
    cancelled(stop);admit_profiled(geometry,profiles,ctx,stop);
    if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds clip{x,y,x+1,y+1};
    if(ctx.clip)clip={std::max(clip.left,ctx.clip->left),std::max(clip.top,ctx.clip->top),
                      std::min(clip.right,ctx.clip->right),std::min(clip.bottom,ctx.clip->bottom)};
    if(clip.right<=clip.left||clip.bottom<=clip.top)return {};
    local.clip=clip;return render_vector_line_geometry(geometry,profiles,local,stop).pixel(x,y);
}
namespace {
bool ruler_intersects(VectorBounds gate,VectorBounds raw){return gate.left<gate.right&&gate.top<gate.bottom&&raw.left<raw.right&&raw.top<raw.bottom&&intersects(gate,raw);}
struct RulerRange {VectorBounds gate;std::optional<std::uint64_t> color;std::size_t begin{},end{};};
struct RulerPlan {Admission admission;std::vector<std::vector<Outline>> planes;std::vector<std::vector<RulerRange>> ranges;std::vector<std::uint64_t> colors;std::uint64_t storage{};};
VectorBounds circle_gate(const VectorRulerCommand& c){
    const auto coordinate=[](std::int64_t v){if(v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())throw std::invalid_argument("Ruler circle bounds overflow");return int(v);};
    return {coordinate(std::int64_t(c.center_x)-c.radius),coordinate(std::int64_t(c.center_y)-c.radius),coordinate(std::int64_t(c.center_x)+c.radius),coordinate(std::int64_t(c.center_y)+c.radius)};
}
Outline circle_outline(const VectorRulerCommand& c,const VectorRenderContext& ctx,std::stop_token stop){
    Outline out;out.circle=Circle{{double(c.center_x),double(c.center_y)},float(c.radius),c.gate};out.walker=WalkerGates{c.gate,c.gate};
    const int count=2*int(std::trunc(double(float(c.radius))/ctx.base_scale))+4;
    const float delta=float(numeric_detail::binary64(360.0/count));
    const double scale=(1.0/ctx.base_scale)*ctx.oversample;
    out.bounds={std::numeric_limits<int>::max(),std::numeric_limits<int>::max(),std::numeric_limits<int>::min(),std::numeric_limits<int>::min()};
    for(int i=0;i<=count;++i){
        if(!(i%64))cancelled(stop);
        const auto radians=numeric_detail::binary64(numeric_detail::binary64(double(i)*delta)*double(0.017453292f));
        const auto dx=std::floor(numeric_detail::binary64(std::cos(radians)*float(c.radius)));
        const auto dy=std::floor(double(float(numeric_detail::binary64(std::sin(radians)*float(c.radius)))));
        append(out.points,{double(c.center_x)+dx,double(c.center_y)+dy},ctx);
    }
    if(out.points.size()>1&&out.points.back()!=out.points.front())out.points.push_back(out.points.front());
    for(auto raw:out.points){const auto converted=ctx.clipboard_affine?vector_render_detail::clipboard_quantize({double(raw.x),double(raw.y)},ctx,ctx.oversample):VectorPoint{std::floor(raw.x*scale)-ctx.origin.x*ctx.oversample,std::floor(raw.y*scale)-ctx.origin.y*ctx.oversample};const IPoint p{int(converted.x),int(converted.y)};
        out.bounds.left=std::min(out.bounds.left,p.x);out.bounds.top=std::min(out.bounds.top,p.y);out.bounds.right=std::max(out.bounds.right,p.x+1);out.bounds.bottom=std::max(out.bounds.bottom,p.y+1);}
    return out;
}
template<class Plane> RulerPlan prepare_rulers(std::span<const Plane> geometries,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t previous_work=0,bool fractional4=false,bool sparse_executor=false,ContextProfile context_profile=ContextProfile::Color,float minimum=0,bool modal4=false,std::uint64_t previous_storage=0){
    cancelled(stop);if(ctx.oversample!=1&&ctx.oversample!=2&&!(ctx.oversample==4&&(fractional4||modal4)))throw std::invalid_argument("Ruler normal cache requires1x/2x context");
    if(geometries.size()>100000)throw std::length_error("Too many Ruler planes");
    RulerPlan out;const VectorLineGeometry empty;out.admission=admit(empty,ctx,fractional4,false,false,context_profile);charge(out.admission.work,previous_work);const auto clip=out.admission.clip;
    const VectorBounds physical{clip.left/256*256,clip.top/256*256,std::min(ctx.width,(clip.right+255)/256*256),std::min(ctx.height,(clip.bottom+255)/256*256)};
    const auto columns=clip.right>clip.left?std::uint64_t((clip.right-1)/256-clip.left/256+1):0,rows=clip.bottom>clip.top?std::uint64_t((clip.bottom-1)/256-clip.top/256+1):0;
    std::uint64_t storage=previous_storage,commands{},points{};
    const auto bytes=[&](std::uint64_t n){constexpr std::uint64_t limit=512ull*1024*1024;if(storage>limit||n>limit-storage)throw std::length_error("Ruler render storage exceeds512MiB");storage+=n;};
    bytes(columns*rows*(bit_profile(context_profile)?8192ull:65536*std::uint64_t(ctx.depth/8)));
    if(ctx.oversample==4)bytes(1024ull*1024*9);
    bytes(2*std::uint64_t(ctx.oversample==4?1024:512)*(ctx.oversample==4?1024:512)*sizeof(Pixel)+geometries.size()*(sizeof(std::vector<Outline>)+sizeof(std::vector<RulerRange>)));
    const auto list=[&](std::size_t n,std::size_t stride){if(n>100000)throw std::length_error("Ruler source collection exceeds limits");bytes(2*n*stride);};
    const auto raw_archive=[&](const VectorLineArchive& a){
        bytes(sizeof(a)+a.header_suffix.size()+a.body_suffix.size()+a.layer_suffix.size());
        const auto pool=[&](const auto& records,auto payload){list(records.size(),sizeof(typename std::decay_t<decltype(records)>::value_type));for(const auto& r:records){bytes(r.suffix.size());payload(r.value);}};
        pool(a.vertices,[&](const VectorVertex& v){list(v.subcurves.size(),sizeof(int));});pool(a.scales,[](const auto&){});
        pool(a.subcurves,[&](const VectorSubcurve& v){list(v.scales.size(),sizeof(int));});pool(a.models,[&](const VectorModelCurve& v){list(v.subcurves.size(),sizeof(int));});
        pool(a.crosspoints,[](const auto&){});pool(a.lines,[&](const VectorLine& v){list(v.members.size(),sizeof(VectorLineMember));});
    };
    const auto profiles=[](const Plane& p)->std::span<const VectorSpanProfiles>{if constexpr(requires{p.profiles;})return p.profiles;else return p.line_profiles;};
    // All line/circle occurrences, including hidden caller planes, are charged
    // before generating polygons or allocating the output raster.
    for(const auto& plane:geometries){
        cancelled(stop);bytes(plane.reserved_storage);if constexpr(requires{plane.reserved_work;})charge(out.admission.work,plane.reserved_work);bytes(sizeof(plane));raw_archive(plane.lines.archive);list(plane.lines.strokes.size(),sizeof(VectorLineStroke));list(plane.commands.size(),sizeof(VectorRulerCommand));list(plane.model_bounds.size(),64+sizeof(std::pair<const int,VectorBounds>));
        const auto ps=profiles(plane);list(ps.size(),sizeof(VectorSpanProfiles));if(!ps.empty())bytes(plane.lines.strokes.size()*sizeof(std::size_t));
        std::uint64_t maximum_copy{};
        for(const auto& stroke:plane.lines.strokes){list(stroke.spans.size(),sizeof(VectorLineSpan));std::uint64_t copy=sizeof(stroke)+stroke.spans.size()*sizeof(VectorLineSpan);
            for(const auto& span:stroke.spans){list(span.width_nodes.size(),sizeof(VectorWidthNode));copy+=span.width_nodes.size()*sizeof(VectorWidthNode);}maximum_copy=std::max(maximum_copy,copy);}
        bytes(maximum_copy);const auto source_admission=admit_profiled(plane.lines,ps,ctx,stop,ProfileSource::Stored,fractional4,sparse_executor,context_profile,minimum);
        charge(out.admission.work,source_admission.preparation_work);
        if(plane.commands.size()>100000-commands)throw std::length_error("Too many Ruler commands");commands+=plane.commands.size();bytes(plane.commands.size()*sizeof(RulerRange));
        bytes(plane.lines.strokes.size()*sizeof(Admission));std::vector<Admission> costs;costs.reserve(plane.lines.strokes.size());
        std::size_t first=0;for(const auto& stroke:plane.lines.strokes){VectorLineGeometry g;g.strokes.push_back(stroke);costs.push_back(admit_profiled(g,ps.empty()?ps:ps.subspan(first,stroke.spans.size()),ctx,stop,ProfileSource::Stored,fractional4,sparse_executor,context_profile,minimum));charge(out.admission.work,costs.back().preparation_work);first+=stroke.spans.size();}
        for(const auto& c:plane.commands){
            cancelled(stop);std::uint64_t count{},work{};
            if(c.stroke>=0){if(std::size_t(c.stroke)>=costs.size())throw std::invalid_argument("Dead Ruler projected stroke");const auto& a=costs[std::size_t(c.stroke)];count=a.points;work=a.work;bytes(plane.lines.strokes[std::size_t(c.stroke)].spans.size()*sizeof(Outline));
                for(const auto& span:plane.lines.strokes[std::size_t(c.stroke)].spans)if(!plane.model_bounds.contains(span.model_index))throw std::invalid_argument("Missing Ruler model stroke gate");}
            else{
                if(c.gate!=circle_gate(c))throw std::invalid_argument("Ruler circle gate disagrees with raw parameters");
                if(c.radius<=0)continue;
                if(c.radius>65536||!point_ok({double(c.center_x),double(c.center_y)}))throw std::length_error("Ruler circle exceeds supported raw radius/coordinate limits");
                for(auto v:{double(c.center_x)-c.radius,double(c.center_x)+c.radius,double(c.center_y)-c.radius,double(c.center_y)+c.radius})if(std::abs(v)>raw_limit)throw std::length_error("Ruler circle raw extent exceeds limits");
                const auto q=[&](double v,double origin){return v*ctx.oversample/ctx.base_scale-origin*ctx.oversample;};
                if(std::max({std::abs(q(c.gate.left,ctx.origin.x)),std::abs(q(c.gate.right,ctx.origin.x)),std::abs(q(c.gate.top,ctx.origin.y)),std::abs(q(c.gate.bottom,ctx.origin.y))})>scratch_limit)throw std::length_error("Ruler circle fixed-point extent exceeds limits");
                bytes(sizeof(Outline));count=2*std::uint64_t(std::trunc(double(float(c.radius))/ctx.base_scale))+6;
                const auto area=std::uint64_t(physical.right-physical.left)*std::uint64_t(physical.bottom-physical.top);
                work=columns*rows*count+area*(ctx.oversample*ctx.oversample+1)+std::uint64_t(physical.bottom-physical.top)*ctx.oversample*count*std::bit_width(count);
            }
            if(count>max_points-points)throw std::length_error("Ruler outline point count exceeds limits");points+=count;charge(out.admission.work,work);bytes(count*(sizeof(IPoint)+128)+64); // Stored polygons plus simultaneous routing/events.
        }
    }
    if(points&&!sparse_executor){
        // Ranges include material-changing empty Lines. Conservatively charge
        // visits/resolve across the physical clip even when touched bounds let
        // the executor skip some blocks. All-empty scenes emit no scratch.
        charge(out.admission.work,columns*rows*commands);
        charge(out.admission.work,std::uint64_t(physical.right-physical.left)*std::uint64_t(physical.bottom-physical.top)*(ctx.oversample*ctx.oversample+1));
    }
    out.admission.points=points;out.planes.resize(geometries.size());out.ranges.resize(geometries.size());out.colors.reserve(geometries.size());
    for(std::size_t i=0;i<geometries.size();++i){const auto& plane=geometries[geometries.size()-1-i];auto& shapes=out.planes[i];out.colors.push_back(plane.color);
        const auto ps=profiles(plane);std::vector<std::size_t> starts;if(!ps.empty()){starts.reserve(plane.lines.strokes.size());std::size_t n=0;for(const auto& s:plane.lines.strokes){starts.push_back(n);n+=s.spans.size();}}
        for(const auto& c:plane.commands){RulerRange range{c.gate,{},shapes.size(),0};
            if(c.stroke>=0){const auto& stroke=plane.lines.strokes.at(std::size_t(c.stroke));range.color=stroke.color;
                std::size_t n=ps.empty()?0:starts.at(std::size_t(c.stroke));for(const auto& span:stroke.spans){const auto* p=ps.empty()?nullptr:&ps[n++];auto shape=outline(span,color(stroke.color,ctx.depth),ctx,out.admission.curve_step,stop,nullptr,p,minimum);shape.walker=WalkerGates{c.gate,plane.model_bounds.at(span.model_index)};shapes.push_back(std::move(shape));}}
            else if(c.radius>0)shapes.push_back(circle_outline(c,ctx,stop));
            range.end=shapes.size();out.ranges[i].push_back(range);
        }
        check_boundary_admission(shapes,ctx,out.admission,stop,context_profile);
    }
    out.storage=storage;cancelled(stop);return out;
}
}
bool can_render_vector_ruler_geometries(std::span<const VectorRulerPlaneGeometry> g,const VectorRenderContext& ctx){try{(void)prepare_rulers(g,ctx,{});return true;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}}
Raster render_vector_ruler_geometries(std::span<const VectorRulerPlaneGeometry> g,const VectorRenderContext& ctx,std::stop_token stop){
    const auto plan=prepare_rulers(g,ctx,stop);
    return render_outlines(plan.planes,ctx,plan.admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){
        auto pigment=color(plan.colors[p],ctx.depth);
        for(const auto& command:plan.ranges[p]){cancelled(stop);if(!ruler_intersects(command.gate,raw))continue;if(command.color)pigment=color(*command.color,ctx.depth);
            for(std::size_t n=command.begin;n<command.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,ctx,plan.admission,stop,pigment);}
    });
}
Pixel vector_ruler_geometries_pixel(std::span<const VectorRulerPlaneGeometry> g,const VectorRenderContext& ctx,int x,int y,std::stop_token stop){
    cancelled(stop);(void)prepare_rulers(g,ctx,stop);if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds c{x,y,x+1,y+1};if(ctx.clip)c={std::max(c.left,ctx.clip->left),std::max(c.top,ctx.clip->top),std::min(c.right,ctx.clip->right),std::min(c.bottom,ctx.clip->bottom)};
    if(c.right<=c.left||c.bottom<=c.top)return {};local.clip=c;return render_vector_ruler_geometries(g,local,stop).pixel(x,y);
}
namespace {
struct ShapeLinesView {
    std::uint64_t color{};
    const VectorLineGeometry& lines;
    const std::map<std::int32_t,VectorBounds>& model_bounds;
    std::vector<VectorRulerCommand> commands;
    std::uint64_t reserved_storage{};
    std::span<const VectorSpanProfiles> profiles;
    std::uint64_t reserved_work{};
};
struct ShapePlan {
    RulerPlan lines;
    vector_render_detail::FacePrograms faces;
    std::vector<std::vector<int>> line_ranges,face_programs;
};
ShapePlan prepare_shapes(std::span<const VectorShapePlaneGeometry> geometry,const VectorRenderContext& ctx,std::stop_token stop,bool modal4=false,std::uint64_t previous_work=0,std::uint64_t previous_storage=0){
    cancelled(stop);if(geometry.size()>100000)throw std::length_error("Shape plane limit exceeded");
    if(previous_work>128ull*1024*1024||previous_storage>512ull*1024*1024)throw std::length_error("Shape source budget seed exceeds limits");
    const auto c=admit(VectorLineGeometry{},ctx).clip;
    if(ctx.oversample!=1&&ctx.oversample!=2&&!(modal4&&ctx.oversample==4))throw std::invalid_argument("Shape normal cache requires1x/2x context");
    constexpr std::uint64_t limit=512ull*1024*1024;std::uint64_t storage{previous_storage},count{};
    const auto bytes=[&](std::uint64_t n){if(storage>limit||n>limit-storage)throw std::length_error("Shape source storage exceeds512MiB");storage+=n;};
    bytes(geometry.size()*(sizeof(ShapeLinesView)+2*sizeof(std::vector<int>)));
    std::set<const VectorShapeFaceGeometry*> unique_faces;std::set<const VectorGradationMaterial*> unique_gradations;
    std::set<const VectorTextureMaterial*> unique_textures;std::set<const VectorShapeTextureRecipe*> unique_recipes;std::array<std::size_t,6> graph{};
    for(const auto& p:geometry){cancelled(stop);
        if(p.commands.size()>100000-count||p.faces.size()>100000)throw std::length_error("Shape source collection exceeds limits");count+=p.commands.size();
        bytes(p.commands.size()*(sizeof(VectorShapeCommand)+sizeof(VectorRulerCommand)+2*sizeof(int)+sizeof(vector_render_detail::FaceOccurrence)));
        bytes(p.faces.size()*sizeof(std::shared_ptr<const VectorShapeFaceGeometry>));
        for(const auto& f:p.faces){if(!f)throw std::invalid_argument("NULL Shape projected Face");if(!unique_faces.insert(f.get()).second)continue;const auto& face=f->geometry;
            const auto bill=[&](std::size_t field,std::size_t n,std::size_t stride){if(n>100000-graph[field])throw std::length_error("Shape source Face graph exceeds limits");graph[field]+=n;bytes(2*n*stride);};
            bytes(sizeof(VectorShapeFaceGeometry)+64+(f->model_bounds.size()+f->loop_bounds.size())*sizeof(VectorBounds));bill(0,face.vertices.size(),sizeof(VectorFaceVertex));bill(1,face.models.size(),sizeof(VectorFaceModel));bill(2,face.loops.size(),sizeof(VectorFaceLoop));bill(3,face.holes.size(),sizeof(std::uint32_t));
            if((face.material_flags&15)!=1&&(face.material_flags&15)!=2&&(face.material_flags&15)!=4)throw std::invalid_argument("Shape source Face requires supported material");
            if((face.material_flags&15)!=4&&f->texture_recipe)throw std::invalid_argument("Shape texture recipe on non-texture Face");
            if((face.material_flags&15)==4){if(bool(face.texture)==bool(f->texture_recipe))throw std::invalid_argument("Shape texture needs exactly one decoded source or recipe");
                if(face.texture&&unique_textures.insert(face.texture.get()).second)bytes(2*vector_texture_storage(*face.texture)+128);
                if(f->texture_recipe&&unique_recipes.insert(f->texture_recipe.get()).second){const auto& recipe=*f->texture_recipe;bytes(2*vector_render_detail::shape_texture_storage(recipe)+recipe.leaf->size()+sizeof(recipe)+128);}}
            if((face.material_flags&15)==2){if(!face.gradation)throw std::invalid_argument("NULL Shape gradation source");if(face.gradation->stops.size()>100000)throw std::length_error("Shape source stop chain exceeds limits");
                if(unique_gradations.insert(face.gradation.get()).second)bytes(sizeof(VectorGradationMaterial)+2*face.gradation->stops.size()*sizeof(VectorGradationStop)+64);}
            for(const auto& m:face.models)bill(4,m.spans.size(),sizeof(VectorFaceSpan));for(const auto& l:face.loops)bill(5,l.members.size(),sizeof(VectorFaceMember));
        }
        for(const auto& command:p.commands){
            if(command.stroke>=0&&std::size_t(command.stroke)>=p.lines.strokes.size())throw std::invalid_argument("Dead Shape projected stroke");
            if(command.face>=0&&std::size_t(command.face)>=p.faces.size())throw std::invalid_argument("Dead Shape projected Face");}
    }
    ShapePlan out;out.line_ranges.resize(geometry.size());out.face_programs.resize(geometry.size());
    std::vector<ShapeLinesView> views;views.reserve(geometry.size());std::vector<vector_render_detail::FaceOccurrence> occurrences;occurrences.reserve(std::size_t(count));
    for(std::size_t i=0;i<geometry.size();++i){const auto& p=geometry[i];views.push_back({0,p.lines,p.model_bounds,{},p.reserved_storage,p.line_profiles});auto& v=views.back();v.commands.reserve(p.commands.size());
        const auto emit=[&](const VectorShapeCommand& command){const auto dest=geometry.size()-1-i;
            int line=-1,face=-1;if(command.stroke>=0){line=int(v.commands.size());v.commands.push_back({command.stroke,0,0,0,command.gate});}
            if(command.face>=0){face=int(occurrences.size());const auto& f=*p.faces[std::size_t(command.face)];occurrences.push_back({&f.geometry,command.gate,f.region_bounds,f.model_bounds,f.loop_bounds,f.texture_recipe});}
            out.line_ranges[dest].push_back(line);out.face_programs[dest].push_back(face);
        };
        if(ctx.oversample==1&&!modal4)for(auto it=p.commands.rbegin();it!=p.commands.rend();++it)emit(*it);else for(const auto& command:p.commands)emit(command);
    }
    // Source Face graphs and simultaneous command adapters reserve space before
    // Line polygons/routing allocation. The original immutable scenes stay live.
    out.lines=prepare_rulers(std::span<const ShapeLinesView>(views),ctx,stop,previous_work,false,false,ContextProfile::Color,0,modal4,storage);
    const auto columns=c.right>c.left?std::uint64_t((c.right-1)/256-c.left/256+1):0,rows=c.bottom>c.top?std::uint64_t((c.bottom-1)/256-c.top/256+1):0;
    charge(out.lines.admission.work,columns*rows*count);
    const auto width=c.right>c.left?std::uint64_t(std::min(ctx.width,(c.right+255)/256*256)-c.left/256*256):0;
    const auto height=c.bottom>c.top?std::uint64_t(std::min(ctx.height,(c.bottom+255)/256*256)-c.top/256*256):0;
    // Executor visits complete physical blocks for Face-only scenes as well.
    charge(out.lines.admission.work,width*height*(ctx.oversample==4?273:ctx.oversample*ctx.oversample+1));
    out.faces=vector_render_detail::prepare_shape_face_programs(occurrences,ctx,{out.lines.storage,out.lines.admission.work,out.lines.admission.points,out.lines.admission.points,true},stop);
    return out;
}
Raster render_shapes(const ShapePlan& plan,const VectorRenderContext& ctx,std::stop_token stop,bool overwrite1x=false,bool byte_output=false,bool face_chain=false,std::set<TileCoord>* resolved=nullptr,std::set<TileCoord>* touched_blocks=nullptr,bool fractional4=false){
    cancelled(stop);Raster result(ctx.width,ctx.height,ctx.depth,ctx.resolution);const auto c=plan.lines.admission.clip;const int mode=int(ctx.oversample);
    for(int ty=c.top/256;ty*256<c.bottom;++ty)for(int tx=c.left/256;tx*256<c.right;++tx){
        cancelled(stop);const VectorBounds block{tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)};const auto raw=raw_block(block,ctx);
        const int w=block.right-block.left,h=block.bottom-block.top;
        Scratch scratch{{block.left*mode,block.top*mode,block.right*mode,block.bottom*mode},w*mode,h*mode,ctx.depth,ctx.oversample,{},{},{},fractional4};
        scratch.overwrite1x=overwrite1x;scratch.byte_output=byte_output;scratch.pixels.resize(std::size_t(w*mode)*h*mode,Pixel{0,0,0,0});std::vector<Pixel> companion;
        if(mode>1){companion.resize(scratch.pixels.size(),Pixel{0,0,0,0});scratch.marked.resize(scratch.pixels.size());}
        const auto flush=[&](bool whole){if(mode==1)return;
            if(whole){for(std::size_t n=0;n<scratch.pixels.size();++n){if(!(n%512))cancelled(stop);companion[n]=scratch.byte_output?compose_byte(companion[n],scratch.pixels[n]):compose(companion[n],scratch.pixels[n],ctx.depth);scratch.pixels[n]={0,0,0,0};scratch.marked[n]=0;}}
            else for(auto n:scratch.dirty){companion[n]=scratch.byte_output?compose_byte(companion[n],scratch.pixels[n]):compose(companion[n],scratch.pixels[n],ctx.depth);scratch.pixels[n]={0,0,0,0};scratch.marked[n]=0;}
            scratch.dirty.clear();
        };
        for(std::size_t p=0;p<plan.line_ranges.size();++p)for(std::size_t command=0;command<plan.line_ranges[p].size();++command){cancelled(stop);
            const auto line=plan.line_ranges[p][command],face=plan.face_programs[p][command];
            if(line>=0){const auto& range=plan.lines.ranges[p][std::size_t(line)];if(!ruler_intersects(range.gate,raw))continue;
                for(std::size_t n=range.begin;n<range.end;++n)draw_outline(plan.lines.planes[p][n],scratch,raw,block,ctx,plan.lines.admission,stop);
                flush(false);
            }
            if(face>=0&&vector_render_detail::paint_shape_face_program(plan.faces,std::size_t(face),block,scratch.pixels,ctx,stop,overwrite1x)){scratch.primitive_drawn=true;scratch.pixel_written=true;if(!face_chain)flush(true);}
        }
        if(resolved&&scratch.primitive_drawn)resolved->insert({tx,ty});if(touched_blocks&&scratch.pixel_written)touched_blocks->insert({tx,ty});
        if(face_chain)flush(true);
        if(mode>1)scratch.pixels=std::move(companion);
        for(int y=std::max(block.top,c.top);y<std::min(block.bottom,c.bottom);++y){cancelled(stop);for(int x=std::max(block.left,c.left);x<std::min(block.right,c.right);++x){const auto pixel=resolve(scratch,x-block.left,y-block.top);if(pixel.a)result.set_pixel(x,y,pixel);}}
    }
    cancelled(stop);return result;
}
}
bool can_render_vector_shape_geometries(std::span<const VectorShapePlaneGeometry> g,const VectorRenderContext& ctx){try{(void)prepare_shapes(g,ctx,{});return true;}catch(const FormatError&){return false;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}}
Raster render_vector_shape_geometries(std::span<const VectorShapePlaneGeometry> g,const VectorRenderContext& ctx,std::stop_token stop){return render_shapes(prepare_shapes(g,ctx,stop),ctx,stop);}
Pixel vector_shape_geometries_pixel(std::span<const VectorShapePlaneGeometry> g,const VectorRenderContext& ctx,int x,int y,std::stop_token stop){
    cancelled(stop);(void)prepare_shapes(g,ctx,stop);if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds clip{x,y,x+1,y+1};if(ctx.clip)clip={std::max(clip.left,ctx.clip->left),std::max(clip.top,ctx.clip->top),std::min(clip.right,ctx.clip->right),std::min(clip.bottom,ctx.clip->bottom)};
    if(clip.right<=clip.left||clip.bottom<=clip.top)return {};local.clip=clip;return render_vector_shape_geometries(g,local,stop).pixel(x,y);
}

namespace vector_render_detail {
namespace {
const StoredLinePlane& stored_source(const StoredLinePlane& p){return p;}
const StoredLinePlane& stored_source(const StoredPaintPlane& p){return p.lines;}
template<class Plane> RulerPlan prepare_stored_lines(std::span<const Plane> geometry,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t extra_storage=0,std::uint64_t previous_work=0,bool fractional4=false,bool sparse_executor=false,ContextProfile context_profile=ContextProfile::Color,float minimum=0,bool modal4=false){
    cancelled(stop);if(geometry.size()>100000)throw std::length_error("Stored Line plane limit exceeded");
    if(ctx.oversample!=1&&ctx.oversample!=2&&!(ctx.oversample==4&&(fractional4||modal4)))throw std::invalid_argument("Stored Line normal cache requires1x/2x");
    constexpr std::uint64_t limit=512ull*1024*1024;
    if(extra_storage>limit)throw std::length_error("Stored Line extra storage exceeds limits");
    std::uint64_t storage=extra_storage+geometry.size()*sizeof(ShapeLinesView),commands{};
    if(storage>limit)throw std::length_error("Stored Line view storage exceeds limits");
    for(const auto& source:geometry){const auto& p=stored_source(source);if(p.line_bounds.size()!=p.lines.strokes.size())throw std::invalid_argument("Stored LC gate count does not match strokes");
        if(p.lines.strokes.size()>100000-commands)throw std::length_error("Stored Line command limit exceeded");commands+=p.lines.strokes.size();
        const auto bytes=p.line_bounds.size()*sizeof(VectorBounds)+p.lines.strokes.size()*sizeof(VectorRulerCommand);
        if(bytes>limit-storage)throw std::length_error("Stored Line adapter storage exceeds limits");storage+=bytes;}
    std::vector<ShapeLinesView> views;views.reserve(geometry.size());
    for(const auto& source:geometry){const auto& p=stored_source(source);views.push_back({0,p.lines,p.model_bounds,{},p.reserved_storage,p.line_profiles,p.reserved_work});auto& v=views.back();v.commands.reserve(p.lines.strokes.size());
        const auto emit=[&](std::size_t n){v.commands.push_back({int(n),0,0,0,p.line_bounds[n]});};
        if(ctx.oversample==1&&!modal4&&context_profile!=ContextProfile::BitMask&&context_profile!=ContextProfile::BitSelect1x)for(auto n=p.lines.strokes.size();n-->0;)emit(n);else for(std::size_t n=0;n<p.lines.strokes.size();++n)emit(n);}
    if(!views.empty()){if(views.front().reserved_storage>limit||storage>limit-views.front().reserved_storage)throw std::length_error("Stored Line joint adapter storage exceeds limits");views.front().reserved_storage+=storage;}
    if(ctx.minimum_line_width&&minimum==0)minimum=float(numeric_detail::binary64(ctx.base_scale*double(std::bit_cast<float>(std::uint32_t{0x3f19999a}))));
    return prepare_rulers(std::span<const ShapeLinesView>(views),ctx,stop,previous_work,fractional4,sparse_executor,context_profile,minimum,modal4);
}
}
bool can_render_stored_line_planes(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx){try{(void)prepare_stored_lines(g,ctx,{});return true;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}}
StoredLineCacheCosts stored_line_planes_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,
                                             StoredLineCacheCosts previous,std::stop_token stop){
    auto plan=prepare_stored_lines(g,ctx,stop,previous.storage,previous.work);
    //The adapter carries extra storage on its first view. Empty batches
    //have no view, but must still retain the caller's preceding resources.
    if(g.empty()){
        constexpr std::uint64_t limit=512ull*1024*1024;
        if(plan.storage>limit-previous.storage)throw std::length_error("Stored Line cumulative storage exceeds limits");
        plan.storage+=previous.storage;
    }
    return {plan.storage,plan.admission.work};
}
Raster render_stored_line_planes(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,std::stop_token stop){
    const auto plan=prepare_stored_lines(g,ctx,stop);
    if(!plan.admission.points)return Raster(ctx.width,ctx.height,ctx.depth,ctx.resolution);
    // 6453C0 flushes after all LC, not after each LC. Ordinary LC gates
    // are raw four comparisons (59C2F0), unlike Ruler's IntersectRect gate.
    return render_outlines(plan.planes,ctx,plan.admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){
        for(const auto& c:plan.ranges[p]){cancelled(stop);if(!intersects(c.gate,raw))continue;
            for(std::size_t n=c.begin;n<c.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,ctx,plan.admission,stop);}
    });
}
Pixel stored_line_planes_pixel(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,int x,int y,std::stop_token stop){
    cancelled(stop);(void)prepare_stored_lines(g,ctx,stop);if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds c{x,y,x+1,y+1};if(ctx.clip)c={std::max(c.left,ctx.clip->left),std::max(c.top,ctx.clip->top),std::min(c.right,ctx.clip->right),std::min(c.bottom,ctx.clip->bottom)};
    if(c.right<=c.left||c.bottom<=c.top)return {};local.clip=c;return render_stored_line_planes(g,local,stop).pixel(x,y);
}
namespace {
struct PaintPlan {bool fractional4{},retain_a0{};
    RulerPlan lines;
    FacePrograms faces;
    std::vector<std::vector<std::size_t>> face_programs;
    std::vector<bool> visible;
    std::vector<VectorBounds> blocks;
    StoredLineCacheCosts costs;
};
bool paint_line_touches(const RulerPlan& lines,std::size_t plane,VectorBounds block,const VectorRenderContext& ctx){
    const auto raw=raw_block(block,ctx);const int m=int(ctx.oversample);
    const VectorBounds samples{block.left*m,block.top*m,block.right*m,block.bottom*m};
    for(const auto& range:lines.ranges[plane])if(intersects(range.gate,raw))
        for(auto n=range.begin;n<range.end;++n){const auto& shape=lines.planes[plane][n];
            if(intersects(shape.walker->model,raw)&&(fills_block(shape,block)||intersects(shape.bounds,samples)))return true;}
    return false;
}
PaintPlan prepare_paint(std::span<const StoredPaintPlane> geometry,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t previous_work,std::uint64_t previous_storage=0,ContextProfile profile=ContextProfile::Color){
    const bool direct=profile==ContextProfile::DirectOutput;
    const bool fractional4=direct&&ctx.oversample==4;
    cancelled(stop);if((ctx.oversample!=1&&ctx.oversample!=2&&!fractional4)||(ctx.oversample==1&&ctx.depth!=32&&!direct))throw std::invalid_argument("Stored Paint requires normal1/AA2 or direct weighted4 context");
    (void)admit(VectorLineGeometry{},ctx,fractional4,false,false,profile);
    if(geometry.size()>100000)throw std::length_error("Stored Paint plane limit exceeded");
    constexpr std::uint64_t limit=512ull*1024*1024;
    if(previous_storage>limit)throw std::length_error("Paint prior source storage exceeds512MiB");
    std::uint64_t storage{previous_storage},commands{};
    const auto bytes=[&](std::uint64_t n){if(n>limit-storage)throw std::length_error("Paint joint source storage exceeds512MiB");storage+=n;};
    bytes(geometry.size()*(sizeof(StoredPaintPlane)+sizeof(std::vector<std::size_t>)+1));
    // The physical mask descriptors retained by the native owner are budgeted
    // with the simultaneous Line and Face programs, even for hidden planes.
    bytes(((std::uint64_t(ctx.width)+31)/32)*std::uint64_t(ctx.height)*4+
          ((std::uint64_t(ctx.width)+255)/256)*((std::uint64_t(ctx.height)+255)/256));
    std::set<const VectorFaceGeometry*> sources;
    PaintPlan out;out.fractional4=fractional4;out.retain_a0=direct&&ctx.depth==64&&ctx.oversample==1;out.face_programs.resize(geometry.size());out.visible.resize(geometry.size());
    std::vector<FaceOccurrence> faces;
    for(std::size_t i=0;i<geometry.size();++i){cancelled(stop);const auto& p=geometry[i];
        if(!p.lines.lines.strokes.empty()&&!p.faces.empty())throw std::invalid_argument("Paint plane cannot own both Lines and Faces");
        if(p.faces.size()>100000-commands||p.lines.lines.strokes.size()>100000-commands-p.faces.size())throw std::length_error("Paint command occurrence limit exceeded");
        commands+=p.faces.size()+p.lines.lines.strokes.size();bytes(p.faces.size()*(sizeof(FaceOccurrence)+sizeof(std::size_t)+sizeof(const VectorFaceGeometry*)));
        const auto dest=geometry.size()-1-i;out.visible[dest]=p.visible;
        if(!p.texture_recipes.empty()&&p.texture_recipes.size()!=p.faces.size())throw std::invalid_argument("Paint texture recipe count disagrees with Faces");
        std::size_t face_index{};for(const auto* g:p.faces){if(!g)throw std::invalid_argument("NULL Paint Face source");
            if(sources.insert(g).second){bytes(2*sizeof(VectorFaceGeometry));
                const auto bill=[&](std::size_t n,std::size_t stride){if(n>100000)throw std::length_error("Paint source Face graph exceeds limits");bytes(2*n*stride);};
                bill(g->vertices.size(),sizeof(VectorFaceVertex));bill(g->models.size(),sizeof(VectorFaceModel));bill(g->loops.size(),sizeof(VectorFaceLoop));bill(g->holes.size(),sizeof(std::uint32_t));
                for(const auto& m:g->models)bill(m.spans.size(),sizeof(VectorFaceSpan));for(const auto& l:g->loops)bill(l.members.size(),sizeof(VectorFaceMember));}
            out.face_programs[dest].push_back(faces.size());faces.push_back({g,{},{},{},{},p.texture_recipes.empty()?nullptr:p.texture_recipes[face_index]});++face_index;
        }
    }
    // Skip the generic Ruler's whole-frame executor estimate. Paint visits the
    // union of real Line/Face physical blocks, preserving sparse large owners.
    out.lines=prepare_stored_lines(geometry,ctx,stop,storage,previous_work,fractional4,true,profile);
    out.faces=(profile==ContextProfile::ReferenceOutput?prepare_paint_reference_face_programs:profile==ContextProfile::DirectOutput?prepare_paint_direct_face_programs:prepare_paint_face_programs)(faces,ctx,{out.lines.storage,out.lines.admission.work,out.lines.admission.points,out.lines.admission.points,false},stop);
    auto budget=out.faces.costs;
    const auto reserve=[&](std::uint64_t n){if(n>limit-budget.storage)throw std::length_error("Paint block storage exceeds512MiB");budget.storage+=n;};
    const auto c=out.lines.admission.clip;
    const auto less=[](VectorBounds a,VectorBounds b){return std::tie(a.top,a.left,a.bottom,a.right)<std::tie(b.top,b.left,b.bottom,b.right);};
    std::set<VectorBounds,decltype(less)> blocks(less);
    const auto add=[&](VectorBounds b){if(blocks.insert(b).second)reserve(sizeof(VectorBounds)*2+64);};
    for(const auto& b:face_program_blocks(out.faces)){cancelled(stop);add(b);}
    if(c.left<c.right&&c.top<c.bottom)for(const auto& plane:out.lines.planes)for(const auto& shape:plane){cancelled(stop);
        for(auto b:shape.full_blocks)if(intersects(b,c))add(b);
        const auto floor_sample=[m=ctx.oversample](std::int64_t n){return n>=0?n/m:-((-n+m-1)/m);};
        const int left=int(std::max<std::int64_t>(c.left,floor_sample(shape.bounds.left))),top=int(std::max<std::int64_t>(c.top,floor_sample(shape.bounds.top)));
        const int right=int(std::min<std::int64_t>(c.right,floor_sample(std::int64_t(shape.bounds.right)-1)+1)),bottom=int(std::min<std::int64_t>(c.bottom,floor_sample(std::int64_t(shape.bounds.bottom)-1)+1));
        if(left>=right||top>=bottom)continue;
        for(int ty=top/256;ty*256<bottom;++ty)for(int tx=left/256;tx*256<right;++tx)add({tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)});
    }
    charge(budget.work,blocks.size()*(geometry.size()+commands+out.lines.admission.points));
    for(auto block:blocks){cancelled(stop);const auto area=std::uint64_t(block.right-block.left)*(block.bottom-block.top);
        charge(budget.work,area*(ctx.oversample*ctx.oversample+1)); // Full owner scratch/resolve, including pixel queries.
        for(std::size_t p=0;p<geometry.size();++p){bool touched=paint_line_touches(out.lines,p,block,ctx);
            for(auto face:out.face_programs[p])touched=touched||face_program_has_block(out.faces,face,block);
            if(touched)charge(budget.work,area*ctx.oversample*ctx.oversample);}
    }
    //The empty Line adapter has no first view to carry joint reservations.
    //Record that storage once for cumulative-cost callers. Existing render
    //and boolean-admission callers do not consume this new accounting field.
    out.costs={budget.storage+(geometry.empty()?storage:0),budget.work};
    out.blocks.assign(blocks.begin(),blocks.end());complete_face_programs(out.faces,stop);cancelled(stop);return out;
}
Raster render_paint(const PaintPlan& plan,const VectorRenderContext& ctx,std::stop_token stop){
    cancelled(stop);Raster result(ctx.width,ctx.height,ctx.depth,ctx.resolution);const auto c=plan.lines.admission.clip;
    for(auto block:plan.blocks){cancelled(stop);const auto raw=raw_block(block,ctx);const int w=block.right-block.left,h=block.bottom-block.top,m=int(ctx.oversample);
        Scratch scratch{{block.left*m,block.top*m,block.right*m,block.bottom*m},w*m,h*m,ctx.depth,ctx.oversample,{},{},{},plan.fractional4};
        scratch.pixels.resize(std::size_t(w)*h*m*m,plan.retain_a0?Pixel{}:Pixel{0,0,0,0});std::vector<Pixel> companion;
        if(m>1){scratch.marked.resize(scratch.pixels.size());companion.resize(scratch.pixels.size(),Pixel{0,0,0,0});}
        for(std::size_t p=0;p<plan.visible.size();++p){cancelled(stop);if(!plan.visible[p])continue;bool painted{};
            for(const auto& range:plan.lines.ranges[p]){if(!intersects(range.gate,raw))continue;
                for(auto n=range.begin;n<range.end;++n)draw_outline(plan.lines.planes[p][n],scratch,raw,block,ctx,plan.lines.admission,stop);}
            painted=!scratch.dirty.empty();
            for(auto face:plan.face_programs[p])painted=paint_shape_face_program(plan.faces,face,block,scratch.pixels,ctx,stop,false,plan.fractional4)||painted;
            if(m>1&&painted)for(std::size_t n=0;n<scratch.pixels.size();++n){if(!(n%512))cancelled(stop);
                companion[n]=scratch.byte_output?compose_byte(companion[n],scratch.pixels[n]):compose(companion[n],scratch.pixels[n],ctx.depth);scratch.pixels[n]={0,0,0,0};scratch.marked[n]=0;}
            scratch.dirty.clear();
        }
        if(m>1)scratch.pixels=std::move(companion);
        for(int y=std::max(block.top,c.top);y<std::min(block.bottom,c.bottom);++y){cancelled(stop);
            for(int x=std::max(block.left,c.left);x<std::min(block.right,c.right);++x){const auto pixel=resolve(scratch,x-block.left,y-block.top);if(pixel.a||(plan.retain_a0&&pixel!=Pixel{}))result.set_pixel(x,y,pixel);}}
    }
    cancelled(stop);return result;
}
}
bool can_render_stored_paint_planes(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,std::uint64_t work){try{(void)prepare_paint(g,ctx,{},work);return true;}catch(const FormatError&){return false;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}}
StoredLineCacheCosts stored_paint_planes_costs(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,
                                              StoredLineCacheCosts previous,std::stop_token stop){
    const auto plan=prepare_paint(g,ctx,stop,previous.work,previous.storage);
    if(plan.costs.storage>512ull*1024*1024)throw std::length_error("Paint cumulative storage exceeds512MiB");
    return plan.costs;
}
Raster render_stored_paint_planes(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t work){return render_paint(prepare_paint(g,ctx,stop,work),ctx,stop);}
StoredLineCacheCosts stored_paint_reference_costs(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    const auto plan=prepare_paint(g,ctx,stop,previous.work,previous.storage,ContextProfile::ReferenceOutput);
    if(plan.costs.storage>512ull*1024*1024)throw std::length_error("Paint reference cumulative storage exceeds512MiB");return plan.costs;
}
Raster render_stored_paint_reference(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t work){return render_paint(prepare_paint(g,ctx,stop,work,0,ContextProfile::ReferenceOutput),ctx,stop);}
StoredLineCacheCosts stored_paint_direct_costs(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    const auto plan=prepare_paint(g,ctx,stop,previous.work,previous.storage,ContextProfile::DirectOutput);
    if(plan.costs.storage>512ull*1024*1024)throw std::length_error("Paint direct cumulative storage exceeds512MiB");return plan.costs;
}
Raster render_stored_paint_direct(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,std::stop_token stop,std::uint64_t work){return render_paint(prepare_paint(g,ctx,stop,work,0,ContextProfile::DirectOutput),ctx,stop);}
Pixel stored_paint_planes_pixel(std::span<const StoredPaintPlane> g,const VectorRenderContext& ctx,int x,int y,std::uint64_t work){
    (void)prepare_paint(g,ctx,{},work);if(x<0||y<0||x>=ctx.width||y>=ctx.height)return {};
    auto local=ctx;VectorBounds c{x,y,x+1,y+1};if(ctx.clip)c={std::max(c.left,ctx.clip->left),std::max(c.top,ctx.clip->top),std::min(c.right,ctx.clip->right),std::min(c.bottom,ctx.clip->bottom)};
    if(c.left>=c.right||c.top>=c.bottom)return {};local.clip=c;return render_stored_paint_planes(g,local,{},work).pixel(x,y);
}
namespace {
struct ByteScratch {
    VectorBounds bounds;int width{},height{};unsigned mode{};Bytes pixels;
    void write(int x,int y,Pixel,bool partial=false,double coverage=1){
        if(x<bounds.left||x>=bounds.right||y<bounds.top||y>=bounds.bottom)return;
        auto& d=pixels[std::size_t(y-bounds.top)*width+std::size_t(x-bounds.left)];
        // The ordinary5A7080 flush descriptor+10 is always zero, including
        // linear and endpoint-alias bodies. 5DD990 inverts it for partials.
        d=partial?byte_sink::partial(d,255,coverage,true):byte_sink::full(d,255,mode==1);
    }
    void span(int y,std::int64_t left,std::int64_t right,Pixel pigment){
        if(mode==2){
            const auto x0=std::max(double(bounds.left),double(left)/256);
            const auto x1=std::min(double(bounds.right),double(right)/256+1.0);
            byte_sink::fractional_span(x0,x1,[&](int x,bool partial,double coverage){write(x,y,pigment,partial,coverage);});return;
        }
        const auto floor_fixed=[](std::int64_t n){return n>=0?n/256:-((-n+255)/256);};
        const auto begin=int(std::max<std::int64_t>(bounds.left,floor_fixed(left)));
        const auto end=int(std::min<std::int64_t>(bounds.right,floor_fixed(right)+1));
        for(int x=begin;x<end;++x)write(x,y,pigment);
    }
};
RulerPlan prepare_line_cache(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,std::stop_token stop,StoredLineCacheCosts previous={},bool print_word=false){
    if(ctx.depth!=32&&(!print_word||ctx.depth!=64))throw std::invalid_argument("Unsupported formal Line cache target");
    const auto clip=admit(VectorLineGeometry{},ctx).clip;
    const auto columns=clip.right>clip.left?std::uint64_t((clip.right-1)/256-clip.left/256+1):0;
    const auto rows=clip.bottom>clip.top?std::uint64_t((clip.bottom-1)/256-clip.top/256+1):0;
    const auto cells=columns*rows*65536;
    // One real Raster8 plane is alive alongside the actual-depth cache and two
    // byte scratch surfaces; published planes are consumed one at a time.
    if(previous.storage>512ull*1024*1024)throw std::length_error("Line scene storage exceeds limits");
    auto plan=prepare_stored_lines(g,ctx,stop,cells+2*512ull*512+previous.storage,previous.work);
    // With no view there is no first reservation to carry the caller's seed.
    if(g.empty()){
        const auto extra=cells+2*512ull*512+previous.storage;
        if(extra>512ull*1024*1024-plan.storage)throw std::length_error("Empty Line cache storage exceeds limits");
        plan.storage+=extra;
    }
    if(plan.admission.points){
        charge(plan.admission.work,cells*(ctx.oversample*ctx.oversample+2)*g.size());
        charge(plan.admission.work,2*cells);
    }
    return plan;
}
void write_byte(Raster& raster,int x,int y,std::uint8_t value){
    if(!value)return;
    auto& tile=raster.tiles[{x/256,y/256}];
    if(!tile)tile=std::make_shared<Bytes>(65536,0);
    else if(tile.use_count()!=1)tile=std::make_shared<Bytes>(*tile);
    (*tile)[std::size_t(y%256)*256+x%256]=value;
}
Raster render_mask(const RulerPlan& plan,std::size_t plane,const VectorRenderContext& ctx,std::stop_token stop){
    Raster result(ctx.width,ctx.height,8,ctx.resolution);const auto clip=plan.admission.clip;
    if(!plan.admission.points)return result;
    const auto m=int(ctx.oversample);
    for(int ty=clip.top/256;ty*256<clip.bottom;++ty)for(int tx=clip.left/256;tx*256<clip.right;++tx){
        cancelled(stop);const VectorBounds block{tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)};
        const auto raw=raw_block(block,ctx);const auto width=block.right-block.left,height=block.bottom-block.top;
        ByteScratch scratch{{block.left*m,block.top*m,block.right*m,block.bottom*m},width*m,height*m,ctx.oversample,Bytes(std::size_t(width*m)*height*m,0)};
        for(const auto& command:plan.ranges[plane]){cancelled(stop);if(!intersects(command.gate,raw))continue;
            for(auto n=command.begin;n<command.end;++n)draw_outline(plan.planes[plane][n],scratch,raw,block,ctx,plan.admission,stop);}
        Bytes companion;
        if(m==2){companion.resize(scratch.pixels.size());for(std::size_t i=0;i<companion.size();++i){if(!(i%512))cancelled(stop);companion[i]=byte_sink::full(0,scratch.pixels[i],true);scratch.pixels[i]=0;}}
        const auto& samples=m==1?scratch.pixels:companion;
        for(int y=std::max(block.top,clip.top);y<std::min(block.bottom,clip.bottom);++y){
            if(!(y%32))cancelled(stop);
            for(int x=std::max(block.left,clip.left);x<std::min(block.right,clip.right);++x){unsigned sum{};
                for(int j=0;j<m;++j)for(int i=0;i<m;++i)sum+=samples[std::size_t((y-block.top)*m+j)*scratch.width+(x-block.left)*m+i];
                write_byte(result,x,y,std::uint8_t(m==1?sum:sum>>2));}
        }
    }
    return result;
}
}
bool can_render_stored_line_cache_planes(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx){try{(void)prepare_line_cache(g,ctx,{});return true;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}}
StoredLineCacheCosts stored_line_cache_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    auto plan=prepare_line_cache(g,ctx,stop,previous);
    // Scene admission itself prepares geometry before the actual render
    // prepares it again. Charge a second full allowance conservatively.
    charge(plan.admission.work,plan.admission.work-previous.work);
    return {plan.storage,plan.admission.work};
}
Raster render_stored_line_mask(const StoredLinePlane& g,const VectorRenderContext& ctx,std::stop_token stop){
    const auto plan=prepare_line_cache({&g,1},ctx,stop);return render_mask(plan,0,ctx,stop);
}
Raster render_stored_line_cache_planes(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,std::stop_token stop){
    const auto plan=prepare_line_cache(g,ctx,stop);Raster result(ctx.width,ctx.height,32,ctx.resolution);
    for(std::size_t plane=0;plane<g.size();++plane){cancelled(stop);const auto mask=render_mask(plan,plane,ctx,stop);
        const auto color=g[g.size()-1-plane].color;
        for(const auto& [tile,bytes]:mask.tiles){for(int y=tile.y*256;y<std::min(ctx.height,(tile.y+1)*256);++y){if(!(y%32))cancelled(stop);
            for(int x=tile.x*256;x<std::min(ctx.width,(tile.x+1)*256);++x){const auto value=(*bytes)[std::size_t(y%256)*256+x%256];if(value)result.set_pixel(x,y,byte_sink::behind_white(result.pixel(x,y),value,color));}}}
    }
    for(const auto& [tile,bytes]:result.tiles){for(int y=tile.y*256;y<std::min(ctx.height,(tile.y+1)*256);++y){if(!(y%32))cancelled(stop);
        for(int x=tile.x*256;x<std::min(ctx.width,(tile.x+1)*256);++x){const auto p=result.pixel(x,y);if(p.a)result.set_pixel(x,y,byte_sink::straight(p));}}}
    cancelled(stop);return result;
}
StoredLineCacheCosts stored_line_print_cache_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    auto plan=prepare_line_cache(g,ctx,stop,previous,true);
    charge(plan.admission.work,plan.admission.work-previous.work);
    return {plan.storage,plan.admission.work};
}
Raster render_stored_line_print_cache(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,std::stop_token stop){
    if(ctx.depth==32)return render_stored_line_cache_planes(g,ctx,stop);
    const auto plan=prepare_line_cache(g,ctx,stop,{},true);
    //644F80 transfers each real Raster8 helper through63C4A0. Its target64
    //switch accepts only source32/64, so source8 takes the native default skip.
    //Still render masks: their graph, work and cancellation remain observable.
    for(std::size_t plane=0;plane<g.size();++plane){cancelled(stop);(void)render_mask(plan,plane,ctx,stop);}
    //On fresh SDK64 white/A0,650B10's packed DWORD alpha bytes are255/0;
    //both skip. The caller materializes all fresh blocks, including padding.
    cancelled(stop);return Raster(ctx.width,ctx.height,ctx.depth,ctx.resolution);
}
StoredLineCacheCosts ruler_output_costs(std::span<const VectorRulerPlaneGeometry> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    cancelled(stop);if(previous.storage>512ull*1024*1024||previous.work>128ull*1024*1024)throw std::length_error("Ruler source budget seed exceeds limits");
    const auto plan=prepare_rulers(g,ctx,stop,previous.work,false,false,ContextProfile::Color,0,false,previous.storage);
    return {plan.storage,plan.admission.work};
}
StoredLineCacheCosts shape_output_costs(std::span<const VectorShapePlaneGeometry> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,std::stop_token stop){
    const auto plan=prepare_shapes(g,ctx,stop,false,previous.work,previous.storage);
    return {plan.faces.costs.storage,plan.faces.costs.work};
}
ClipboardVectorRender render_clipboard_lines(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,bool byte_output,bool fractional4,std::stop_token stop){
    std::set<TileCoord> resolved,touched;previous.storage+=std::uint64_t((ctx.width+255)/256)*((ctx.height+255)/256)*256;
    const auto plan=prepare_stored_lines(g,ctx,stop,previous.storage,previous.work,fractional4,false,ContextProfile::Color,0,true);
    auto pixels=render_outlines(plan.planes,ctx,plan.admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){for(const auto& command:plan.ranges[p])if(intersects(command.gate,raw))for(auto n=command.begin;n<command.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,ctx,plan.admission,stop);},fractional4,ctx.oversample==1,byte_output,&resolved,&touched);return {std::move(pixels),{plan.storage,plan.admission.work},std::move(resolved),std::move(touched)};
}
ClipboardVectorRender render_clipboard_rulers(std::span<const VectorRulerPlaneGeometry> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,bool byte_output,bool fractional4,std::stop_token stop){
    std::set<TileCoord> resolved,touched;previous.storage+=std::uint64_t((ctx.width+255)/256)*((ctx.height+255)/256)*256;auto views=std::vector<VectorRulerPlaneGeometry>(g.begin(),g.end());if(!views.empty())views.front().reserved_storage+=previous.storage;
    const auto plan=prepare_rulers(std::span<const VectorRulerPlaneGeometry>(views),ctx,stop,previous.work,fractional4,false,ContextProfile::Color,0,true);
    auto pixels=render_outlines(plan.planes,ctx,plan.admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){auto pigment=color(plan.colors[p],ctx.depth);for(const auto& command:plan.ranges[p]){if(!ruler_intersects(command.gate,raw))continue;if(command.color)pigment=color(*command.color,ctx.depth);else if(ctx.oversample==1)pigment=color(0xffff000000000000ull,ctx.depth);for(auto n=command.begin;n<command.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,ctx,plan.admission,stop,pigment);}},fractional4,ctx.oversample==1,byte_output,&resolved,&touched);return {std::move(pixels),{plan.storage,plan.admission.work},std::move(resolved),std::move(touched)};
}
ClipboardVectorRender render_clipboard_shapes(std::span<const VectorShapePlaneGeometry> g,const VectorRenderContext& ctx,StoredLineCacheCosts previous,bool byte_output,bool face_chain,bool fractional4,std::stop_token stop){std::set<TileCoord> resolved,touched;previous.storage+=std::uint64_t((ctx.width+255)/256)*((ctx.height+255)/256)*256;auto views=std::vector<VectorShapePlaneGeometry>(g.begin(),g.end());if(!views.empty())views.front().reserved_storage+=previous.storage;auto plan=prepare_shapes(std::span<const VectorShapePlaneGeometry>(views),ctx,stop,true);charge(plan.faces.costs.work,previous.work);return {render_shapes(plan,ctx,stop,ctx.oversample==1,byte_output,face_chain,&resolved,&touched,fractional4),{plan.faces.costs.storage,plan.faces.costs.work},std::move(resolved),std::move(touched)};}
namespace {
struct LineOutputPlan {VectorRenderContext context;RulerPlan plan;bool fractional4{};};
LineOutputPlan prepare_line_output(std::span<const StoredLinePlane> g,VectorRenderContext ctx,StoredLineOutputSampling sampling,std::stop_token stop,StoredLineCacheCosts previous={},ContextProfile profile=ContextProfile::Color){
    cancelled(stop);bool fractional4{};std::uint64_t extra{};
    switch(sampling){
    case StoredLineOutputSampling::Plain1x:ctx.oversample=1;break;
    case StoredLineOutputSampling::AA2:ctx.oversample=2;break;
    case StoredLineOutputSampling::Fractional4x:
        ctx.oversample=4;fractional4=true;
        // Actual4x RGBA scratch + companion + mark bytes + geometric-growth
        // dirty-index capacity, minus the existing normal scratch reservation.
        extra=65536ull*16*(2*sizeof(Pixel)+1+2*sizeof(std::size_t))-2*512ull*512*sizeof(Pixel);break;
    default:throw std::invalid_argument("Unsupported stored Line output sampling");
    }
    if(previous.storage>512ull*1024*1024)throw std::length_error("Native Line output storage exceeds limits");
    auto plan=prepare_stored_lines(g,ctx,stop,extra+previous.storage,previous.work,fractional4,false,profile);
    return {ctx,std::move(plan),fractional4};
}
}
bool can_render_stored_line_output(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling){
    try{(void)prepare_line_output(g,ctx,sampling,{});return true;}catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}
}
StoredLineCacheCosts stored_line_output_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,StoredLineCacheCosts previous,std::stop_token stop){
    auto prepared=prepare_line_output(g,ctx,sampling,stop,previous);auto& plan=prepared.plan;
    if(g.empty()){if(plan.storage>512ull*1024*1024-previous.storage)throw std::length_error("Empty Line output budget exceeds limits");plan.storage+=previous.storage;}
    charge(plan.admission.work,plan.admission.work-previous.work);
    return {plan.storage,plan.admission.work};
}
namespace {
Raster render_line_output(const LineOutputPlan& prepared,std::stop_token stop,bool fresh_word_cache=false){
    const auto& plan=prepared.plan;const auto& local=prepared.context;
    if(!plan.admission.points)return Raster(local.width,local.height,local.depth,local.resolution);
    return render_outlines(plan.planes,local,plan.admission,stop,[&](std::size_t p,Scratch& scratch,VectorBounds raw,VectorBounds block){
        for(const auto& command:plan.ranges[p]){cancelled(stop);if(!intersects(command.gate,raw))continue;
            for(auto n=command.begin;n<command.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,local,plan.admission,stop);}
    },prepared.fractional4,false,false,nullptr,nullptr,fresh_word_cache);
}
}
Raster render_stored_line_output(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,std::stop_token stop){
    return render_line_output(prepare_line_output(g,ctx,sampling,stop),stop);
}
Raster render_stored_temp_print_cache(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,std::stop_token stop){
    return render_line_output(prepare_line_output(g,ctx,StoredLineOutputSampling::Plain1x,stop),stop,true);
}
StoredLineCacheCosts stored_line_reference_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,StoredLineCacheCosts previous,std::stop_token stop){
    auto prepared=prepare_line_output(g,ctx,sampling,stop,previous,ContextProfile::ReferenceOutput);auto& plan=prepared.plan;
    if(g.empty()){if(plan.storage>512ull*1024*1024-previous.storage)throw std::length_error("Empty Line reference budget exceeds limits");plan.storage+=previous.storage;}
    charge(plan.admission.work,plan.admission.work-previous.work);return {plan.storage,plan.admission.work};
}
Raster render_stored_line_reference(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,std::stop_token stop){
    return render_line_output(prepare_line_output(g,ctx,sampling,stop,{},ContextProfile::ReferenceOutput),stop);
}
StoredLineCacheCosts stored_line_direct_costs(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,StoredLineCacheCosts previous,std::stop_token stop){
    auto prepared=prepare_line_output(g,ctx,sampling,stop,previous,ContextProfile::DirectOutput);auto& plan=prepared.plan;
    if(g.empty()){if(plan.storage>512ull*1024*1024-previous.storage)throw std::length_error("Empty Line direct budget exceeds limits");plan.storage+=previous.storage;}
    charge(plan.admission.work,plan.admission.work-previous.work);return {plan.storage,plan.admission.work};
}
Raster render_stored_line_direct(std::span<const StoredLinePlane> g,const VectorRenderContext& ctx,StoredLineOutputSampling sampling,std::stop_token stop){
    return render_line_output(prepare_line_output(g,ctx,sampling,stop,{},ContextProfile::DirectOutput),stop);
}
namespace {
struct BitScratch {
    VectorBounds bounds;
    BinaryMask& mask;
    int cursor_x{-1},cursor_y{-1};
    void move_cursor(int x,int y){
        cursor_x=x;cursor_y=y;
        //AutoLock's write flag also materializes valid cursor-only blocks.
        if(x>=0&&x<mask.width()&&y>=0&&y<mask.height())mask.materialize_block(x/256,y/256);
    }
    void begin_scanlines(int y){move_cursor(cursor_x,y);}
    void next_scanline(){move_cursor(cursor_x,cursor_y+1);}
    void horizontal_scanline(int y){move_cursor(cursor_x,y);}
    void write(int x,int y,Pixel pigment,bool partial=false,double=1){
        if(partial)throw std::invalid_argument("Transient1bit Line does not admit fractional spans");
        if(x<bounds.left||x>bounds.right||x>=mask.width()||y<bounds.top||y>=bounds.bottom)return;
        //59C2F0 maps alpha WORD0 to transmit and every other alpha to
        //not-transmit.5DD990/5DDCF0 overwrite1bit spans directly in mode0.
        //The callback's MoveCursor/LockBlock allocates even for a clear;
        //RCBlockGetter and the constructor's negative cursor do not.
        mask.materialize_block(x/256,y/256);
        mask.set(x,y,pigment.a!=0);
    }
    void span(int y,std::int64_t left,std::int64_t right,Pixel pigment){
        const auto sar8=[](std::int64_t n){return n>=0?n/256:-((-n+255)/256);};
        const auto begin=int(std::max<std::int64_t>(bounds.left,sar8(left)));
        //5DD990's edge flag1 includes the clip's right column. RectFill
        //still uses the separate exclusive rectangle loop in fill().
        const auto end=int(std::min({std::int64_t(bounds.right)+1,std::int64_t(mask.width()),sar8(right)+1}));
        for(int x=begin;x<end;++x)write(x,y,pigment);
        if(begin<end)move_cursor(end,y); //FillLineRowForPolygon advances after its last covered pixel.
    }
};
}
namespace {
BitLineMask render_bit_line(const StoredLinePlane& source,const VectorRenderContext& context,
                            StoredLineCacheCosts previous,std::stop_token stop,ContextProfile profile,float minimum=0){
    cancelled(stop);
    if(!context.clip)throw std::invalid_argument("Transient1bit Line requires its native trace clip");
    (void)admit(VectorLineGeometry{},context,false,true,false,profile);
    constexpr std::uint64_t limit=512ull*1024*1024;
    if(previous.storage>limit)throw std::length_error("Transient1bit Line prior storage exceeds limits");
    const auto trace=*context.clip;
    const auto expand=[](int value,int delta){const auto n=std::int64_t(value)+delta;
        if(n<INT32_MIN||n>INT32_MAX)throw std::invalid_argument("Transient1bit Line clip inflation exceeds signed32");return int(n);};
    auto local=context;
    //62E820 and62F030 inflate the caller clip by2 BEFORE offscreen
    //intersection; the tracing caller still receives its original clip.
    local.clip=VectorBounds{expand(trace.left,-2),expand(trace.top,-2),expand(trace.right,2),expand(trace.bottom,2)};
    const auto words=((std::uint64_t(context.width)+31)/32)*std::uint64_t(context.height);
    const auto blocks=((std::uint64_t(context.width)+255)/256)*((std::uint64_t(context.height)+255)/256);
    const auto backing=words*sizeof(std::uint32_t)+blocks+sizeof(BinaryMask);
    if(backing>limit-previous.storage)throw std::length_error("Transient1bit Line backing exceeds limits");
    //Each private profile retains its native raw scale and LC order, while
    //sharing stored gates, body/CP emitters and the direct1bit block sink.
    const auto plan=prepare_stored_lines(std::span<const StoredLinePlane>(&source,1),local,stop,
        previous.storage+backing,previous.work,false,true,profile,minimum);
    auto costs=StoredLineCacheCosts{plan.storage,plan.admission.work};
    charge(costs.work,words+blocks);
    const auto clip=plan.admission.clip;
    const auto columns=clip.right>clip.left?std::uint64_t((clip.right-1)/256-clip.left/256+1):0;
    const auto rows=clip.bottom>clip.top?std::uint64_t((clip.bottom-1)/256-clip.top/256+1):0;
    charge(costs.work,columns*rows*(source.lines.strokes.size()+1));
    if(profile==ContextProfile::BitCircle1x){
        //Later constructors may clear blocks materialized by a preceding
        //inclusive span/cursor. Prepay all partial clears/full releases.
        charge(costs.work,columns*rows+std::uint64_t(clip.right-clip.left)*std::uint64_t(clip.bottom-clip.top));
    }
    //Inclusive span endpoints may touch the column in the next physical
    //block. Dense mask/block backing above covers that block; reserve the
    //extra per-row boundary writes before allocation as well.
    charge(costs.work,columns*std::uint64_t(clip.bottom-clip.top)*(plan.admission.points+1));
    cancelled(stop);
    BitLineMask result{BinaryMask(context.width,context.height),trace,clip,costs};
    cancelled(stop); //Allocation may itself deliver cancellation, even when drawclip is empty.
    //The first block starts fresh. Later62E820 constructors clear any
    //handle created by earlier inclusive spans/cursors before drawing.
    if(clip.left>=clip.right||clip.top>=clip.bottom)return result;
    for(int ty=clip.top/256;ty*256<clip.bottom;++ty)for(int tx=clip.left/256;tx*256<clip.right;++tx){
        cancelled(stop);
        //RCBlockGetter(...,1) returns clip intersected with each physical
        //block. This rectangle also controls native point accumulation.
        const VectorBounds block{std::max(clip.left,tx*256),std::max(clip.top,ty*256),
                                 std::min(clip.right,(tx+1)*256),std::min(clip.bottom,(ty+1)*256)};
        if(profile==ContextProfile::BitCircle1x&&result.mask.block_present(tx,ty)){
            //SDK10551940 skips absent handles, frees exact256-square
            //coverage, or calls depth1 RectFill(mode1,0) on partial coverage.
            //The latter overwrites a half-open rect without a final X/Y step.
            if(block==VectorBounds{tx*256,ty*256,(tx+1)*256,(ty+1)*256})result.mask.release_block(tx,ty);
            else for(int y=block.top;y<block.bottom;++y){cancelled(stop);
                for(int x=block.left;x<block.right;++x)result.mask.set(x,y,false);}
        }
        const auto raw=raw_block(block,local);
        BitScratch scratch{block,result.mask};
        for(const auto& command:plan.ranges.front()){
            cancelled(stop);if(!intersects(command.gate,raw))continue;
            for(auto n=command.begin;n<command.end;++n)
                draw_outline(plan.planes.front()[n],scratch,raw,block,local,plan.admission,stop);
        }
    }
    cancelled(stop);return result;
}
}
void draw_selection_line_bits(const StoredLinePlane& source,std::size_t index,const VectorRenderContext& context,
                              BinaryMask& target,bool minimum_width,StoredLineCacheCosts& costs,std::stop_token stop){
    cancelled(stop);
    if(!context.clip||context.clipboard_affine||context.converter_fine_step||context.origin.x!=std::trunc(context.origin.x)||context.origin.y!=std::trunc(context.origin.y)
        ||target.width()!=context.width||target.height()!=context.height||index>=source.lines.strokes.size()||source.line_bounds.size()!=source.lines.strokes.size())
        throw std::invalid_argument("Invalid color-selection Line target");
    (void)admit(VectorLineGeometry{},context,false,true,false,ContextProfile::BitSelect1x);
    // Copy one LC and its own CP slice. All saved model gates stay borrowed
    // from the decoded source; source/backing resources are charged once.
    StoredLinePlane one;const auto& stroke=source.lines.strokes[index];
    std::uint64_t copy=sizeof(StoredLinePlane)+sizeof(VectorLineStroke)+stroke.spans.size()*(sizeof(VectorLineSpan)+sizeof(VectorSpanProfiles)+128);
    for(const auto& span:stroke.spans)copy+=span.width_nodes.size()*sizeof(VectorWidthNode);
    if(costs.storage>512ull*1024*1024||copy>512ull*1024*1024-costs.storage)throw std::length_error("Color-selection Line copy exceeds storage limit");
    one.lines.strokes.push_back(stroke);one.line_bounds.push_back(source.line_bounds[index]);
    for(const auto& span:stroke.spans)one.model_bounds.emplace(span.model_index,source.model_bounds.at(span.model_index));
    if(!source.line_profiles.empty()){
        std::size_t first{};for(std::size_t i=0;i<index;++i)first+=source.lines.strokes[i].spans.size();
        if(first>source.line_profiles.size()||stroke.spans.size()>source.line_profiles.size()-first)throw std::invalid_argument("Invalid color-selection CP slice");
        one.line_profiles.assign(source.line_profiles.begin()+first,source.line_profiles.begin()+first+stroke.spans.size());
    }
    const auto plan=prepare_stored_lines(std::span<const StoredLinePlane>(&one,1),context,stop,costs.storage+copy,costs.work,false,true,
        ContextProfile::BitSelect1x,minimum_width?std::bit_cast<float>(std::uint32_t{0x4119999a}):0);
    costs.work=plan.admission.work;const auto clip=plan.admission.clip;
    const auto columns=clip.right>clip.left?std::uint64_t((clip.right-1)/256-clip.left/256+1):0;
    const auto rows=clip.bottom>clip.top?std::uint64_t((clip.bottom-1)/256-clip.top/256+1):0;
    charge(costs.work,columns*rows+columns*std::uint64_t(clip.bottom-clip.top)*(plan.admission.points+1));
    for(int ty=clip.top/256;ty*256<clip.bottom;++ty)for(int tx=clip.left/256;tx*256<clip.right;++tx){
        cancelled(stop);const VectorBounds block{std::max(clip.left,tx*256),std::max(clip.top,ty*256),std::min(clip.right,(tx+1)*256),std::min(clip.bottom,(ty+1)*256)};
        const auto raw=raw_block(block,context);BitScratch scratch{block,target};
        for(const auto& command:plan.ranges.front())if(intersects(command.gate,raw))for(auto n=command.begin;n<command.end;++n)
            draw_outline(plan.planes.front()[n],scratch,raw,block,context,plan.admission,stop);
    }
    cancelled(stop);
}
Raster render_direct_line_bits(std::span<const StoredLinePlane> source,const VectorRenderContext& context,const Raster& target,StoredLineCacheCosts& costs){
    if(target.depth!=1||target.width!=context.width||target.height!=context.height)throw std::invalid_argument("Invalid direct Line bit target");
    const auto words=((std::uint64_t(target.width)+31)/32)*target.height;
    if(words>512ull*1024*1024/4)throw std::length_error("Direct Line mask storage exceeds limit");
    const float seed=float(context.base_scale*8.0),minimum=float(seed*std::bit_cast<float>(std::uint32_t{0x3f19999a}));
    const auto plan=prepare_stored_lines(source,context,{},costs.storage+words*4,costs.work,false,true,ContextProfile::Color,minimum,true);
    costs={plan.storage,plan.admission.work};charge(costs.work,std::uint64_t(target.width)*target.height*2);
    BinaryMask mask(target.width,target.height);
    for(const auto& [tile,bytes]:target.tiles){if(!bytes||bytes->size()!=target.tile_bytes()||tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=target.width||std::int64_t(tile.y)*256>=target.height)throw std::invalid_argument("Malformed direct Line bit tile");mask.materialize_block(tile.x,tile.y);
        for(int y=tile.y*256;y<std::min(target.height,(tile.y+1)*256);++y)for(int x=tile.x*256;x<std::min(target.width,(tile.x+1)*256);++x)mask.set(x,y,((*bytes)[std::size_t(y%256)*32+(x%256)/8]>>(7-x%8))&1);}
    const auto clip=plan.admission.clip;
    for(int ty=clip.top/256;ty*256<clip.bottom;++ty)for(int tx=clip.left/256;tx*256<clip.right;++tx){
        const VectorBounds block{std::max(clip.left,tx*256),std::max(clip.top,ty*256),std::min(clip.right,(tx+1)*256),std::min(clip.bottom,(ty+1)*256)};const auto raw=raw_block(block,context);BitScratch scratch{block,mask};
        for(std::size_t p=0;p<source.size();++p)for(const auto& command:plan.ranges[p])if(intersects(command.gate,raw))for(auto n=command.begin;n<command.end;++n)draw_outline(plan.planes[p][n],scratch,raw,block,context,plan.admission,{});
    }
    Raster result(target.width,target.height,1,target.resolution);
    for(int ty=0;ty*256<target.height;++ty)for(int tx=0;tx*256<target.width;++tx)if(mask.block_present(tx,ty)){
        auto bytes=std::make_shared<Bytes>(8192,0);for(int y=ty*256;y<std::min(target.height,(ty+1)*256);++y)for(int x=tx*256;x<std::min(target.width,(tx+1)*256);++x)if(mask.test(x,y))(*bytes)[std::size_t(y%256)*32+(x%256)/8]|=std::uint8_t(1u<<(7-x%8));result.tiles[{tx,ty}]=std::move(bytes);
    }
    return result;
}
BitLineMask render_stored_line_bit_mask(const StoredLinePlane& source,const VectorRenderContext& context,
                                      StoredLineCacheCosts previous,std::stop_token stop){
    return render_bit_line(source,context,previous,stop,ContextProfile::BitMask);
}
BitLineMask render_stored_line_circle_mask_4x(const StoredLinePlane& source,const VectorRenderContext& context,
                                             BitCircleMaskOptions options,StoredLineCacheCosts previous,std::stop_token stop){
    cancelled(stop);
    const float minimum=options.minimum_width?float(numeric_detail::binary64(context.base_scale*double(std::bit_cast<float>(std::uint32_t{0x3f19999a})))):0;
    return render_bit_line(source,context,previous,stop,ContextProfile::BitMask,minimum);
}
BitLineMask render_stored_line_circle_eraser_mask_4x(const StoredLinePlane& source,const VectorRenderContext& context,
                                                   StoredLineCacheCosts previous,std::stop_token stop){
    return render_bit_line(source,context,previous,stop,ContextProfile::BitMask,std::bit_cast<float>(std::uint32_t{0x4119999a}));
}
BitLineMask render_stored_line_circle_mask_1x(const StoredLinePlane& source,const VectorRenderContext& context,
                                             BitCircleMaskOptions options,StoredLineCacheCosts previous,std::stop_token stop){
    cancelled(stop);
    const float minimum=options.minimum_width?float(numeric_detail::binary64(context.base_scale*double(std::bit_cast<float>(std::uint32_t{0x3f19999a})))):0;
    return render_bit_line(source,context,previous,stop,ContextProfile::BitCircle1x,minimum);
}
}

} // namespace retas
