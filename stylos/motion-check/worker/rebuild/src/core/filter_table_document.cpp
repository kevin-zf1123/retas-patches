#include "filter_document.hpp"
#include "filter_table_selection.hpp"
#include "modal_document_guard.hpp"
#include "raster_history.hpp"
#include "selection_raster.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

namespace retas::filter_detail {
FilterWorkerOutcome table_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    FilterKind kind,const FilterSettings& settings,std::stop_token stop,FilterCosts& costs){
    modal_detail::poll(stop);FilterWorkerOutcome result;auto& image=*plane.raster;
    modal_detail::charge(costs.storage,65536+image.tiles.size()*128,modal_detail::max_bytes,"Table filter tables exceed512MiB");
    std::uint64_t table_work=kind==FilterKind::Contrast?1024:2048;
    if(kind==FilterKind::ToneCurve){const auto& value=std::get<ToneCurveSettings>(settings);table_work=768;
        for(const auto& curve:value.curves){const auto n=std::uint64_t(curve.points.size());table_work+=256+(value.interpolation?1024*n*n:33*n*16);}}
    modal_detail::charge(costs.work,table_work,modal_detail::max_work,"Table filter tables exceed128Mi work");
    const auto tables=byte_rgb_tables(kind,settings);
    // 654220 rejects low depths before its write lock. 653F00/653F80 use
    // whole-page CopyAll, so a normal false restores this child's exact source.
    if(image.depth!=24&&image.depth!=32&&image.depth!=64)return result;
    const bool selected=int(selection.state)>=2,alpha_only=plane.common.opacity_mode==4;
    const TableSelection admitted(plane,owner,selection);if(admitted.empty())return result;
    modal_detail::charge(costs.work,2ull*image.width*image.height+((std::uint64_t(image.width)+255)/256)*((std::uint64_t(image.height)+255)/256),
        modal_detail::max_work,"Table filter scan exceeds128Mi work");
    Raster staged=image;bool modified{};
    const auto materialize=[&](TileCoord key){
        if(staged.tiles.contains(key))return;
        modal_detail::charge(costs.storage,image.tile_bytes()+128,modal_detail::max_bytes,"Table filter backing exceeds512MiB");
        modal_detail::charge(costs.work,65536,modal_detail::max_work,"Table filter clear exceeds128Mi work");
        selection_detail::store_raw_pixel(staged,key.x*256,key.y*256,selection_detail::clear_word(image.depth));modified=true;result.result.storage_changed=true;
    };
    // The native entry write lock allocates00 before GetBlockExist, making it
    // a present source even for an initially missing RGB24 surface.
    materialize({0,0});
    const int columns=(image.width+255)/256,rows=(image.height+255)/256;
    for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){
        modal_detail::poll(stop);const TileCoord key{tx,ty};const bool present=staged.tiles.contains(key);
        if(!present&&(!alpha_only||(!selected&&!tables[0][0])))continue;
        const int left=tx*256,top=ty*256,right=std::min(left+256,image.width),bottom=std::min(top+256,image.height);
        materialize(key);Bytes* output{};
        if(!present&&!selected&&image.depth==24){result.result.changed_pixels+=std::size_t(right-left)*(bottom-top);continue;}
        const auto writable=[&]() -> Bytes& {
            auto& bytes=staged.tiles.at(key);if(bytes.use_count()>1){
                modal_detail::charge(costs.storage,image.tile_bytes(),modal_detail::max_bytes,"Table filter COW exceeds512MiB");bytes=std::make_shared<Bytes>(*bytes);}
            output=bytes.get();return *output;
        };
        const auto assign=[&](std::size_t at,std::uint8_t value){const auto& bytes=output?*output:*staged.tiles.at(key);
            if(bytes[at]!=value){writable()[at]=value;modified=true;}};
        for(int y=top;y<bottom;++y){modal_detail::poll(stop);for(int x=left;x<right;++x){
            // Whole-surface mode4 uses RectFill for a missing block. A
            // selected missing block instead materializes before mask gates.
            if(present||selected){if(!admitted.contains(x,y))continue;}
            const auto at=(std::size_t(y%256)*256+x%256)*(image.depth/8);
            if(image.depth==64){
                const auto& bytes=output?*output:*staged.tiles.at(key);
                if(alpha_only){const auto value=(!present&&!selected)?tables[0][0]:tables[0][bytes[at+7]];assign(at+6,0);assign(at+7,value);}
                else if(bytes[at+6]||bytes[at+7]){const auto b=tables[2][bytes[at+1]],g=tables[1][bytes[at+3]],r=tables[0][bytes[at+5]];
                    assign(at,0);assign(at+1,b);assign(at+2,0);assign(at+3,g);assign(at+4,0);assign(at+5,r);}
            }else {
                if(at+3>=image.tile_bytes())throw std::invalid_argument("Table filter would access an unowned RGB24 fourth byte");
                const auto& bytes=output?*output:*staged.tiles.at(key);
                if(alpha_only)assign(at+3,(!present&&!selected)?tables[0][0]:tables[0][bytes[at+3]]);
                else if(bytes[at+3]){const auto b=tables[2][bytes[at]],g=tables[1][bytes[at+1]],r=tables[0][bytes[at+2]];
                    assign(at,b);assign(at+1,g);assign(at+2,r);}
            }
        }}
        // RGB24's fourth-byte writes can change neighboring logical samples.
        for(int y=top;y<bottom;++y){modal_detail::poll(stop);for(int x=left;x<right;++x)
            result.result.changed_pixels+=staged.pixel(x,y)!=image.pixel(x,y);}
    }
    modal_detail::poll(stop);if(modified)image=std::move(staged);
    result.result.changed=result.result.changed_pixels!=0;result.result.performed=true;result.modified=modified;result.worker_succeeded=true;return result;
}
}
