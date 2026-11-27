#pragma once
#include "retas/document.hpp"
#include "retas/floating.hpp"
#include "retas/layer_move.hpp"
#include "raster_layer_expansion.hpp"
#include "modal_document_guard.hpp"
#include "selection_raster.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace retas::raster_history_detail {
constexpr std::uint64_t storage_limit=512ull*1024*1024,work_limit=128ull*1024*1024;
struct Budget {
    std::uint64_t bytes{},work{};std::stop_token stop;
    void poll()const{modal_detail::poll(stop);}
    void allocate(std::uint64_t n){poll();modal_detail::charge(bytes,n,storage_limit,"Raster history exceeds512MiB");}
    void visit(std::uint64_t n){poll();modal_detail::charge(work,n,work_limit,"Raster history exceeds128Mi work");}
};
inline Rect clipped(Rect roi,const Raster& image){
    const auto left=std::clamp<std::int64_t>(roi.x,0,image.width),top=std::clamp<std::int64_t>(roi.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(roi.x)+std::max(0,roi.width),left,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(roi.y)+std::max(0,roi.height),top,image.height);
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
inline void validate(const Raster& image){
    if(image.width<=0||image.height<=0||image.width>100000||image.height>100000||std::int64_t(image.width)*image.height>268435456)
        throw std::invalid_argument("Invalid raster history dimensions");
    for(const auto& [tile,bytes]:image.tiles)if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=image.width||std::int64_t(tile.y)*256>=image.height||!bytes||bytes->size()!=image.tile_bytes())
        throw std::invalid_argument("Invalid raster history backing");
}
template<class Ledger> inline Raster copy_all(const Raster& source,Ledger& budget){
    validate(source);budget.allocate(sizeof(Raster)+source.tiles.size()*128);auto result=source;
    for(auto& [tile,bytes]:result.tiles){(void)tile;budget.visit(1);budget.allocate(bytes->size());bytes=std::make_shared<Bytes>(*bytes);}return result;
}
// Same-coordinate, same-depth SDK Copy(flag1), used twice by native history:
// bind/lazy opposite -> full-sized private offscreen, then replay -> live ROI.
template<class Ledger> inline void copy_roi(const Raster& source,Raster& target,Rect roi,Ledger& budget){
    validate(source);validate(target);
    const auto canonical=[](const Raster& r){return (r.depth==1||r.depth==2||r.depth==8||r.depth==16||r.depth==24||r.depth==32||r.depth==64)&&r.color_order==(r.depth==24?4:r.depth>=32?3:0);};
    if(!canonical(source)||!canonical(target)||source.depth!=target.depth)throw std::invalid_argument("Raster ROI history requires matching canonical depths");
    const bool whole=roi==Rect{0,0,source.width,source.height}&&source.width==target.width&&source.height==target.height;
    roi=clipped(clipped(roi,source),target);if(roi.width<=0||roi.height<=0)return;
    if(whole){auto copied=copy_all(source,budget);target.tiles=std::move(copied.tiles);return;}
    const auto writable=[&](TileCoord tile){
        const auto found=target.tiles.find(tile);
        if(found==target.tiles.end()||found->second.use_count()!=1){budget.allocate(target.tile_bytes()+128);selection_detail::store_raw_pixel(target,tile.x*256,tile.y*256,selection_detail::raw_pixel(target,tile.x*256,tile.y*256));}
    };
    const auto terminal=[&](int x,int y){if(target.contains(x,y))writable({x/256,y/256});};
    std::vector<TileCoord> erase;
    const int right=roi.x+roi.width,bottom=roi.y+roi.height;
    budget.visit(std::uint64_t(roi.width)*roi.height);
    for(int ty=roi.y/256;ty<=(bottom-1)/256;++ty)for(int tx=roi.x/256;tx<=(right-1)/256;++tx){
        budget.poll();const TileCoord tile{tx,ty};const int left=std::max(roi.x,tx*256),top=std::max(roi.y,ty*256),r=std::min(right,(tx+1)*256),b=std::min(bottom,(ty+1)*256);
        const auto found=source.tiles.find(tile);const bool present=found!=source.tiles.end();
        if((source.depth<=8||source.depth==32)&&r-left==256&&b-top==256){
            if(present){budget.allocate(source.tile_bytes()+128);target.tiles[tile]=std::make_shared<Bytes>(*found->second);}
            else{budget.allocate(sizeof(TileCoord));erase.push_back(tile);}continue;
        }
        writable(tile);
        for(int y=top;y<b;++y){budget.poll();for(int x=left;x<r;++x)selection_detail::store_raw_pixel(target,x,y,present?selection_detail::raw_pixel(source,x,y):selection_detail::clear_word(source.depth));
            // Copy64's missing-block setter advances NextX even after the
            // last sample. Its present-block loop has no terminal advance.
            if(source.depth==64&&!present)terminal(r,y);
        }
        // Copy1/2/32 partial-width loops advance NextY after the last row.
        // Copy8 and CommonCopy16/24 have no cross-block terminal cursor.
        if((source.depth<=2||source.depth==32)&&r-left<256)terminal(left,b);
    }
    for(auto tile:erase)target.tiles.erase(tile);
}
inline Raster snapshot(const Raster& source,Rect roi,Budget& budget){
    budget.allocate(sizeof(Raster));Raster result(source.width,source.height,source.depth,source.resolution);result.color_order=source.color_order;
    copy_roi(source,result,roi,budget);return result;
}
struct Target {
    std::uint64_t layer{},plane{};std::size_t layer_hint{},plane_hint{};std::uint32_t layer_family{},plane_family{};
};
inline Target target(const DocumentData& data,std::size_t layer,std::size_t plane){const auto& l=data.layers.at(layer);const auto& p=l.planes.at(plane);return {l.common.runtime_identity,p.common.runtime_identity,layer,plane,l.family,p.family};}
inline bool same_identity_layout(const DocumentData& a,const DocumentData& b){
    if(a.layers.size()!=b.layers.size())return false;for(std::size_t l=0;l<a.layers.size();++l){if(a.layers[l].common.runtime_identity!=b.layers[l].common.runtime_identity||a.layers[l].planes.size()!=b.layers[l].planes.size())return false;for(std::size_t p=0;p<a.layers[l].planes.size();++p)if(a.layers[l].planes[p].common.runtime_identity!=b.layers[l].planes[p].common.runtime_identity)return false;}return true;
}
inline std::size_t owner_index(const DocumentData& data,const Target& target){
    std::optional<std::size_t> index;for(std::size_t i=0;i<data.layers.size();++i)if(data.layers[i].common.runtime_identity==target.layer&&data.layers[i].family==target.layer_family){if(index)throw std::logic_error("Ambiguous raster history owner");index=i;}
    if(!index)throw std::logic_error("Raster history owner no longer exists");return *index;
}
inline Plane& resolve(DocumentData& data,const Target& target){
    auto& owner=data.layers[owner_index(data,target)];
    Plane* result{};for(auto& p:owner.planes)if(p.common.runtime_identity==target.plane&&p.family==target.plane_family){if(result)throw std::logic_error("Ambiguous raster history plane");result=&p;}
    if(!result)throw std::logic_error("Raster history plane no longer exists");return *result;
}
// Build once for a live receiver whose order can differ from admission. An
// unchanged receiver may use its admitted slots even if manually copied
// elements already shared an identity; stale receivers require unique keys.
class Resolver {
    using Key=std::pair<std::uint64_t,std::uint32_t>;
    using Index=std::map<Key,std::size_t>;
    static constexpr auto ambiguous=std::numeric_limits<std::size_t>::max();
    DocumentData& data_;
    Index owners_;
    std::vector<Index> planes_;
    bool hints_;
    static void add(Index& index,Key key,std::size_t value){const auto [at,inserted]=index.emplace(key,value);if(!inserted)at->second=ambiguous;}
    static std::size_t lookup(const Index& index,Key key){const auto at=index.find(key);if(at==index.end())throw std::logic_error("Raster history target no longer exists");if(at->second==ambiguous)throw std::logic_error("Ambiguous raster history target");return at->second;}
public:
    Resolver(DocumentData& data,Budget& budget,bool admitted_hints):data_(data),hints_(admitted_hints){
        std::uint64_t count=data.layers.size();for(const auto& owner:data.layers)count+=owner.planes.size();
        budget.visit(count);budget.allocate(count*128+data.layers.size()*sizeof(Index));planes_.resize(data.layers.size());
        for(std::size_t l=0;l<data.layers.size();++l){budget.poll();const auto& owner=data.layers[l];add(owners_,{owner.common.runtime_identity,owner.family},l);for(std::size_t p=0;p<owner.planes.size();++p){const auto& plane=owner.planes[p];add(planes_[l],{plane.common.runtime_identity,plane.family},p);}}
    }
    std::size_t owner(const Target& target)const{
        if(hints_&&target.layer_hint<data_.layers.size()){const auto& layer=data_.layers[target.layer_hint];if(layer.common.runtime_identity==target.layer&&layer.family==target.layer_family)return target.layer_hint;}
        return lookup(owners_,{target.layer,target.layer_family});
    }
    std::size_t plane(const Target& target)const{
        const auto layer=owner(target);const auto& planes=data_.layers[layer].planes;
        if(hints_&&target.plane_hint<planes.size()&&planes[target.plane_hint].common.runtime_identity==target.plane&&planes[target.plane_hint].family==target.plane_family)return target.plane_hint;
        return lookup(planes_[layer],{target.plane,target.plane_family});
    }
    Plane& get(const Target& target)const{return data_.layers[owner(target)].planes[plane(target)];}
};
inline Rect capture_roi(const Layer& owner,const Plane& plane,const RegionSelection& selection){
    const auto bounds=selection.document_bounds();const auto x=std::int64_t(bounds.x)-owner.common.x,y=std::int64_t(bounds.y)-owner.common.y;
    const double ratio=plane.common.resolution/owner.common.resolution;
    const auto map=[&](std::int64_t coordinate){const double value=double(coordinate)*ratio;
        if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Raster capture history DPI mapping overflow");return std::int64_t(value);};
    const auto left=map(x),top=map(y),right=map(x+std::max(0,bounds.width)),bottom=map(y+std::max(0,bounds.height));
    const auto l=std::clamp<std::int64_t>(left,0,plane.raster->width),t=std::clamp<std::int64_t>(top,0,plane.raster->height);
    const auto r=std::clamp<std::int64_t>(right,l,plane.raster->width),b=std::clamp<std::int64_t>(bottom,t,plane.raster->height);
    return {int(l),int(t),int(r-l),int(b-t)};
}
struct Edit {
    struct PlaneState {Target target;Rect roi;Raster primary;std::optional<Raster> secondary;};
    struct Geometry {int x{},y{},width{},height{};};
    std::vector<PlaneState> planes;
    std::shared_ptr<const std::vector<std::uint64_t>> layout;
    std::optional<Geometry> primary_geometry,secondary_geometry;
    bool reverse{},native_raster_dirty{};
    static std::shared_ptr<const std::vector<std::uint64_t>> identity_layout(const DocumentData& data,Budget& budget){
        std::uint64_t count=1;for(const auto& owner:data.layers)count+=3+owner.planes.size()*2;
        budget.allocate(sizeof(std::vector<std::uint64_t>)+count*sizeof(std::uint64_t));budget.visit(count);
        auto result=std::make_shared<std::vector<std::uint64_t>>();result->reserve(count);result->push_back(data.layers.size());
        for(const auto& owner:data.layers){budget.poll();result->push_back(owner.common.runtime_identity);result->push_back(owner.family);result->push_back(owner.planes.size());for(const auto& plane:owner.planes){result->push_back(plane.common.runtime_identity);result->push_back(plane.family);}}
        return result;
    }
    bool layout_matches(const DocumentData& data)const{
        if(!layout)return false;std::size_t index{};const auto next=[&](std::uint64_t value){return index<layout->size()&&(*layout)[index++]==value;};
        if(!next(data.layers.size()))return false;for(const auto& owner:data.layers){if(!next(owner.common.runtime_identity)||!next(owner.family)||!next(owner.planes.size()))return false;for(const auto& plane:owner.planes)if(!next(plane.common.runtime_identity)||!next(plane.family))return false;}return index==layout->size();
    }
    static std::shared_ptr<const Edit> capture(const DocumentData& data,const FloatingSelection& floating,const RegionSelection& selection,Budget& budget){
        auto edit=std::make_shared<Edit>();budget.allocate(sizeof(Edit));edit->layout=identity_layout(data,budget);
        for(const auto& source:*floating.sources){const auto& owner=data.layers.at(source.layer);for(auto index:source.planes){const auto& plane=owner.planes.at(index);const auto roi=capture_roi(owner,plane,selection);if(roi.width<=0||roi.height<=0)continue;
            budget.allocate(sizeof(PlaneState));edit->planes.push_back({target(data,source.layer,index),roi,snapshot(*plane.raster,roi,budget),{}});}}
        return edit;
    }
    static std::vector<std::shared_ptr<const Edit>> paste(const DocumentData& data,const FloatingSelection& floating,Budget& budget){
        const auto integer=[](std::int64_t n){if(n<std::numeric_limits<int>::min()||n>std::numeric_limits<int>::max())throw std::invalid_argument("Raster history coordinate overflow");return int(n);};
        const auto extent=[&](const Raster& raster){validate(raster);Rect out;bool found{};for(const auto& [tile,bytes]:raster.tiles){budget.visit(1);(void)bytes;const int l=tile.x*256,t=tile.y*256,r=std::min(l+256,raster.width),b=std::min(t+256,raster.height);if(!found){out={l,t,r-l,b-t};found=true;}else{const int right=std::max(out.x+out.width,r),bottom=std::max(out.y+out.height,b);out.x=std::min(out.x,l);out.y=std::min(out.y,t);out.width=right-out.x;out.height=bottom-out.y;}}return out;};
        budget.allocate(floating.layers->size()*4*sizeof(std::shared_ptr<const Edit>));
        std::vector<std::shared_ptr<const Edit>> result;result.reserve(floating.layers->size()*2);const auto layout=identity_layout(data,budget);
        for(std::size_t i=0;i<floating.layers->size();++i){const auto& source=(*floating.layers)[i];const auto& mapping=(*floating.sources)[i];const auto& live=data.layers.at(mapping.layer);
            std::optional<Rect> bounds;for(const auto& p:source.planes){const auto roi=extent(*p.raster);if(roi.width<=0||roi.height<=0)continue;if(!bounds)bounds=roi;else{const int right=std::max(bounds->x+bounds->width,roi.x+roi.width),bottom=std::max(bounds->y+bounds->height,roi.y+roi.height);bounds->x=std::min(bounds->x,roi.x);bounds->y=std::min(bounds->y,roi.y);bounds->width=right-bounds->x;bounds->height=bottom-bounds->y;}}
            if(!bounds)continue;
            std::uint64_t metadata=sizeof(Layer)+live.common.name.size()+live.common.suffix.size()+live.specific.size()+live.suffix.size()+live.plane_suffix.size()+live.opaque_record.size();
            for(const auto& p:live.planes)metadata+=sizeof(Plane)+p.common.name.size()+p.common.suffix.size()+p.specific.size()+p.suffix.size()+p.opaque_record.size()+adjust_render_detail::cache_storage(p)+(p.raster?p.raster->tiles.size()*128:0);
            budget.allocate(metadata);auto owner=live;
            const auto dx=std::int64_t(source.common.x)+floating.delta[0]-owner.common.x,dy=std::int64_t(source.common.y)+floating.delta[1]-owner.common.y;
            const auto left=std::int64_t(bounds->x)+dx,top=std::int64_t(bounds->y)+dy,right=left+bounds->width,bottom=top+bounds->height;
            if(left<0||top<0||right>=owner.common.width||bottom>=owner.common.height){
                budget.allocate(sizeof(Edit));auto size=std::make_shared<Edit>();size->layout=layout;size->primary_geometry=Geometry{owner.common.x,owner.common.y,owner.common.width,owner.common.height};
                for(std::size_t p=0;p<owner.planes.size();++p)if(owner.planes[p].family&0x10000000u){if(!owner.planes[p].raster||owner.planes[p].opaque)throw std::invalid_argument("Raster size history requires decoded children");budget.allocate(sizeof(PlaneState));size->planes.push_back({target(data,mapping.layer,p),{},copy_all(*owner.planes[p].raster,budget),{}});}
                if(!size->planes.empty())result.push_back(std::move(size));
                const auto x=std::min<std::int64_t>(0,left),y=std::min<std::int64_t>(0,top);
                layer_move_detail::expand_raster_layer(owner,{integer(x),integer(y),integer(std::max<std::int64_t>(owner.common.width,right)-x),integer(std::max<std::int64_t>(owner.common.height,bottom)-y)},budget.bytes,budget.work,budget.stop);
            }
            budget.allocate(sizeof(Edit));auto pixels=std::make_shared<Edit>();pixels->layout=layout;
            for(std::size_t p=0;p<source.planes.size();++p){auto roi=extent(*source.planes[p].raster);if(roi.width<=0||roi.height<=0)continue;
                roi.x=integer(std::int64_t(roi.x)+source.common.x+floating.delta[0]-owner.common.x);roi.y=integer(std::int64_t(roi.y)+source.common.y+floating.delta[1]-owner.common.y);
                const auto index=mapping.planes.at(p);const auto& plane=owner.planes.at(index);roi=clipped(roi,*plane.raster);if(roi.width<=0||roi.height<=0)continue;
                budget.allocate(sizeof(PlaneState));pixels->planes.push_back({target(data,mapping.layer,index),roi,snapshot(*plane.raster,roi,budget),{}});
            }
            if(!pixels->planes.empty())result.push_back(std::move(pixels));
        }
        return result;
    }
    std::shared_ptr<const Edit> replay(DocumentData& data,Budget& budget)const{
        Resolver targets(data,budget,layout_matches(data));
        budget.allocate(sizeof(Edit)+planes.size()*sizeof(PlaneState));auto opposite=std::make_shared<Edit>(*this);
        for(const auto& item:planes)budget.allocate(item.primary.tiles.size()*128+(item.secondary?item.secondary->tiles.size()*128:0));
        if(primary_geometry){
            if(planes.empty())throw std::logic_error("Raster geometry history has no planes");
            auto& owner=data.layers[targets.owner(planes.front().target)];
            if(!opposite->secondary_geometry)opposite->secondary_geometry=Geometry{owner.common.x,owner.common.y,owner.common.width,owner.common.height};
            for(auto& item:opposite->planes){auto& plane=targets.get(item.target);if(!plane.raster||plane.opaque)throw std::logic_error("Raster geometry target changed");if(!item.secondary)item.secondary=copy_all(*plane.raster,budget);
                auto restored=copy_all(reverse?*item.secondary:item.primary,budget);plane.common.width=restored.width;plane.common.height=restored.height;plane.common.depth=restored.depth;plane.common.resolution=restored.resolution;plane.raster=std::move(restored);plane.pixels_changed=true;if(native_raster_dirty)plane.raster_modified=true;}
            const auto geometry=reverse?*opposite->secondary_geometry:*primary_geometry;owner.common.x=geometry.x;owner.common.y=geometry.y;owner.common.width=geometry.width;owner.common.height=geometry.height;
        }else for(auto& item:opposite->planes){auto& plane=targets.get(item.target);if(!plane.raster||plane.opaque)throw std::logic_error("Raster history target changed");
            if(!item.secondary)item.secondary=snapshot(*plane.raster,item.roi,budget);
            copy_roi(reverse?*item.secondary:item.primary,*plane.raster,item.roi,budget);plane.pixels_changed=true;if(native_raster_dirty)plane.raster_modified=true;
        }
        opposite->reverse=!reverse;return opposite;
    }
};
struct FloatingBinding {
    std::shared_ptr<const std::vector<std::uint64_t>> layout;
    std::vector<std::vector<Target>> sources;
    static std::shared_ptr<const FloatingBinding> capture(const DocumentData& data,const FloatingSelection& floating,Budget& budget){
        budget.allocate(sizeof(FloatingBinding)+floating.sources->size()*sizeof(std::vector<Target>));auto result=std::make_shared<FloatingBinding>();result->layout=Edit::identity_layout(data,budget);result->sources.reserve(floating.sources->size());
        for(const auto& source:*floating.sources){budget.visit(1);budget.allocate(source.planes.size()*sizeof(Target));auto& targets=result->sources.emplace_back();targets.reserve(source.planes.size());for(auto plane:source.planes)targets.push_back(target(data,source.layer,plane));}return result;
    }
    std::shared_ptr<FloatingSelection> restore(DocumentData& data,const FloatingSelection& floating,Budget& budget)const{
        Edit identity;identity.layout=layout;Resolver targets(data,budget,identity.layout_matches(data));
        if(!floating.sources||floating.sources->size()!=sources.size())throw std::logic_error("Floating history source binding changed");
        budget.allocate(sizeof(FloatingSelection)+sources.size()*sizeof(FloatingSource));auto restored=std::make_shared<FloatingSelection>(floating);auto mappings=std::make_shared<std::vector<FloatingSource>>(*floating.sources);
        for(std::size_t i=0;i<sources.size();++i){auto& mapping=(*mappings)[i];if(mapping.planes.size()!=sources[i].size())throw std::logic_error("Floating history plane binding changed");budget.allocate(mapping.planes.size()*sizeof(std::size_t));
            for(std::size_t p=0;p<sources[i].size();++p){budget.visit(1);mapping.layer=targets.owner(sources[i][p]);mapping.planes[p]=targets.plane(sources[i][p]);}}
        restored->sources=std::move(mappings);return restored;
    }
};
}
