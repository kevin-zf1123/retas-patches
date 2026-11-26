#pragma once
#include "retas/vector_face_render.hpp"
#include "vector_clipboard_context.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>

namespace retas::vector_scan_detail {
inline bool contains(VectorBounds b,VectorFaceVertex p){return p.x>=b.left&&p.x<b.right&&p.y>=b.top&&p.y<b.bottom;}
inline bool intersects(VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;}
// Native raw routing and boundary trees. The consumer supplies its point/work
// budget and Region containment callback; materials and span writers are separate.
template<class Budget> struct Router {
    using P=VectorFaceVertex;
    VectorBounds raw,block;const VectorRenderContext& context;Budget& budget;
    std::vector<P> points;std::array<std::map<int,bool>,4> boundary;
    P last{},first{};bool started{},inside_run{},last_valid{},reference{};
    bool inside(P p)const{return contains(raw,p);}
    P quantize(P p,bool sampled=true)const{
        if(!context.clipboard_affine&&(reference?(!std::isfinite(context.base_scale)||context.base_scale<=0):(context.base_scale!=4&&context.base_scale!=16)))throw std::invalid_argument("Native Router requires raw4/raw16 quantization");
        const int scale=sampled?context.oversample:1;
        if(context.clipboard_affine){const auto q=vector_render_detail::clipboard_quantize({double(p.x),double(p.y)},context,unsigned(scale));return {int(q.x),int(q.y)};}
        if(reference){const auto reciprocal=numeric_detail::binary64(1.0/context.base_scale),factor=numeric_detail::binary64(double(scale)*reciprocal);
            return {vector_render_detail::clipboard_integer(std::floor(numeric_detail::binary64(double(p.x)*factor))),vector_render_detail::clipboard_integer(std::floor(numeric_detail::binary64(double(p.y)*factor)))};}
        return {int(std::floor(double(p.x)*scale/context.base_scale)-context.origin.x*scale),int(std::floor(double(p.y)*scale/context.base_scale)-context.origin.y*scale)};
    }
    void append(P point){const auto q=quantize(point);if(points.empty()||points.back()!=q){budget.point();points.push_back(q);}}
    void sentinel(){if(points.empty())return;budget.point();points.push_back({0,std::numeric_limits<int>::max()});}
    std::vector<P> intersections(P a,P b)const{
        std::vector<P> result;result.reserve(2);
        const VectorBounds box{std::min(a.x,b.x),std::min(a.y,b.y),std::max(a.x,b.x)+1,std::max(a.y,b.y)+1};
        if(a==b||!intersects(raw,box))return result;
        if(a.x==b.x){
            if(box.top<=raw.top&&box.bottom>raw.top)result.push_back({a.x,raw.top});
            if(box.top<=raw.bottom&&box.bottom>raw.bottom)result.push_back({a.x,raw.bottom});
        }else if(a.y==b.y){
            if(box.left<=raw.left&&box.right>raw.left)result.push_back({raw.left,a.y});
            if(box.left<=raw.right&&box.right>raw.right)result.push_back({raw.right,a.y});
        }else{
            const double slope=double(std::int64_t(a.y)-b.y)/double(std::int64_t(a.x)-b.x),inverse=1/slope;
            const auto vertical=[&](int x){const double y=double(std::int64_t(x)-a.x)*slope+a.y;if(y>=raw.top&&y<=raw.bottom)result.push_back({x,int(std::floor(y))});};
            if(box.left<raw.left)vertical(raw.left);if(box.right>raw.right)vertical(raw.right);
            const auto horizontal=[&](int y){const double x=double(std::int64_t(y)-a.y)*inverse+a.x;
                if((x>raw.left&&x<raw.right)||((x==raw.left||x==raw.right)&&result.empty()))result.push_back({int(std::floor(x)),y});};
            if(box.top<raw.top)horizontal(raw.top);if(box.bottom>raw.bottom)horizontal(raw.bottom);
        }
        if(result.size()>2)throw std::invalid_argument("Ambiguous vector face boundary intersection");return result;
    }
    void event(P p){
        const bool kind=!inside_run;bool matched{};
        for(unsigned side:{0u,2u,1u,3u}){
            const bool nearby=side==0?p.x<=raw.left+1:side==2?p.x>=raw.right-1:side==1?p.y<=raw.top+1:p.y>=raw.bottom-1;
            if(!nearby)continue;const int coordinate=side==0||side==2?p.y:p.x;
            auto& bucket=boundary[side];auto i=bucket.lower_bound(coordinate-1);
            for(;i!=bucket.end()&&i->first<=coordinate+1;++i)if(i->second!=kind){
                if(side==0||side==2){p.x=side==0?raw.left:raw.right;p.y=i->first;}
                else {p.y=side==1?raw.top:raw.bottom;p.x=i->first;}
                bucket.erase(i);matched=true;break;
            }
            if(matched)break;
        }
        if(!matched){
            if(p.x==raw.left)boundary[0][p.y]=kind;else if(p.x==raw.right)boundary[2][p.y]=kind;
            else if(p.y==raw.top)boundary[1][p.x]=kind;else if(p.y==raw.bottom)boundary[3][p.x]=kind;
        }
        append(p);const bool old=inside_run;inside_run=!inside_run;if(old)sentinel();
    }
    void transition(P p){
        auto hit=intersections(p,last);
        if(hit.size()==1)event(hit[0]);else if(hit.size()==2){
            if(hit[0]==p||hit[0]==last)event(hit[1]);else if(hit[1]==p||hit[1]==last)event(hit[0]);
        }
    }
    void outside_segment(P p){
        const auto hit=intersections(last,p);if(hit.size()!=2)return;
        const auto a=quantize(hit[0],false),b=quantize(hit[1],false);
        if(a.x==b.x&&(a.x==block.left||a.x==block.right)&&!((a.y==block.top&&b.y==block.bottom)||(a.y==block.bottom&&b.y==block.top)))return;
        if(a.y==b.y&&(a.y==block.top||a.y==block.bottom)&&!((a.x==block.left&&b.x==block.right)||(a.x==block.right&&b.x==block.left)))return;
        const auto dx=std::int64_t(p.x)-last.x,dy=std::int64_t(p.y)-last.y;
        const auto parameter=[&](P q){return float(std::abs(dx)>std::abs(dy)?double(std::int64_t(q.x)-last.x)/dx:double(std::int64_t(q.y)-last.y)/dy);};
        const float ta=parameter(hit[0]),tb=parameter(hit[1]);
        if(ta<tb){event(hit[0]);event(hit[1]);}else if(ta>tb){event(hit[1]);event(hit[0]);}
    }
    void route(P p){
        budget.charge(1);
        if(!started){started=true;last_valid=true;first=last=p;inside_run=inside(p);if(inside_run)append(p);return;}
        if(p==last)return;
        if(inside(p)){if(!inside_run)transition(p);append(p);}else if(inside_run)transition(p);else if(last_valid)outside_segment(p);
        last=p;last_valid=true;
    }
    void flush(){
        // 59D350 calls 5E11D0 with literal0: emit sentinel, never invent a
        // closing segment. Valid graph identity closure emitted its endpoint.
        if(started)sentinel();inside_run=false;started=false;last_valid=false;
    }
    void flatten(unsigned side){
        const bool vertical=side==0||side==2,flag=side==1||side==2;
        const int fixed=side==0?raw.left:side==2?raw.right:side==1?raw.top:raw.bottom;
        const auto point=[&](int coordinate){return vertical?P{fixed,coordinate}:P{coordinate,fixed};};
        auto& bucket=boundary[side];auto i=bucket.begin();if(i==bucket.end())return;
        bool extend=flag?i->second:!i->second;
        while(i!=bucket.end()){
            if(extend){append(point(vertical?raw.top:raw.left));extend=false;}
            else {if(i->second!=flag)append(point(i->first));++i;}
            if(i==bucket.end())append(point(vertical?raw.bottom:raw.right));
            else {if(i->second==flag)append(point(i->first));++i;}
            sentinel();
        }
    }
    void synthesize(unsigned side){
        const auto nonempty=[&](unsigned s){return !boundary[s].empty();};
        const auto first_kind=[&](unsigned s){return boundary[s].begin()->second;};
        const auto last_kind=[&](unsigned s){return boundary[s].rbegin()->second;};bool emit{};
        if(side==0||side==2){
            const bool left=side==0;
            if(nonempty(1))emit=left?first_kind(1):!last_kind(1);
            else if(nonempty(3))emit=left?!first_kind(3):last_kind(3);
            else if(nonempty(0))emit=!first_kind(0);else if(nonempty(2))emit=first_kind(2);
            if(emit){const int x=left?raw.left:raw.right;append({x,raw.top});append({x,raw.bottom});sentinel();}
        }else{
            const bool top=side==1;
            if(nonempty(0))emit=top?!first_kind(0):last_kind(0);
            else if(nonempty(2))emit=top?first_kind(2):!last_kind(2);
            else if(nonempty(1))emit=first_kind(1);else if(nonempty(3))emit=!first_kind(3);
            if(emit){const int y=top?raw.top:raw.bottom;append({raw.left,y});append({raw.right,y});sentinel();}
        }
    }
    template<class Contains> bool block_probe(Contains&& test){
        // 5DD750: top-left must be inside, and then top-left minus(1,1).
        // 5A1CF0 maps any outside/hole hit to0, otherwise1; it never returns2.
        return test(P{raw.left,raw.top})&&test(P{raw.left-1,raw.top-1});
    }
    template<class Contains> void finish(Contains&& test){
        if(points.empty())return;
        if(std::all_of(boundary.begin(),boundary.end(),[](const auto& b){return b.empty();})){
            if(block_probe(test)){
                append({raw.left,raw.top});append({raw.right,raw.top});append({raw.right,raw.bottom});append({raw.left,raw.bottom});append({raw.left,raw.top});
                sentinel();
            }
            return;
        }
        for(unsigned side=0;side<4;++side){if(boundary[side].empty())synthesize(side);else flatten(side);}
    }
};
}
