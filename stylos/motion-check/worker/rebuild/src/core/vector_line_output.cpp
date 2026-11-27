#include "vector_direct_context.hpp"
#include "vector_line_output.hpp"
#include "paint_owner_render.hpp"
#include "text_font_backend_internal.hpp"
#include <bit>
#include <limits>
#include <stdexcept>

namespace retas::vector_render_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
bool raster_depth(int depth){return depth==1||depth==2||depth==8||depth==16||depth==24||depth==32||depth==64;}
int raster_scale(double source,double target){for(int scale:{1,2,4,8})if(source==numeric_detail::binary64(target*scale))return scale;return 0;}
void charge(std::uint64_t& total,std::uint64_t more,std::uint64_t limit){if(total>limit||more>limit-total)throw std::length_error("Native document output exceeds cumulative limits");total+=more;}
void admit_raster(const Layer& layer,double dpi,StoredLineCacheCosts& costs,std::stop_token stop){
    if(layer.planes.size()>1024)throw std::length_error("Native General output plane limit exceeded");
    charge(costs.storage,layer.specific.size()+layer.suffix.size()+layer.plane_suffix.size()+layer.common.name.size()+layer.common.suffix.size()+sizeof(Layer),max_storage);
    for(const auto& p:layer.planes){
        if(stop.stop_requested())throw std::runtime_error("Native document output cancelled");
        if(p.opaque||!p.raster)throw std::invalid_argument("Missing native General output source");
        const auto& r=*p.raster;
        if(r.width<1||r.height<1||r.width>100000||r.height>100000||std::int64_t(r.width)*r.height>268435456
            ||!raster_depth(r.depth)||r.color_order!=(r.depth==24?4:r.depth<32?0:3)||r.width!=p.common.width||r.height!=p.common.height
            ||r.depth!=p.common.depth||!std::isfinite(r.resolution)||r.resolution<=0||r.resolution!=p.common.resolution)
            throw std::invalid_argument("Invalid native General output raster metadata");
        charge(costs.storage,p.specific.size()+p.suffix.size()+p.common.name.size()+p.common.suffix.size()+sizeof(Plane),max_storage);
        charge(costs.work,1,max_work);
        const auto scale=raster_scale(p.common.resolution,dpi);std::uint64_t present_work{};
        for(const auto& [key,tile]:r.tiles){
            if(stop.stop_requested())throw std::runtime_error("Native document output cancelled");
            const auto x=std::int64_t(key.x)*256,y=std::int64_t(key.y)*256;
            if(x<0||y<0||x>=r.width||y>=r.height||!tile||tile->size()!=r.tile_bytes())
                throw std::invalid_argument("Invalid native General output raster tile");
            charge(costs.storage,tile->size()+sizeof(Bytes)+128,max_storage);
            const auto samples=std::uint64_t(std::min<std::int64_t>(256,r.width-x))*std::uint64_t(std::min<std::int64_t>(256,r.height-y));present_work+=samples;
            //2x includes a preserved padding bit after an odd logical width,
            // but drops an unpaired final row. Bill it even in hidden sources.
            const auto padding=scale==2&&r.depth==1&&r.width%2&&x+256>=r.width?std::uint64_t(std::min<std::int64_t>(256,r.height-y)/2)*2:0;
            charge(costs.work,samples+padding+1,max_work);
        }
        if(layer.common.opacity_mode==2&&scale==1)charge(costs.work,std::uint64_t(r.width)*r.height-present_work+std::uint64_t((r.width+255)/256)*std::uint64_t((r.height+255)/256),max_work);
    }
}
Pixel word_cache_pixel(const Raster& target,TileCoord tile,std::size_t at){
    const auto found=target.tiles.find(tile);if(found==target.tiles.end())return {};
    const auto& bytes=*found->second;
    const auto word=[&](std::size_t index){return std::uint16_t(unsigned(bytes[index])|unsigned(bytes[index+1])<<8);};
    return {word(at+4),word(at+2),word(at),word(at+6)};
}
void set_word_cache_pixel(Raster& target,TileCoord tile,std::size_t at,Pixel pixel){
    auto& backing=target.tiles[tile];
    if(!backing){backing=std::make_shared<Bytes>(target.tile_bytes(),0xff);for(std::size_t i=6;i<backing->size();i+=8)(*backing)[i]=(*backing)[i+1]=0;}
    const auto word=[&](std::size_t index,std::uint16_t value){(*backing)[index]=std::uint8_t(value);(*backing)[index+1]=std::uint8_t(value>>8);};
    word(at,pixel.b);word(at+2,pixel.g);word(at+4,pixel.r);word(at+6,pixel.a);
}
void render_scaled_bitmap_word(const Raster& source,const CommonElement& owner,Raster& target,std::stop_token stop){
    // GetNextBlock orders rows before columns. Doubled physical target rows
    // can overlap the next source block, so preserve this source traversal.
    std::vector<std::pair<TileCoord,const Bytes*>> tiles;tiles.reserve(source.tiles.size());
    for(const auto& [tile,bytes]:source.tiles)tiles.emplace_back(tile,bytes.get());
    std::sort(tiles.begin(),tiles.end(),[](const auto& a,const auto& b){return std::pair{a.first.y,a.first.x}<std::pair{b.first.y,b.first.x};});
    for(const auto& [tile,bytes]:tiles){
        if(stop.stop_requested())throw std::runtime_error("Native Line output cancelled");
        const auto left=std::max<std::int64_t>(std::int64_t(tile.x)*32,-std::int64_t(owner.x)),top=std::max<std::int64_t>(std::int64_t(tile.y)*32,-std::int64_t(owner.y));
        const auto right=std::min({std::int64_t(tile.x)*32+32,std::int64_t(source.width/8),std::int64_t(target.width)-owner.x}),bottom=std::min({std::int64_t(tile.y)*32+32,std::int64_t(source.height/8),std::int64_t(target.height)-owner.y});
        if(left>=right||top>=bottom)continue;
        // The root issues separate logical target256 work rectangles. Reset
        // the leaf cursor for each intersection with a physical source block.
        for(auto ty=(top+owner.y)/256;ty<=(bottom-1+owner.y)/256;++ty)for(auto tx=(left+owner.x)/256;tx<=(right-1+owner.x)/256;++tx){
            const auto begin_x=std::max(left,tx*256-owner.x),begin_y=std::max(top,ty*256-owner.y);
            const auto end_x=std::min(right,(tx+1)*256-owner.x),end_y=std::min(bottom,(ty+1)*256-owner.y);
            const auto first_row=begin_y+owner.y-ty*256;
            if(first_row+2*(end_y-begin_y-1)>=256)throw std::invalid_argument("Native8x WORD output would access unowned target tile rows");
            const TileCoord destination{int(tx),int(ty)};
            for(auto y=begin_y;y<end_y;++y){if(stop.stop_requested())throw std::runtime_error("Native Line output cancelled");
                const auto physical_y=first_row+2*(y-begin_y);
                for(auto x=begin_x;x<end_x;++x){unsigned count{};const int sx=int(x*8)%256,sy=int(y*8)%256;
                    for(int row=0;row<8;++row)count+=std::popcount(unsigned((*bytes)[std::size_t(sy+row)*32+sx/8]));
                    if(!count)continue;
                    const auto at=std::size_t(physical_y*256+x+owner.x-tx*256)*8;
                    const auto d=word_cache_pixel(target,destination,at),s=output_terminal::scaled_bitmap_word_behind_white(d,count);
                    if(s!=d)set_word_cache_pixel(target,destination,at,s);
                }
            }
        }
    }
}
std::optional<std::vector<const Layer*>> admitted_layers(const DocumentData& data,LineDocumentOutput purpose){
    if(data.unrestored_external_content||data.layers.empty())return {};
    std::vector<const Layer*> layers;layers.reserve(data.layers.size());
    for(const auto& l:data.layers){if(l.family==0x10008000||((purpose==LineDocumentOutput::StandardImage||purpose==LineDocumentOutput::StandardImageWhite)&&l.family==0x10040000))continue;layers.push_back(&l);}
    if(layers.empty())return {};
    for(const auto* layer:layers){const auto& l=*layer;
        if(l.family==0x40000200)continue;
        if(l.family==0x40000800||l.family==0x48040800){
            if(l.opaque||!std::isfinite(l.common.resolution)||l.common.resolution<=0||l.common.opacity<0||l.common.opacity>100)return {};
            for(const auto& p:l.planes)if(!text_font_detail::image_masks(l,p,data.base.dpi))return {};
            continue;
        }
        if(l.family==0x21200000||l.family==0x20800000){
            if(l.opaque||(l.common.depth!=32&&l.common.depth!=64)||!std::isfinite(l.common.resolution)||l.common.resolution<=0
                ||l.common.opacity<0||l.common.opacity>100||l.common.opacity_mode<1||l.common.opacity_mode>5
                ||l.planes.size()>1024||std::abs(std::int64_t(l.common.x))>65536||std::abs(std::int64_t(l.common.y))>65536)return {};
            continue;
        }
        if(l.opaque||(l.family!=0x22100000&&l.family!=0x24400000&&l.family!=0x20000200&&l.family!=0x10000100)
            ||(l.family==0x10000100?!raster_depth(l.common.depth):(l.common.depth!=32&&l.common.depth!=64))||l.common.opacity<0||l.common.opacity>100
            ||(l.common.opacity_mode!=1&&(l.family!=0x10000100||l.common.opacity_mode!=2))
            ||(l.family==0x10000100?l.common.resolution!=data.base.dpi:(!std::isfinite(l.common.resolution)||l.common.resolution<=0))||(l.family!=0x10000100&&l.common.flags[4])||l.planes.empty()
            ||std::abs(std::int64_t(l.common.x))>65536||std::abs(std::int64_t(l.common.y))>65536)return {};
        if(l.family==0x10000100)for(const auto& p:l.planes){
            if(p.family!=0x10000001||!raster_depth(p.common.depth)||p.common.opacity_mode<1||p.common.opacity_mode>5)return {};}
    }
    return layers;
}
}
bool requires_native_paint_image(const DocumentData& data){
    return std::ranges::any_of(data.layers,[](const Layer& l){return (l.family==0x21200000||l.family==0x20800000)
        &&(l.common.opacity!=100||(l.common.opacity_mode!=1&&l.common.opacity_mode!=4));});
}
bool supports_native_line_document(const DocumentData& data,LineDocumentOutput purpose){return bool(admitted_layers(data,purpose));}
std::optional<Raster> render_native_line_document(const DocumentData& data,int depth,LineDocumentOutput purpose,std::stop_token stop,StoredLineCacheCosts* ledger){
    const auto poll=[&]{if(stop.stop_requested())throw std::runtime_error("Native Line output cancelled");};poll();
    if(depth!=32&&depth!=64)throw std::invalid_argument("Native Line output requires32/64 target");
    StoredLineOutputSampling sampling;bool force_white{};std::optional<Pixel> background;
    switch(purpose){case LineDocumentOutput::StandardImage:sampling=StoredLineOutputSampling::Fractional4x;break;
    case LineDocumentOutput::StandardImageWhite:sampling=StoredLineOutputSampling::Fractional4x;force_white=true;break;
    case LineDocumentOutput::PlainPreview:sampling=StoredLineOutputSampling::Plain1x;force_white=true;break;
    case LineDocumentOutput::QualityPreview:sampling=StoredLineOutputSampling::Fractional4x;force_white=true;break;
    default:throw std::invalid_argument("Unsupported native Line output purpose");}
    const auto admitted=admitted_layers(data,purpose);if(!admitted)return {};
    const auto width=data.base.width(),height=data.base.height();
    const auto& layers=*admitted;
    // Request61/361/322 all scan the visible background-family stack in
    // reverse order. The family bit, not Common.flags4, selects its color.
    for(std::size_t i=layers.size();i-->0;){const auto& l=*layers[i];if(l.common.flags[1]&&(l.family&0x04000000u)){
        background=Pixel::rgba8(unsigned(l.common.color>>8)&255,unsigned(l.common.color>>24)&255,unsigned(l.common.color>>40)&255);break;}}
    if(!background&&force_white)background=Pixel::rgba8(255,255,255);
    Raster result(width,height,depth,data.base.dpi);
    const auto columns=std::uint64_t((width+255)/256),rows=std::uint64_t((height+255)/256),cells=columns*rows*65536;
    StoredLineCacheCosts costs=ledger?*ledger:StoredLineCacheCosts{};
    charge(costs.storage,cells*std::uint64_t(depth/8),max_storage);charge(costs.work,2*cells,max_work);
    const auto remaps=adjust_render_detail::prepare(data,costs.storage,costs.work,stop);
    struct Owner {VectorRenderContext context;std::vector<StoredLinePlane> planes;bool visible{};const Layer* raster{};unsigned density{};const adjust_render_detail::Tables* tables{};const Layer* text{};std::optional<Raster> paint{};bool opaque{};};
    std::vector<Owner> owners;owners.reserve(layers.size());std::size_t visible{},single_planes{};
    for(const auto* layer:layers){const auto& l=*layer;poll();
        if(l.family==0x40000200)continue;
        const auto* tables=remaps.empty()?nullptr:remaps[std::size_t(layer-data.layers.data())].get();
        if(l.family==0x40000800||l.family==0x48040800){
            resolution_detail::Budget budget{costs.storage,costs.work,stop};
            if(l.planes.size()>100000)throw std::length_error("Native Text output plane limit exceeded");
            budget.allocate(l.specific.size()+l.suffix.size()+l.plane_suffix.size()+l.common.name.size()+l.common.suffix.size()+sizeof(Layer));budget.visit(1);
            for(const auto& p:l.planes){
                budget.allocate(p.specific.size()+p.suffix.size()+p.common.name.size()+p.common.suffix.size()+sizeof(Plane)+p.runtime_text_cache->specific.size()+sizeof(TextRasterCache));
                budget.visit(p.specific.size()*3ull+1); // admission and draw fingerprints
                text_font_detail::admit_display_masks(*text_font_detail::image_masks(l,p,data.base.dpi),budget);
            }
            costs.storage=budget.bytes;costs.work=budget.work;
            owners.push_back({{}, {},bool(l.common.flags[1]),nullptr,0,nullptr,&l});
            if(l.common.flags[1]){++visible;single_planes=l.planes.size();}continue;
        }
        if(l.family==0x10000100){admit_raster(l,data.base.dpi,costs,stop);owners.push_back({{}, {},bool(l.common.flags[1]),&l,0,tables});if(l.common.flags[1]){++visible;single_planes=l.planes.size();}continue;}
        if(l.family==0x21200000||l.family==0x20800000){
            const bool opaque=l.common.opacity_mode==2;
            Owner owner;owner.context=direct_output_context(l,{0,0,width,height},data.base.dpi,depth,purpose==LineDocumentOutput::PlainPreview?1u:4u);
            owner.visible=l.common.flags[1];owner.density=output_terminal::density(opaque?100:l.common.opacity,depth);owner.tables=tables;owner.opaque=opaque;
            owner.paint=paint_direct_redraw(l,owner.context,costs,stop);
            charge(costs.work,std::uint64_t(width)*height,max_work);
            if(owner.visible){++visible;single_planes=l.planes.size();}
            owners.push_back(std::move(owner));continue;
        }
        std::uint64_t wire=l.specific.size();for(const auto& p:l.planes)wire+=p.specific.size();
        const auto validations=std::max<std::size_t>(l.planes.size(),1);
        if(wire>(max_storage-costs.storage)/16||wire>(max_work-costs.work)/validations)throw std::length_error("Native Line output source exceeds cumulative limits");
        costs.work+=wire*validations;
        Owner owner{direct_output_context(l,{0,0,width,height},data.base.dpi,depth,1),decode_stored_line_planes(l),bool(l.common.flags[1]),nullptr,output_terminal::density(l.common.opacity,depth),tables};
        costs=stored_line_direct_costs(owner.planes,owner.context,sampling,costs);
        if(!l.runtime_vector_cache_present)throw std::invalid_argument("Native Line redraw requires a runtime cache carrier");
        // Whole hidden graph/domain/cost validation precedes publication.
        std::uint64_t reserved{};for(auto& p:owner.planes){reserved+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=owner.planes.size();i-->0;)if(!l.planes[i].common.flags[1])owner.planes.erase(owner.planes.begin()+i);
        if(!owner.planes.empty())owner.planes.front().reserved_storage=reserved;
        if(owner.visible){++visible;single_planes=l.planes.size();}
        owners.push_back(std::move(owner));
    }
    const bool copy=!background&&visible==1&&single_planes==1;
    if(copy)for(const auto& owner:owners)if(owner.visible&&!owner.raster&&!owner.text&&owner.tables)
        charge(costs.work,std::uint64_t(width)*height,max_work);
    for(std::size_t i=owners.size();i-->0;){poll();const auto& owner=owners[i];if(!owner.visible)continue;
        if(owner.paint){
            //440500 forces62F4A0's target-depth temporary. It owns every
            //requested physical block, so selector1 sees present white/A0.
            const auto& source=*owner.paint;
            for(int y=0;y<height;++y){if(!(y%32))poll();for(int x=0;x<width;++x){
                const auto old=result.pixel(x,y);
                const auto pixel=owner.opaque?output_terminal::opaque_present(old,source.pixel(x,y),depth,depth,owner.tables)
                    :output_terminal::behind_white(old,output_terminal::raster_source(source.pixel(x,y),depth,depth,owner.density,false,owner.tables,copy),depth,copy);
                if(pixel!=old)result.set_pixel(x,y,pixel);
            }}
            continue;
        }
        if(owner.text){const auto& l=*owner.text;
            for(std::size_t n=l.planes.size();n-->0;){const auto& p=l.planes[n];poll();if(!p.common.flags[1])continue;
                text_font_detail::paint_display_masks(result,*text_font_detail::image_masks(l,p,data.base.dpi),l.common.opacity,p.runtime_text_cache->image_geometry?std::array<int,2>{}:std::array<int,2>{l.common.x,l.common.y},stop);
            }
            continue;
        }
        if(owner.raster){const auto& l=*owner.raster;
            // CRasterLayer+A8 sends each reverse saved plane directly to the
            // document terminal. Precomposing a modern owner changes rounding.
            for(std::size_t n=l.planes.size();n-->0;){const auto& p=l.planes[n];poll();if(!p.common.flags[1])continue;const auto& r=*p.raster;
                const auto scale=raster_scale(p.common.resolution,data.base.dpi);const bool opaque=l.common.opacity_mode==2&&scale==1;
                // 63E860 has no leaf for the other60D550 selectors. They
                // still count as saved children and suppress root fast copy.
                if(!scale)continue;
                const auto density=output_terminal::density(opaque?100:l.common.opacity,depth);
                const auto material=p.common.flags[4]?p.common.color:0ull;
                if(scale>1){
                    if(depth==64&&scale==8&&r.depth==1){render_scaled_bitmap_word(r,l.common,result,stop);continue;}
                    // Native scaled wrappers dispatch only source1. The2x/4x
                    // wrappers have no target64 leaf; missing blocks also skip.
                    if(depth!=32||r.depth!=1)continue;
                    const auto effective=density|((scale==2||scale==4)&&p.common.flags[4]?unsigned(p.common.color>>56):0u);
                    const auto projected_width=r.width/scale+(scale==2&&r.width%2),projected_height=r.height/scale;
                    for(const auto& [tile,bytes]:r.tiles){poll();const auto cell_side=256/scale;
                        const auto left=std::max<std::int64_t>(std::int64_t(tile.x)*cell_side,-std::int64_t(l.common.x)),top=std::max<std::int64_t>(std::int64_t(tile.y)*cell_side,-std::int64_t(l.common.y));
                        const auto right=std::min({std::int64_t(tile.x+1)*cell_side,std::int64_t(projected_width),std::int64_t(width)-l.common.x}),bottom=std::min({std::int64_t(tile.y+1)*cell_side,std::int64_t(projected_height),std::int64_t(height)-l.common.y});
                        for(auto y=top;y<bottom;++y){if(!(y%32))poll();for(auto x=left;x<right;++x){const int sx=int(x*scale)%256,sy=int(y*scale)%256,shift=8-scale-sx%8;unsigned count{};
                            for(int row=0;row<scale;++row)count+=std::popcount((unsigned((*bytes)[std::size_t(sy+row)*32+sx/8])>>shift)&((1u<<scale)-1));
                            if(count){const int dx=int(x+l.common.x),dy=int(y+l.common.y);const auto d=result.pixel(dx,dy),s=output_terminal::scaled_mask_behind_white(d,count,scale,effective,material);if(s!=d)result.set_pixel(dx,dy,s);}}}
                    }
                    continue;
                }
                // The native64 fixed packet ORs its saved alpha into density;
                // no-LUT RGBA terminals still read RGB from the raster.
                const auto plane_density=density|(depth==64&&p.common.flags[4]?unsigned(p.common.color>>48):0u);
                const bool mask=depth==32&&(r.depth==1||r.depth==8),rgba=r.depth==32||r.depth==64;
                const auto block=[&](TileCoord tile,bool present){
                    const auto left=std::max<std::int64_t>(std::int64_t(tile.x)*256,-std::int64_t(l.common.x)),top=std::max<std::int64_t>(std::int64_t(tile.y)*256,-std::int64_t(l.common.y));
                    const auto right=std::min({std::int64_t(tile.x)*256+256,std::int64_t(r.width),std::int64_t(width)-l.common.x}),bottom=std::min({std::int64_t(tile.y)*256+256,std::int64_t(r.height),std::int64_t(height)-l.common.y});
                    for(auto y=top;y<bottom;++y){if(!(y%32))poll();for(auto x=left;x<right;++x){const int dx=int(x+l.common.x),dy=int(y+l.common.y);
                        if(opaque&&!present)result.set_pixel(dx,dy,output_terminal::opaque_missing(result.pixel(dx,dy),owner.tables,depth));
                        else if(mask){const auto sample=unsigned(r.pixel(int(x),int(y)).a/257);if(sample){const auto d=result.pixel(dx,dy),s=output_terminal::mask_behind_white(d,sample,r.depth,density,material);if(s!=d)result.set_pixel(dx,dy,s);}}
                        else if(depth==32&&r.depth==24){const auto d=result.pixel(dx,dy);if(d.a&&d.a!=65535&&density){
                            const auto at=(std::size_t(y%256)*256+std::size_t(x%256))*3;const auto& bytes=*r.tiles.at(tile);
                            if(at+3>=bytes.size())throw std::invalid_argument("Native RGB24 output would read an unowned physical coverage byte");
                            const auto s=output_terminal::rgb24_behind_white(d,r.pixel(int(x),int(y)),bytes[at+3],density);if(s!=d)result.set_pixel(dx,dy,s);}}
                        // Native depth-switch defaults skip present samples;
                        // owner/plane counts and opaque missing fill still apply.
                        else if(!rgba)continue;
                        else if(opaque)result.set_pixel(dx,dy,output_terminal::opaque_present(result.pixel(dx,dy),r.pixel(int(x),int(y)),r.depth,depth,owner.tables));
                        else{const auto s=output_terminal::raster_source(r.pixel(int(x),int(y)),r.depth,depth,plane_density,p.common.opacity_mode==5,owner.tables,copy);if(copy||s.a)result.set_pixel(dx,dy,output_terminal::behind_white(result.pixel(dx,dy),s,depth,copy));}}}
                };
                if(opaque){const auto left=std::max<std::int64_t>(0,-std::int64_t(l.common.x)),top=std::max<std::int64_t>(0,-std::int64_t(l.common.y));
                    const auto right=std::min<std::int64_t>(r.width,std::int64_t(width)-l.common.x),bottom=std::min<std::int64_t>(r.height,std::int64_t(height)-l.common.y);
                    if(left<right&&top<bottom)for(auto y=top/256;y<=(bottom-1)/256;++y)for(auto x=left/256;x<=(right-1)/256;++x){poll();const TileCoord tile{int(x),int(y)};block(tile,r.tiles.contains(tile));}
                }else for(const auto& [tile,bytes]:r.tiles)block(tile,true);
            }
            continue;
        }
        const auto source=render_stored_line_direct(owner.planes,owner.context,sampling,stop);
        if(copy&&owner.tables){
            // 62F4A0 pre-materializes every tile in the document request,
            // even for an empty Line list. Copy+LUT observes its white/A0
            // samples outside the sparse geometry cache's allocated tiles.
            for(int y=0;y<height;++y){if(!(y%32))poll();for(int x=0;x<width;++x){
                const auto s=output_terminal::raster_source(source.pixel(x,y),depth,depth,owner.density,false,owner.tables,true);
                if(s!=result.pixel(x,y))result.set_pixel(x,y,s);
            }}
            continue;
        }
        for(const auto& [tile,bytes]:source.tiles){for(int y=tile.y*256;y<std::min(height,(tile.y+1)*256);++y){if(!(y%32))poll();
            for(int x=tile.x*256;x<std::min(width,(tile.x+1)*256);++x){const auto s=output_terminal::raster_source(source.pixel(x,y),depth,depth,owner.density,false,owner.tables,copy);if(copy||s.a)result.set_pixel(x,y,output_terminal::behind_white(result.pixel(x,y),s,depth,copy));}}}
    }
    if(background){for(int y=0;y<height;++y){if(!(y%32))poll();for(int x=0;x<width;++x)result.set_pixel(x,y,output_terminal::finish(result.pixel(x,y),depth,background,false));}}
    else if(!copy){for(const auto& [tile,bytes]:result.tiles){for(int y=tile.y*256;y<std::min(height,(tile.y+1)*256);++y){if(!(y%32))poll();
        for(int x=tile.x*256;x<std::min(width,(tile.x+1)*256);++x){const auto p=result.pixel(x,y);if(p.a)result.set_pixel(x,y,output_terminal::finish(p,depth,std::nullopt,false));}}}}
    poll();if(ledger)*ledger=costs;return result;
}
}
