#include "filter_document.hpp"
#include "filter_table_selection.hpp"
#include "modal_document_guard.hpp"
#include "selection_raster.hpp"
#include <algorithm>

namespace retas::filter_detail {
namespace {
struct Kernel {std::array<int,9> weights;int divisor;};
Kernel kernel(FilterKind kind){
    switch(kind){
    case FilterKind::SharpSoft:return {{1,-12,1,-12,144,-12,1,-12,1},100};
    case FilterKind::SharpHard:return {{0,-1,0,-1,6,-1,0,-1,0},2};
    case FilterKind::BlurSoft:return {{0,1,0,1,4,1,0,1,0},8};
    case FilterKind::BlurHard:return {{1,2,1,2,4,2,1,2,1},16};
    default:throw std::invalid_argument("Expected a fixed convolution preset");
    }
}
}
FilterWorkerOutcome convolution_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    FilterKind kind,std::stop_token stop,FilterCosts& costs){
    modal_detail::poll(stop);const bool invert=kind==FilterKind::Invert;const auto filter=invert?Kernel{}:kernel(kind);auto& image=*plane.raster;FilterWorkerOutcome result;
    const auto charge_work=[&](std::uint64_t n){modal_detail::charge(costs.work,n,modal_detail::max_work,"Fixed filter exceeds128Mi work");};
    const auto charge_storage=[&](std::uint64_t n){modal_detail::charge(costs.storage,n,modal_detail::max_bytes,"Fixed filter exceeds512MiB");};
    // 651C30 returns true below RGBA.656980 then refuses1/2/8/16
    // before its entry locks;6579F0 deliberately ignores that copyback BOOL.
    result.worker_succeeded=true;result.result.performed=true;
    if(image.depth<24)return result;
    const bool word=image.depth==64;const int lane=word?2:1,stride=image.depth/8,maximum=word?65535:255;
    const int columns=(image.width+255)/256,rows=(image.height+255)/256;
    const auto tile_count=std::uint64_t(columns)*rows;
    charge_storage(65536+image.tiles.size()*256ull);charge_work(tile_count+image.tiles.size());
    Raster source=image;
    const auto materialize=[&](Raster& target,TileCoord tile){
        if(target.tiles.contains(tile))return;
        charge_storage(image.tile_bytes()+128);charge_work(65536);
        selection_detail::store_raw_pixel(target,tile.x*256,tile.y*256,selection_detail::clear_word(image.depth));
    };
    if(image.depth==24){
        materialize(source,{0,0});
        const TableSelection selected(plane,owner,selection);
        if(!selected.empty()){
            charge_work(2ull*image.width*image.height);
            // Every copied byte is identity; only an actually admitted p[3]
            // outside its physical RGB24 tile needs an explicit refusal.
            for(const auto& [tile,unused]:source.tiles){modal_detail::poll(stop);const int x=tile.x*256+255,y=tile.y*256+255;
                if(x<image.width&&y<image.height&&selected.contains(x,y))throw std::invalid_argument("Fixed filter would access an unowned RGB24 fourth byte");}
        }
    }else{
        // Both native locks are writable. Each source MoveCursor and each
        // output setter visits the full page before selection copyback.
        charge_work(64ull*image.width*image.height);
        Raster transformed=image;
        for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){modal_detail::poll(stop);materialize(source,{tx,ty});materialize(transformed,{tx,ty});}
        const auto sample=[&](int x,int y){
            const auto at=(std::size_t(y%256)*256+x%256)*stride;const auto& bytes=*source.tiles.at({x/256,y/256});std::array<int,4> channels;
            for(int c=0;c<4;++c)channels[c]=bytes[at+c*lane]|(word?unsigned(bytes[at+c*lane+1])<<8:0);return channels;
        };
        const auto assign=[&](Raster& target,int x,int y,const std::array<int,4>& values,bool alpha_only){
            const auto at=(std::size_t(y%256)*256+x%256)*stride;auto& bytes=target.tiles.at({x/256,y/256});
            bool differs{};for(int c=alpha_only?3:0;c<4;++c){const int before=(*bytes)[at+c*lane]|(word?unsigned((*bytes)[at+c*lane+1])<<8:0);differs|=before!=values[c];}
            if(!differs)return;
            if(bytes.use_count()>1){charge_storage(image.tile_bytes()+128);charge_work(65536);bytes=std::make_shared<Bytes>(*bytes);}
            for(int c=alpha_only?3:0;c<4;++c){(*bytes)[at+c*lane]=std::uint8_t(values[c]);if(word)(*bytes)[at+c*lane+1]=std::uint8_t(values[c]>>8);}
        };
        const bool alpha_only=plane.common.opacity_mode==4;
        for(int y=0;y<image.height;++y){modal_detail::poll(stop);for(int x=0;x<image.width;++x){
            if(invert){auto output=sample(x,y);
                if(alpha_only)output[3]=maximum-output[3];
                else if(owner.family==0x11000800u&&(output[3]==0||(output[0]==maximum&&output[1]==maximum&&output[2]==maximum)))output={0,0,0,maximum};
                else if(output[3])for(int c=0;c<3;++c)output[c]=maximum-output[c];
                assign(transformed,x,y,output,false);continue;
            }
            std::array<std::int64_t,4> sums{};int index{};
            for(int dy=-1;dy<=1;++dy)for(int dx=-1;dx<=1;++dx){const auto pixel=sample(std::clamp(x+dx,0,image.width-1),std::clamp(y+dy,0,image.height-1));
                const auto weight=filter.weights[std::size_t(index++)];for(int c=alpha_only?3:0;c<4;++c)sums[c]+=std::int64_t(pixel[c])*weight;}
            std::array<int,4> output{};for(int c=alpha_only?3:0;c<4;++c)output[c]=int(std::clamp<std::int64_t>(sums[c]/filter.divisor,0,maximum));assign(transformed,x,y,output,alpha_only);
        }}
        // Even empty copyback returns the successful convolution BOOL, with
        // the source's full-page read materialization still observable.
        const TableSelection selected(plane,owner,selection);
        if(!selected.empty())for(int y=0;y<image.height;++y){modal_detail::poll(stop);for(int x=0;x<image.width;++x){if(!selected.contains(x,y))continue;
            const auto at=(std::size_t(y%256)*256+x%256)*stride;const auto& bytes=*transformed.tiles.at({x/256,y/256});std::array<int,4> output;
            for(int c=0;c<4;++c)output[c]=bytes[at+c*lane]|(word?unsigned(bytes[at+c*lane+1])<<8:0);assign(source,x,y,output,false);
        }}
    }
    result.result.storage_changed=source.tiles.size()!=image.tiles.size();
    for(const auto& [tile,bytes]:source.tiles){modal_detail::poll(stop);const auto original=image.tiles.find(tile);
        if(original!=image.tiles.end()&&original->second==bytes)continue;
        const int right=std::min((tile.x+1)*256,image.width),bottom=std::min((tile.y+1)*256,image.height);
        for(int y=tile.y*256;y<bottom;++y){modal_detail::poll(stop);for(int x=tile.x*256;x<right;++x)result.result.changed_pixels+=source.pixel(x,y)!=image.pixel(x,y);}
    }
    result.result.changed=result.result.changed_pixels!=0;result.modified=result.result.changed||result.result.storage_changed;
    modal_detail::poll(stop);if(result.modified)image=std::move(source);return result;
}
}
