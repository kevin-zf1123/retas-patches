#include "retas/selection.hpp"
#include "retas/fill.hpp"
#include "selection_mask_source.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
RasterSelection RasterSelection::frozen() const{
    auto copy=*this;if(mask_)copy.mask_=std::make_shared<const BinaryMask>(*mask_);return copy;
}
namespace {
Rect clip(Rect r,int width,int height){
    const auto left=std::clamp<std::int64_t>(r.x,0,width),top=std::clamp<std::int64_t>(r.y,0,height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),left,width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),top,height);
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
bool empty(Rect r){return r.width<=0||r.height<=0;}
Rect unite(Rect a,Rect b){
    if(empty(a))return b;if(empty(b))return a;
    const auto left=std::min(a.x,b.x),top=std::min(a.y,b.y);
    const auto right=std::max(std::int64_t(a.x)+a.width,std::int64_t(b.x)+b.width);
    const auto bottom=std::max(std::int64_t(a.y)+a.height,std::int64_t(b.y)+b.height);
    if(right-left>std::numeric_limits<int>::max()||bottom-top>std::numeric_limits<int>::max())throw std::invalid_argument("选区边界超出支持范围。");
    return {left,top,int(right-left),int(bottom-top)};
}
}
void selection_detail::copy_mask_rectangle(BinaryMask& destination,const BinaryMask& source,Rect request,std::stop_token stop){
    const auto poll=[&]{if(stop.stop_requested())throw std::runtime_error("Selection layer copy cancelled");};poll();
    // Native one-bit Copy(mode1), equal grids/coordinates, independent sizes. Full-surface
    // equality is tested before clipping; CopyAll preserves present-zero.
    if(destination.width()==source.width()&&destination.height()==source.height()&&request==Rect{0,0,source.width(),source.height()}){destination=source;return;}
    const auto clipped=clip(request,std::min(source.width(),destination.width()),std::min(source.height(),destination.height()));if(empty(clipped))return;
    const int right=clipped.x+clipped.width,bottom=clipped.y+clipped.height;
    for(int ty=clipped.y/256;ty<=(bottom-1)/256;++ty)for(int tx=clipped.x/256;tx<=(right-1)/256;++tx){poll();
        const int left=std::max(clipped.x,tx*256),top=std::max(clipped.y,ty*256);
        const int r=std::min(right,(tx+1)*256),b=std::min(bottom,(ty+1)*256);
        if(!source.block_present(tx,ty)&&left==tx*256&&top==ty*256&&r==(tx+1)*256&&b==(ty+1)*256){
            destination.release_block(tx,ty);continue;
        }
        destination.materialize_block(tx,ty);
        for(int y=top;y<b;++y){poll();for(int x=left;x<r;++x)destination.set(x,y,source.test(x,y));}
        // Same-coordinate Copy1's partial-width row path calls NextCursorY
        // after its final row, even for a missing source. The256-wide raw
        // memcpy/memset branch above has no terminal cursor advance.
        if(r-left<256&&b<destination.height())destination.materialize_block(tx,b/256);
    }
}
namespace {
void clear_mask_rectangle(BinaryMask& mask,std::int64_t left,std::int64_t top,std::int64_t right,std::int64_t bottom){
    const auto request_left=left,request_top=top,request_right=right,request_bottom=bottom;
    left=std::clamp<std::int64_t>(left,0,mask.width());top=std::clamp<std::int64_t>(top,0,mask.height());
    right=std::clamp<std::int64_t>(right,left,mask.width());bottom=std::clamp<std::int64_t>(bottom,top,mask.height());
    if(left==right||top==bottom)return;
    for(int ty=int(top)/256;ty<=(bottom-1)/256;++ty)for(int tx=int(left)/256;tx<=(right-1)/256;++tx){
        if(!mask.block_present(tx,ty))continue;
        const int l=int(std::max(left,std::int64_t(tx)*256)),t=int(std::max(top,std::int64_t(ty)*256));
        const int r=int(std::min(right,std::int64_t(tx+1)*256)),b=int(std::min(bottom,std::int64_t(ty+1)*256));
        // ClrRect compares the original request against the nominal tile,
        // before clipping pixel writes to the carrier's logical edge.
        if(request_left<=tx*256&&request_top<=ty*256&&request_right>=(tx+1)*256&&request_bottom>=(ty+1)*256)mask.release_block(tx,ty);
        else for(int y=t;y<b;++y)for(int x=l;x<r;++x)mask.set(x,y,false);
    }
}

}
bool RegionSelection::contains_local(int x,int y) const {
    if(state==SelectionLayerState::FullPlane)return true;
    if(!active()||!bounds.contains(x,y))return false;
    return state==SelectionLayerState::FullPlane||state==SelectionLayerState::Rectangle||(mask&&mask->test(x,y));
}
bool RegionSelection::contains_document(int x,int y) const {
    if(state==SelectionLayerState::FullPlane)return true;
    const auto local_x=std::int64_t(x)-origin[0],local_y=std::int64_t(y)-origin[1];
    if(local_x<std::numeric_limits<int>::min()||local_x>std::numeric_limits<int>::max()
        ||local_y<std::numeric_limits<int>::min()||local_y>std::numeric_limits<int>::max())return false;
    return contains_local(int(local_x),int(local_y));
}
Rect RegionSelection::document_bounds() const noexcept {
    const auto x=std::int64_t(bounds.x)+origin[0],y=std::int64_t(bounds.y)+origin[1];
    if(x>=std::numeric_limits<int>::min()&&x<=std::numeric_limits<int>::max()
        &&y>=std::numeric_limits<int>::min()&&y<=std::numeric_limits<int>::max())return {int(x),int(y),bounds.width,bounds.height};
    const auto left=std::max<std::int64_t>(x,std::numeric_limits<int>::min()),top=std::max<std::int64_t>(y,std::numeric_limits<int>::min());
    const auto right=std::min<std::int64_t>(x+std::max(0,bounds.width),std::int64_t(std::numeric_limits<int>::max())+1);
    const auto bottom=std::min<std::int64_t>(y+std::max(0,bounds.height),std::int64_t(std::numeric_limits<int>::max())+1);
    if(left>=right||top>=bottom)return {};
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
RegionSelection rectangle_selection(Rect bounds,std::array<int,2> origin,std::array<int,2> size){return {SelectionLayerState::Rectangle,bounds,origin,{},size};}
RegionSelection full_selection(int width,int height,std::array<int,2> origin,std::array<int,2> size){
    if(width<0||height<0)throw std::invalid_argument("选区尺寸无效。");
    return {SelectionLayerState::FullPlane,{0,0,width,height},origin,{},size==std::array<int,2>{}?std::array<int,2>{width,height}:size};
}
RegionSelection cleared_selection(std::array<int,2> origin,std::array<int,2> size){return {SelectionLayerState::Cleared,{},origin,{},size};}
std::array<int,2> selection_carrier_size(const RegionSelection& selection,int page_width,int page_height){
    const auto size=selection.carrier_size==std::array<int,2>{}?std::array<int,2>{page_width,page_height}:selection.carrier_size;
    if(page_width<=0||page_height<=0||page_width>100000||page_height>100000||size[0]<=0||size[1]<=0
        ||size[0]>100000||size[1]>100000||std::uint64_t(size[0])*size[1]>128ull*1024*1024)
        throw std::invalid_argument("选区存储尺寸无效或范围过大。");
    if(selection.mask&&(selection.mask->width()!=size[0]||selection.mask->height()!=size[1]))
        throw std::invalid_argument("选区遮罩与存储尺寸不一致。");
    return size;
}
double selection_carrier_dpi(const RegionSelection& selection,double document_dpi){
    const auto dpi=selection.carrier_dpi==0?document_dpi:selection.carrier_dpi;
    if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("选区载体分辨率无效。");return dpi;
}
RegionSelection move_selection(const RegionSelection& current,int page_width,int page_height,int dx,int dy){
    const auto size=selection_carrier_size(current,page_width,page_height);if(!current.active())return current;
    const auto integer=[](std::int64_t value){
        if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::invalid_argument("选区移动坐标超出支持范围。");
        return int(value);
    };
    const auto left=std::int64_t(current.origin[0])+dx,top=std::int64_t(current.origin[1])+dy;
    const auto right=left+size[0],bottom=top+size[1];
    for(const auto edge:{left,top,right,bottom})(void)integer(edge);
    int xpad{},ypad{},width=size[0],height=size[1];
    if(!(left<=0&&top<=0&&right>=page_width&&bottom>=page_height)){
        auto il=std::max<std::int64_t>(0,left),it=std::max<std::int64_t>(0,top);
        auto ir=std::min<std::int64_t>(page_width,right),ib=std::min<std::int64_t>(page_height,bottom);
        if(il>=ir||it>=ib)il=it=ir=ib=0;
        const auto bump=[](std::int64_t value){return value>0?value+1:value;};
        xpad=integer((bump(il)+255)/256*256);ypad=integer((bump(it)+255)/256*256);
        width=integer(std::int64_t(size[0])+xpad+bump(page_width-ir));height=integer(std::int64_t(size[1])+ypad+bump(page_height-ib));
    }
    auto result=current;result.origin={integer(left-xpad),integer(top-ypad)};
    if(result.vector_cache)result.vector_cache_origin={integer(std::int64_t(result.vector_cache_origin[0])+dx),integer(std::int64_t(result.vector_cache_origin[1])+dy)};
    result.bounds.x=integer(std::int64_t(result.bounds.x)+xpad);result.bounds.y=integer(std::int64_t(result.bounds.y)+ypad);result.carrier_size={width,height};
    (void)integer(std::int64_t(result.origin[0])+width);(void)integer(std::int64_t(result.origin[1])+height);
    // Enforce the resource boundary before allocating or publishing any state.
    auto dimensions=result;dimensions.mask.reset();(void)selection_carrier_size(dimensions,page_width,page_height);
    if(width!=size[0]||height!=size[1]||(current.state==SelectionLayerState::FullPlane&&!current.mask)){
        auto mask=std::make_shared<BinaryMask>(width,height);
        // Rect/Full backing may have been clipped before expansion. Materialize
        // its old physical bits, not the shifted bounds on the larger surface.
        const auto scan=current.mask?Rect{0,0,size[0],size[1]}:clip(current.bounds,size[0],size[1]);
        for(int y=scan.y;y<std::int64_t(scan.y)+scan.height;++y)for(int x=scan.x;x<std::int64_t(scan.x)+scan.width;++x)
            if(current.mask?current.mask->test(x,y):current.state==SelectionLayerState::Rectangle||current.state==SelectionLayerState::FullPlane)mask->set(x+xpad,y+ypad);
        if(current.mask)for(int ty=0;ty<(size[1]+255)/256;++ty)for(int tx=0;tx<(size[0]+255)/256;++tx)
            if(current.mask->block_present(tx,ty))mask->materialize_block(tx+xpad/256,ty+ypad/256);
        result.mask=std::move(mask);
    }
    return result;
}
RegionSelection invert_selection(const RegionSelection& current,int page_width,int page_height){
    const auto size=selection_carrier_size(current,page_width,page_height);
    if(current.state<SelectionLayerState::Cleared||current.state>SelectionLayerState::VectorIndicated)
        throw std::invalid_argument("选区状态无效。");
    auto result=current;result.carrier_size=size;
    const auto clear=[&]{
        auto cleared=result;cleared.state=SelectionLayerState::Cleared;cleared.bounds={};cleared.origin={};cleared.mask.reset();
        cleared.vector_cache.reset();cleared.vector_cache_origin={};return cleared;
    };
    if(current.state==SelectionLayerState::FullPlane)return clear();
    if(current.state==SelectionLayerState::Cleared){
        result=clear();result.state=SelectionLayerState::FullPlane;result.bounds={0,0,page_width,page_height};
        auto mask=std::make_shared<BinaryMask>(size[0],size[1]);
        for(int y=0;y<std::min(page_height,size[1]);++y)for(int x=0;x<std::min(page_width,size[0]);++x)mask->set(x,y);
        result.mask=std::move(mask);return result;
    }
    if(!current.mask&&current.state!=SelectionLayerState::Rectangle)
        throw std::invalid_argument("反转选区需要已物化的遮罩。");
    const auto checked=[](std::int64_t value){
        if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
            throw std::invalid_argument("选区反转坐标超出支持范围。");
        return int(value);
    };
    const auto pl=-std::int64_t(current.origin[0]),pt=-std::int64_t(current.origin[1]);
    const auto pr=pl+page_width,pb=pt+page_height;
    const auto bl=std::int64_t(current.bounds.x),bt=std::int64_t(current.bounds.y);
    const auto br=bl+current.bounds.width,bb=bt+current.bounds.height;
    if(current.bounds.width<0||current.bounds.height<0)throw std::invalid_argument("选区边界无效。");
    for(const auto edge:{pl,pt,pr,pb,br,bb})(void)checked(edge);
    const Rect page{int(pl),int(pt),page_width,page_height};
    auto mask=current.mask?std::make_shared<BinaryMask>(*current.mask):std::make_shared<BinaryMask>(size[0],size[1]);
    if(!current.mask){
        const auto old=clip(current.bounds,size[0],size[1]);
        for(int y=old.y;y<old.y+old.height;++y)for(int x=old.x;x<old.x+old.width;++x)mask->set(x,y);
    }
    const auto scan=clip(page,size[0],size[1]);
    for(int y=scan.y;y<scan.y+scan.height;++y)for(int x=scan.x;x<scan.x+scan.width;++x)mask->set(x,y,!mask->test(x,y));
    mask->optimize_blocks(page);
    // Optimization precedes ClrRect. Partial strip clears retain empty blocks.
    if(bl<pl)clear_mask_rectangle(*mask,bl,bt,pl,bb);
    if(bt<pt)clear_mask_rectangle(*mask,bl,bt,br,pt);
    if(br>pr)clear_mask_rectangle(*mask,pr,bt,br,bb);
    if(bb>pb)clear_mask_rectangle(*mask,bl,pb,br,bb);
    result.bounds=bl>pl&&br<pr&&bt>pt&&bb<pb?page:mask->extent({0,0,size[0],size[1]});
    if(empty(result.bounds))return clear();
    result.state=SelectionLayerState::Mask;result.mask=std::move(mask);
    if(current.state==SelectionLayerState::VectorIndicated){result.vector_cache.reset();result.vector_cache_origin={};}
    return result;
}
bool RasterSelection::contains(int x,int y) const noexcept {
    if(!bounds_.contains(x,y))return false;
    if(!masked_)return true;
    const auto mx=std::int64_t(x)+mask_offset_[0],my=std::int64_t(y)+mask_offset_[1];
    return mask_&&mx>=0&&my>=0&&mx<mask_->width()&&my<mask_->height()&&mask_->test(int(mx),int(my));
}
bool RasterSelection::has_materialized_mask() const noexcept {return !masked_||bool(mask_);}
bool RasterSelection::native_mask_contains(int x,int y) const noexcept {
    if(!bounds_.contains(x,y))return false;
    if(!masked_)return true;
    if(!mask_)return false;
    const auto mx=std::int64_t(x)+mask_offset_[0],my=std::int64_t(y)+mask_offset_[1];
    if(mx<0||my<0||mx>=mask_->width()||my>=mask_->height())return true;
    return mask_->test(int(mx),int(my));
}
Rect RasterSelection::extent(Rect within) const noexcept {
    const auto left=std::max<std::int64_t>(within.x,bounds_.x),top=std::max<std::int64_t>(within.y,bounds_.y);
    const auto right=std::min(std::int64_t(within.x)+std::max(0,within.width),std::int64_t(bounds_.x)+std::max(0,bounds_.width));
    const auto bottom=std::min(std::int64_t(within.y)+std::max(0,within.height),std::int64_t(bounds_.y)+std::max(0,bounds_.height));
    if(left>=right||top>=bottom)return {};
    if(!masked_)return {int(left),int(top),int(right-left),int(bottom-top)};
    if(!mask_)return {};
    const auto mx=std::clamp<std::int64_t>(left+mask_offset_[0],0,mask_->width());
    const auto my=std::clamp<std::int64_t>(top+mask_offset_[1],0,mask_->height());
    const auto mr=std::clamp<std::int64_t>(right+mask_offset_[0],mx,mask_->width());
    const auto mb=std::clamp<std::int64_t>(bottom+mask_offset_[1],my,mask_->height());
    const auto result=mask_->extent({int(mx),int(my),int(mr-mx),int(mb-my)});
    if(result.width<=0||result.height<=0)return {};
    return {int(std::int64_t(result.x)-mask_offset_[0]),int(std::int64_t(result.y)-mask_offset_[1]),result.width,result.height};
}
std::optional<RasterSelection> project_selection(const RegionSelection& selection,
    std::array<int,2> target_origin,int width,int height) noexcept {
    if(!selection.active())return {};
    width=std::max(0,width);height=std::max(0,height);
    const auto dx=std::int64_t(target_origin[0])-selection.origin[0],dy=std::int64_t(target_origin[1])-selection.origin[1];
    const auto x=std::int64_t(selection.bounds.x)-dx,y=std::int64_t(selection.bounds.y)-dy;
    const auto left=std::clamp<std::int64_t>(x,0,width),top=std::clamp<std::int64_t>(y,0,height);
    const auto right=std::clamp<std::int64_t>(x+std::max(0,selection.bounds.width),left,width);
    const auto bottom=std::clamp<std::int64_t>(y+std::max(0,selection.bounds.height),top,height);
    RasterSelection result({int(left),int(top),int(right-left),int(bottom-top)});
    result.mask_=selection.mask;result.mask_offset_={dx,dy};
    result.masked_=selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated
        ||(selection.state==SelectionLayerState::FullPlane&&selection.mask);
    return result;
}
RegionSelection selection_detail::combine_mask_source(const RegionSelection& current,SelectionCombine mode,int page_width,int page_height,Rect request,const BinaryMask& incoming,const SelectionMaskValidator& validate){
    const auto size=selection_carrier_size(current,page_width,page_height);const auto width=size[0],height=size[1];
    if((incoming.width()!=width||incoming.height()!=height)&&mode!=SelectionCombine::Replace&&mode!=SelectionCombine::Add)throw std::invalid_argument("异尺寸输入只支持替换或添加选区。");
    if(mode<SelectionCombine::Replace||mode>SelectionCombine::Intersect)throw std::invalid_argument("选区组合方式无效。");
    const auto clear=[&]{auto result=cleared_selection({},size);result.vector_flags=current.vector_flags;result.carrier_dpi=current.carrier_dpi;return result;};
    const auto left=std::max<std::int64_t>(request.x,-std::int64_t(current.origin[0]));
    const auto top=std::max<std::int64_t>(request.y,-std::int64_t(current.origin[1]));
    const auto right=std::min(std::int64_t(request.x)+std::max(0,request.width),std::int64_t(page_width)-current.origin[0]);
    const auto bottom=std::min(std::int64_t(request.y)+std::max(0,request.height),std::int64_t(page_height)-current.origin[1]);
    if(left>=right||top>=bottom)return mode==SelectionCombine::Replace||mode==SelectionCombine::Intersect?clear():current;
    if(left>std::numeric_limits<int>::max()||top>std::numeric_limits<int>::max())throw std::invalid_argument("选区原点映射超出支持范围。");
    request={int(left),int(top),int(right-left),int(bottom-top)};
    if(mode==SelectionCombine::Add){
        if(current.state==SelectionLayerState::FullPlane)return current;
        if(!current.active())mode=SelectionCombine::Replace;
    }else if(mode==SelectionCombine::Subtract&&!current.active())return current;
    else if(mode==SelectionCombine::Intersect&&(current.state==SelectionLayerState::Cleared||current.state==SelectionLayerState::FullPlane))mode=SelectionCombine::Replace;
    std::optional<BinaryMask> validated;
    if(validate&&mode!=SelectionCombine::Replace){
        validated=incoming;
        if(!validate(request,*validated)){
            if(mode==SelectionCombine::Subtract&&current.state==SelectionLayerState::FullPlane){
                auto materialized=current;auto full=std::make_shared<BinaryMask>(width,height);
                for(int y=0;y<height;++y)for(int x=0;x<width;++x)full->set(x,y);
                materialized.mask=std::move(full);materialized.carrier_size=size;return materialized;
            }
            return current;
        }
    }
    const auto& source=validated?*validated:incoming;
    std::shared_ptr<BinaryMask> output;
    if(mode==SelectionCombine::Replace){
        output=std::make_shared<BinaryMask>(width,height);copy_mask_rectangle(*output,source,request);
    }else{
        output=current.mask?std::make_shared<BinaryMask>(*current.mask):std::make_shared<BinaryMask>(width,height);
        if(current.state==SelectionLayerState::FullPlane||(!current.mask&&current.state==SelectionLayerState::Rectangle)){
            const auto fill=current.state==SelectionLayerState::FullPlane?Rect{0,0,width,height}:clip(current.bounds,width,height);
            for(int y=fill.y;y<fill.y+fill.height;++y)for(int x=fill.x;x<fill.x+fill.width;++x)output->set(x,y);
        }
        // Native CArea preserves all storage outside the operation rectangle.
        // Intersect visits old bounds; Add/Subtract visit the admitted request.
        const auto scan=mode==SelectionCombine::Add?clip(request,std::min(width,source.width()),std::min(height,source.height())):clip(mode==SelectionCombine::Intersect?current.bounds:request,width,height);
        if(!empty(scan)){
            for(int ty=scan.y/256;ty<=(scan.y+scan.height-1)/256;++ty)
                for(int tx=scan.x/256;tx<=(scan.x+scan.width-1)/256;++tx){
                if(mode==SelectionCombine::Add&&source.block_present(tx,ty))output->materialize_block(tx,ty);
            }
            for(int y=scan.y;y<scan.y+scan.height;++y)for(int x=scan.x;x<scan.x+scan.width;++x){
                const bool incoming_bit=source.test(x,y);
                if(mode==SelectionCombine::Add){if(incoming_bit)output->set(x,y);}
                else if(mode==SelectionCombine::Subtract?incoming_bit:!incoming_bit)output->set(x,y,false);
            }
            if(mode!=SelectionCombine::Add)output->optimize_blocks(scan);
        }
    }
    Rect bounds=mode==SelectionCombine::Replace?request:mode==SelectionCombine::Add?unite(current.bounds,request):current.bounds;
    if(validate&&mode==SelectionCombine::Replace&&!validate(bounds,*output))return clear();
    if(mode==SelectionCombine::Subtract||mode==SelectionCombine::Intersect){bounds=output->extent({0,0,width,height});if(empty(bounds))return clear();}
    auto result=current;result.state=SelectionLayerState::Mask;result.bounds=bounds;result.mask=std::move(output);result.carrier_size=size;
    result.vector_cache.reset();result.vector_cache_origin={};return result;
}
RegionSelection combine_selection_mask(const RegionSelection& current,SelectionCombine mode,int page_width,int page_height,Rect request,const BinaryMask& incoming,const SelectionMaskValidator& validate){
    const auto size=selection_carrier_size(current,page_width,page_height);
    if(incoming.width()!=size[0]||incoming.height()!=size[1])throw std::invalid_argument("输入选区遮罩与存储尺寸不一致。");
    return selection_detail::combine_mask_source(current,mode,page_width,page_height,request,incoming,validate);
}
}
