#include "retas/selection.hpp"
#include "retas/fill.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <vector>

namespace retas {
namespace {
void check_stop(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("选区运算已取消。");}
int checked(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
        throw std::invalid_argument("选区形态运算坐标超出支持范围。");
    return int(value);
}
void pass(const BinaryMask& source,BinaryMask& destination,Rect work,int radius,bool vertical,bool expand,std::stop_token stop){
    const int first=vertical?work.y:work.x,length=vertical?work.height:work.width;
    const int other_first=vertical?work.x:work.y,other_end=other_first+(vertical?work.width:work.height);
    const int physical=vertical?source.height():source.width(),tiles=(physical+255)/256;
    std::vector<int> counts(std::size_t(length)+1),visits(std::size_t(tiles)+1);
    for(int line=other_first;line<other_end;++line){
        check_stop(stop);counts[0]=0;std::fill(visits.begin(),visits.end(),0);
        for(int i=0;i<length;++i)counts[i+1]=counts[i]+int(vertical?source.test(line,first+i):source.test(first+i,line));
        // Each interval includes the cursor after its final write. Moving a
        // write cursor alone materializes backing, even for a zero-length run.
        const auto visit=[&](int begin,int end){
            begin=std::max(0,begin);end=std::min(physical-1,end);if(begin>end)return;
            ++visits[begin/256];--visits[end/256+1];
        };
        bool previous=length>0&&counts[1]!=0;
        for(int i=0;i<length;++i){
            const bool current=counts[i+1]!=counts[i];
            const int low=int(std::max<std::int64_t>(0,std::int64_t(i)-radius));
            const int high=int(std::min<std::int64_t>(length,std::int64_t(i)+radius+1));
            const int selected=counts[high]-counts[low];
            if(expand?selected>0:selected<high-low){
                if(vertical)destination.set(line,first+i,expand);else destination.set(first+i,line,expand);
            }
            if(expand){
                if(previous){
                    visit(first+i,first+i);
                    if(!current){previous=false;visit(first+i,first+int(std::min<std::int64_t>(length,std::int64_t(i)+radius)));}
                }else if(current){previous=true;visit(first+low,first+i+1);}
            }else if(previous&&!current){previous=false;visit(first+low,first+i);}
            else if(!previous&&current){previous=true;visit(first+i,first+int(std::min<std::int64_t>(length,std::int64_t(i)+radius)));}
        }
        int active{};for(int tile=0;tile<tiles;++tile){active+=visits[tile];if(active>0){
            if(vertical)destination.materialize_block(line/256,tile);else destination.materialize_block(tile,line/256);
        }}
    }
}
}
int selection_morphology_radius(double millimeters,double carrier_dpi){
    if(!std::isfinite(millimeters)||millimeters<0||!std::isfinite(carrier_dpi)||carrier_dpi<=0)
        throw std::invalid_argument("选区距离或载体分辨率无效。");
    const auto result=legacy_rt_double_to_int(numeric_detail::binary64(
        numeric_detail::binary64(millimeters*carrier_dpi)*0.03937007874015748));
    if(!result||*result<0)throw std::invalid_argument("选区距离超出支持范围。");
    return *result;
}
RegionSelection morph_selection(const RegionSelection& current,int page_width,int page_height,
    SelectionMorphology mode,int radius,std::stop_token stop){
    if(current.state==SelectionLayerState::Cleared||current.state==SelectionLayerState::FullPlane)return current;
    if(current.state<SelectionLayerState::Rectangle||current.state>SelectionLayerState::VectorIndicated
        ||(mode!=SelectionMorphology::Expand&&mode!=SelectionMorphology::Shrink)||radius<0)
        throw std::invalid_argument("选区形态运算参数无效。");
    const auto size=selection_carrier_size(current,page_width,page_height);check_stop(stop);
    if(current.bounds.width<0||current.bounds.height<0||(!current.mask&&current.state!=SelectionLayerState::Rectangle))
        throw std::invalid_argument("选区形态运算需要有效边界和物理遮罩。");
    const bool expand=mode==SelectionMorphology::Expand;const int padding=expand?radius:1;
    std::int64_t left=std::int64_t(current.bounds.x)-padding,top=std::int64_t(current.bounds.y)-padding;
    std::int64_t right=std::int64_t(current.bounds.x)+current.bounds.width+padding,bottom=std::int64_t(current.bounds.y)+current.bounds.height+padding;
    const auto pl=-std::int64_t(current.origin[0]),pt=-std::int64_t(current.origin[1]);
    const auto pr=pl+page_width,pb=pt+page_height;
    for(auto edge:{left,top,right,bottom,pl,pt,pr,pb})(void)checked(edge);
    const auto il=std::max(left,pl),it=std::max(top,pt),ir=std::min(right,pr),ib=std::min(bottom,pb);
    // Native IntersectRect leaves its destination unchanged on an empty
    // intersection; an off-page operation can still visit valid carrier bits.
    if(il<ir&&it<ib){left=il;top=it;right=ir;bottom=ib;}
    if(left<0||top<0||right>size[0]||bottom>size[1])
        throw std::invalid_argument("选区形态运算工作区超出安全载体范围。");
    const Rect work{int(left),int(top),checked(right-left),checked(bottom-top)};
    auto output=current.mask?std::make_shared<BinaryMask>(*current.mask):std::make_shared<BinaryMask>(size[0],size[1]);
    if(!current.mask){
        const int l=int(std::clamp<std::int64_t>(current.bounds.x,0,size[0])),t=int(std::clamp<std::int64_t>(current.bounds.y,0,size[1]));
        const int r=int(std::clamp<std::int64_t>(std::int64_t(current.bounds.x)+current.bounds.width,l,size[0]));
        const int b=int(std::clamp<std::int64_t>(std::int64_t(current.bounds.y)+current.bounds.height,t,size[1]));
        for(int y=t;y<b;++y){check_stop(stop);for(int x=l;x<r;++x)output->set(x,y);}
    }
    auto scratch=expand?BinaryMask(size[0],size[1]):*output;
    pass(*output,scratch,work,radius,true,expand,stop);
    if(!expand)*output=scratch;
    pass(scratch,*output,work,radius,false,expand,stop);check_stop(stop);
    auto result=current;result.carrier_size=size;result.bounds=expand?work:output->extent({0,0,size[0],size[1]});
    if(!expand&&(result.bounds.width<=0||result.bounds.height<=0)){
        result.state=SelectionLayerState::Cleared;result.bounds={};result.origin={};result.mask.reset();
        result.vector_cache.reset();result.vector_cache_origin={};return result;
    }
    result.state=SelectionLayerState::Mask;result.mask=std::move(output);
    if(current.state==SelectionLayerState::VectorIndicated){result.vector_cache.reset();result.vector_cache_origin={};}
    return result;
}
}
