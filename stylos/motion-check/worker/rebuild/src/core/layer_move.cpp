#include "raster_layer_expansion.hpp"
#include "retas/numeric.hpp"
#include "vector_document_geometry.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <set>
#include <type_traits>

namespace retas {
namespace {
struct Edges {int left{},top{},right{},bottom{};};
int integer(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
        throw std::invalid_argument("图层移动坐标超出支持范围。");
    return int(value);
}
int scaled(int value,double from,double to){
    if(!std::isfinite(from)||from<=0||!std::isfinite(to)||to<=0)
        throw std::invalid_argument("图层或平面的分辨率无效。");
    if(from==to)return value;
    const auto result=numeric_detail::binary64(double(value)*numeric_detail::binary64(to/from));
    if(!std::isfinite(result)||result<std::numeric_limits<int>::min()||result>std::numeric_limits<int>::max())
        throw std::invalid_argument("图层移动的分辨率换算超出支持范围。");
    return int(result); // sub_61F770 / sub_61F920: __ftol2, not rtDouble2Int.
}
std::optional<Edges> expansion(const CommonElement& layer,int dx,int dy,int page_width,int page_height,bool allow_empty=false){
    if(layer.width<0||layer.height<0||(!allow_empty&&(!layer.width||!layer.height)))throw std::invalid_argument("图层尺寸无效。");
    const auto left=std::int64_t(layer.x)+dx,top=std::int64_t(layer.y)+dy;
    const auto right=left+layer.width,bottom=top+layer.height;
    for(const auto edge:{left,top,right,bottom})(void)integer(edge);
    if(left<=0&&top<=0&&right>=page_width&&bottom>=page_height)return {};
    // sub_61FD70 initializes I to zero. RCRect::IntersectRect leaves that
    // destination untouched when rectangles are disjoint or only touch.
    std::int64_t il=std::max<std::int64_t>(0,left),it=std::max<std::int64_t>(0,top);
    std::int64_t ir=std::min<std::int64_t>(page_width,right),ib=std::min<std::int64_t>(page_height,bottom);
    if(il>=ir||it>=ib)il=it=ir=ib=0;
    const auto bump=[](std::int64_t value){return value>0?value+1:value;};
    return Edges{integer(-bump(il)),integer(-bump(it)),
        integer(std::int64_t(layer.width)+bump(page_width-ir)),integer(std::int64_t(layer.height)+bump(page_height-ib))};
}
struct ExpansionBudget {
    std::uint64_t& bytes;std::uint64_t& work;std::stop_token stop;
    void charge(std::uint64_t memory,std::uint64_t visits){
        if(stop.stop_requested())throw std::runtime_error("Raster expansion cancelled");
        if(memory>512ull*1024*1024-bytes||visits>128ull*1024*1024-work)throw std::length_error("Raster expansion exceeds aggregate budget");
        bytes+=memory;work+=visits;
    }
};
struct Expansion {int x{},y{};std::optional<Raster> raster;};
Expansion allocate_rect(const Raster& raster,Edges extent,bool sparse_detail=false,ExpansionBudget* budget=nullptr){
    // RCVOffscreen::AllocateRect expands descriptor storage in whole tiles on
    // left/top only. Right/bottom logical dimensions are not tile-rounded.
    const auto pad=[](int edge){return edge<0?integer(((-std::int64_t(edge)+255)/256)*256):0;};
    const int xpad=pad(extent.left),ypad=pad(extent.top);
    const int width=integer(std::int64_t(xpad)+std::max(raster.width,extent.right));
    const int height=integer(std::int64_t(ypad)+std::max(raster.height,extent.bottom));
    if(width==raster.width&&height==raster.height)return {};
    //The8xDetailLine is a sparse native descriptor, not a dense working image.
    //Its logical pixel count may exceed ordinary raster execution budgets.
    if(sparse_detail&&(raster.depth!=1||raster.tiles.size()>512ull*1024*1024/raster.tile_bytes()))throw std::length_error("Temporary DetailLine resident storage limit");
    Raster resized(sparse_detail?1:width,sparse_detail?1:height,raster.depth,raster.resolution);
    if(sparse_detail){resized.width=width;resized.height=height;}resized.color_order=raster.color_order;
    for(const auto& [coordinate,bytes]:raster.tiles){
        if(budget)budget->charge(128,1);
        if(coordinate.x<0||coordinate.y<0||std::int64_t(coordinate.x)*256>=raster.width||std::int64_t(coordinate.y)*256>=raster.height
            ||!bytes||bytes->size()!=raster.tile_bytes())throw std::invalid_argument("图层栅格块无效。");
        // Preserve the entire backing, including latent bytes past the old
        // logical right/bottom edge. New slots remain sparse, not white tiles.
        resized.tiles.emplace(TileCoord{coordinate.x+xpad/256,coordinate.y+ypad/256},bytes);
    }
    return {-xpad,-ypad,std::move(resized)};
}
struct LayerChange {
    int x{},y{},width{},height{};
    std::vector<std::pair<std::size_t,Raster>> planes;
    std::optional<Bytes> specific;
    std::optional<Rect> vector_extent;
    bool changed{};
};
LayerChange prepare_layer_change(const Layer& layer,int dx,int dy,const std::optional<Edges>& bounds,bool allow_mixed_vector=false,ExpansionBudget* budget=nullptr,bool allow_text=false){
    const bool vector=vector_geometry_detail::owner(layer.family);
    if(layer.opaque||(!vector&&(layer.family&0xf0000000)!=0x10000000&&!(allow_text&&(layer.family==0x40000800||layer.family==0x48040800||layer.family==0x40000200))))
        throw std::invalid_argument("此图层类型的移动与扩展尚未接入。");
    LayerChange result;result.width=layer.common.width;result.height=layer.common.height;
    int correction_x{},correction_y{};
    if(vector&&bounds&&layer.runtime_vector_cache_present){
        std::uint64_t local_allocation{},local_work{};auto& allocation=budget?budget->bytes:local_allocation;auto& work=budget?budget->work:local_work;const auto stop=budget?budget->stop:std::stop_token{};auto candidate=layer;
        if(layer.family==0x20000200||allow_mixed_vector)std::erase_if(candidate.planes,[](const Plane& p){return (p.family&0xf0000000u)==0x10000000u;});
        const auto pad=[](int v){return v<0?integer(((-std::int64_t(v)+255)/256)*256):0;};
        const auto xpad=pad(bounds->left),ypad=pad(bounds->top);
        result.width=integer(std::int64_t(xpad)+std::max(layer.common.width,bounds->right));
        result.height=integer(std::int64_t(ypad)+std::max(layer.common.height,bounds->bottom));
        correction_x=-xpad;correction_y=-ypad;
        const auto old_extent=layer.runtime_vector_cache_extent;
        result.vector_extent=old_extent.width>0&&old_extent.height>0?Rect{integer(std::int64_t(old_extent.x)+xpad),integer(std::int64_t(old_extent.y)+ypad),old_extent.width,old_extent.height}:Rect{};
        vector_geometry_detail::translate_cache(candidate,integer(std::int64_t(xpad)*16),integer(std::int64_t(ypad)*16),allocation,work,stop);
        if(xpad||ypad)result.specific=std::move(candidate.specific);
        //630270 passes the expanded cache rectangle plus its correction to
        //each ordinary child. Child corrections do not change the root/cache.
        const Edges expanded{-xpad,-ypad,integer(std::int64_t(result.width)-xpad),integer(std::int64_t(result.height)-ypad)};
        for(std::size_t index=0;index<layer.planes.size();++index){
            const auto& plane=layer.planes[index];if((plane.family&0xf0000000u)!=0x10000000u)continue;
            if(plane.opaque||!plane.raster)throw std::invalid_argument("Temporary vector owner has an undecoded raster child");
            const auto& raster=*plane.raster;
            if(raster.width!=plane.common.width||raster.height!=plane.common.height||raster.depth!=plane.common.depth||raster.resolution!=plane.common.resolution)throw std::invalid_argument("Temporary raster child metadata differs from its offscreen");
            auto child=allocate_rect(raster,{scaled(expanded.left,layer.common.resolution,plane.common.resolution),scaled(expanded.top,layer.common.resolution,plane.common.resolution),scaled(expanded.right,layer.common.resolution,plane.common.resolution),scaled(expanded.bottom,layer.common.resolution,plane.common.resolution)},layer.family==0x20000200&&plane.family==0x11240001);
            if(child.raster)result.planes.emplace_back(index,std::move(*child.raster));
        }
    }else if(bounds&&!vector){
        for(std::size_t index=0;index<layer.planes.size();++index){
            if(budget)budget->charge(0,1);
            const auto& plane=layer.planes[index];
            if(!(plane.family&0x10000000))continue;
            if(plane.opaque||!plane.raster)throw std::invalid_argument("图层包含无法扩展的栅格平面。");
            const auto& raster=*plane.raster;
            if(raster.width!=plane.common.width||raster.height!=plane.common.height||raster.depth!=plane.common.depth
                ||raster.resolution!=plane.common.resolution)throw std::invalid_argument("平面与栅格的尺寸或分辨率不一致。");
            const Edges plane_bounds{scaled(bounds->left,layer.common.resolution,plane.common.resolution),
                scaled(bounds->top,layer.common.resolution,plane.common.resolution),
                scaled(bounds->right,layer.common.resolution,plane.common.resolution),
                scaled(bounds->bottom,layer.common.resolution,plane.common.resolution)};
            auto correction=allocate_rect(raster,plane_bounds,false,budget);
            correction_x=scaled(correction.x,plane.common.resolution,layer.common.resolution);
            correction_y=scaled(correction.y,plane.common.resolution,layer.common.resolution);
            // Native sub_61FFF0 deliberately takes the last raster child.
            const int divisor=(plane.family&0x00200000)?8:1;
            result.width=(correction.raster?correction.raster->width:raster.width)/divisor;
            result.height=(correction.raster?correction.raster->height:raster.height)/divisor;
            if(result.width<=0||result.height<=0)throw std::invalid_argument("扩展后的图层尺寸无效。");
            if(correction.raster){if(budget)budget->charge(2*sizeof(std::pair<std::size_t,Raster>),0);result.planes.emplace_back(index,std::move(*correction.raster));}
        }
    }
    result.x=integer(std::int64_t(layer.common.x)+dx+correction_x);
    result.y=integer(std::int64_t(layer.common.y)+dy+correction_y);
    (void)integer(std::int64_t(result.x)+result.width);
    (void)integer(std::int64_t(result.y)+result.height);
    // Ordinary LYMV never writes CommonElement::position or plane origins.
    result.changed=dx||dy||!result.planes.empty()||result.width!=layer.common.width||result.height!=layer.common.height;
    return result;
}
LayerChange translate_layer(const Layer& layer,int dx,int dy,int page_width,int page_height){
    return prepare_layer_change(layer,dx,dy,expansion(layer.common,dx,dy,page_width,page_height));
}
void apply_layer_change(Layer& layer,LayerChange&& change){
    layer.common.x=change.x;layer.common.y=change.y;
    layer.common.width=change.width;layer.common.height=change.height;
    if(change.specific)layer.specific=std::move(*change.specific);
    if(change.vector_extent)layer.runtime_vector_cache_extent=*change.vector_extent;
    for(auto& [plane_index,raster]:change.planes){
        auto& plane=layer.planes[plane_index];plane.common.width=raster.width;plane.common.height=raster.height;
        *plane.raster=std::move(raster);plane.pixels_changed=true;
    }
}
}
bool expand_raster_layer(Layer& layer,Rect bounds,bool allow_mixed_vector){
    if(bounds.width<=0||bounds.height<=0)throw std::invalid_argument("图层扩展范围无效。");
    const Edges edges{bounds.x,bounds.y,integer(std::int64_t(bounds.x)+bounds.width),integer(std::int64_t(bounds.y)+bounds.height)};
    auto change=prepare_layer_change(layer,0,0,edges,allow_mixed_vector);const bool changed=change.changed;
    apply_layer_change(layer,std::move(change));return changed;
}
namespace layer_move_detail {
void move_geometry_layer(Layer& layer,int dx,int dy,int page_width,int page_height,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    ExpansionBudget budget{bytes,work,stop};budget.charge(sizeof(LayerChange),1);
    auto change=prepare_layer_change(layer,dx,dy,expansion(layer.common,dx,dy,page_width,page_height,true),false,&budget,true);
    budget.charge(0,1);apply_layer_change(layer,std::move(change));
}
bool expand_raster_layer(Layer& layer,Rect bounds,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    if(bounds.width<=0||bounds.height<=0)throw std::invalid_argument("Invalid raster expansion bounds");
    if((layer.family&0xf0000000u)!=0x10000000u)throw std::invalid_argument("Bounded expansion requires a raster owner");
    ExpansionBudget budget{bytes,work,stop};budget.charge(sizeof(LayerChange),1);
    const Edges edges{bounds.x,bounds.y,integer(std::int64_t(bounds.x)+bounds.width),integer(std::int64_t(bounds.y)+bounds.height)};
    auto change=prepare_layer_change(layer,0,0,edges,false,&budget);const bool changed=change.changed;
    budget.charge(0,1);apply_layer_change(layer,std::move(change));return changed;
}
}
bool layer_move_eligible(const Layer& layer){return !layer.opaque&&layer.common.flags[1]&&layer.common.flags[0]==0;}
std::vector<std::size_t> selected_move_layers(const DocumentData& document){
    std::vector<std::size_t> result;
    for(std::size_t i=0;i<document.layers.size();++i)if(document.layers[i].common.flags[2])result.push_back(i);
    return result;
}
static auto prepare_moves(const DocumentData& document,std::span<const std::size_t> selected,int dx,int dy){
    static_assert(std::is_nothrow_move_assignable_v<Raster>);
    std::set<std::size_t> unique;
    std::vector<std::pair<std::size_t,LayerChange>> candidates;candidates.reserve(selected.size());
    const int width=document.base.width(),height=document.base.height();
    for(const auto index:selected){
        if(index>=document.layers.size()||!unique.insert(index).second)throw std::invalid_argument("移动目标列表无效。");
        const auto& layer=document.layers[index];
        if(!layer_move_eligible(layer))continue;
        auto candidate=translate_layer(layer,dx,dy,width,height);
        if(candidate.changed)candidates.emplace_back(index,std::move(candidate));
    }
    return candidates;
}
bool move_raster_layers(DocumentData& document,std::span<const std::size_t> selected,int dx,int dy){
    auto candidates=prepare_moves(document,selected,dx,dy);
    // Publish only geometry and changed descriptor maps. In particular, do
    // not copy large immutable native payloads on every pointer increment.
    for(auto& [index,change]:candidates){
        apply_layer_change(document.layers[index],std::move(change));
    }
    return !candidates.empty();
}
bool move_layer_targets(Document& document,std::span<const std::size_t> selected,int dx,int dy){
    auto candidates=prepare_moves(document.data,selected,dx,dy);
    const auto active=document.data.current_layer;
    const bool current=active>=0&&active<int(document.data.layers.size())&&std::ranges::find(selected,std::size_t(active))!=selected.end()&&layer_move_eligible(document.data.layers[std::size_t(active)]);
    bool attached_changed{};
    std::shared_ptr<const AttachedTemporaryLayers> prepared;
    if(current&&document.attached_temporary_layers()){
        auto pair=std::make_shared<AttachedTemporaryLayers>(*document.attached_temporary_layers());
        const int w=document.data.base.width(),h=document.data.base.height();
        std::optional<LayerChange> vector,raster;
        if(pair->vector){if(pair->vector->family!=0x20000200)throw std::invalid_argument("Attached Temp vector has an unexpected family");vector=translate_layer(*pair->vector,dx,dy,w,h);}
        if(pair->raster){if(pair->raster->family!=0x10020000)throw std::invalid_argument("Attached Temp raster has an unexpected family");raster=translate_layer(*pair->raster,dx,dy,w,h);}
        attached_changed=(vector&&vector->changed)||(raster&&raster->changed);
        if(vector)apply_layer_change(*pair->vector,std::move(*vector));if(raster)apply_layer_change(*pair->raster,std::move(*raster));if(attached_changed)prepared=std::move(pair);
    }
    for(auto& [index,change]:candidates)apply_layer_change(document.data.layers[index],std::move(change));
    if(prepared)document.publish_attached_temporary_layers(std::move(prepared));
    return !candidates.empty()||attached_changed;
}

}
