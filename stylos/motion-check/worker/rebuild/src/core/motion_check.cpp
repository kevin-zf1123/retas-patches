#include "retas/motion_check.hpp"
#include "retas/preview.hpp"
#include "retas/numeric.hpp"
#include "vector_line_output.hpp"
#include "modal_document_guard.hpp"
#include <algorithm>
#include <limits>

namespace retas {
namespace {
constexpr int fps[]{30,25,24,12,10,8};
struct Entry {bool& value;explicit Entry(bool& state):value(state){if(value)throw std::logic_error("Reentrant MotionCheck session");value=true;}~Entry(){value=false;}};
void light_table_charge(const Raster& r,std::uint64_t& storage){
    if(r.depth!=32||r.color_order!=3||r.width<1||r.height<1||r.width>20000||r.height>20000||!std::isfinite(r.resolution)||r.resolution<=0)throw std::invalid_argument("Invalid MotionCheck LightTable");
    modal_detail::charge(storage,sizeof(Raster)+r.tiles.size()*128,modal_detail::max_bytes,"Motion LightTable metadata exceeds512MiB");
    for(const auto& [key,bytes]:r.tiles){if(key.x<0||key.y<0||std::int64_t(key.x)*256>=r.width||std::int64_t(key.y)*256>=r.height||!bytes||bytes->size()!=r.tile_bytes())throw std::invalid_argument("Invalid MotionCheck LightTable tile");modal_detail::bytes_charge(storage,*bytes);}
}

void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("MotionCheck cancelled");}
Pixel color(std::uint32_t c){return Pixel::rgba8(c&255,(c>>8)&255,(c>>16)&255);}
void validate(const MotionCheckSettings& s){if(!std::isfinite(s.scale)||s.scale<=0||s.scale>10000||s.fps_index<0||s.fps_index>5||s.pause_time<0||s.pause_time>3600)throw std::invalid_argument("Invalid MotionCheck scale/FPS/pause range");}
int scaled(int extent,double scale){const auto value=legacy_rt_double_to_int(numeric_detail::binary64(numeric_detail::binary64(extent*scale)*.01));if(!value||*value<1||*value>20000)throw std::invalid_argument("Invalid MotionCheck cache size");return *value;}
std::vector<int> stretch_indices(int source,int destination){std::vector<int> out(std::size_t(destination),0);int at{},error=destination;for(auto& i:out){i=at;at+=source/destination;error-=source%destination;if(error<0){error+=destination;++at;}}return out;}
}
MotionCheckSettings load_motion_check_preferences(std::span<const PrefRecord> records,MotionCheckSettings value){
    const auto read=[&](std::string_view name,std::size_t width,const auto& apply){const auto found=lookup_preference_record(records,ascii(name),width);if(found.status==PrefLookupStatus::Found)apply(found.payload);};
    read("Scale",8,[&](auto p){value.scale=read_pref_f64(p);});
    for(const auto& [name,target]:std::initializer_list<std::pair<std::string_view,std::int32_t*>>{{"Pause",&value.pause},{"PauseTime",&value.pause_time},{"ShowLightTable",&value.show_light_table},{"SelectedArea",&value.selected_area},{"ShowVoice",&value.show_voice},{"PlaySound",&value.play_sound},{"Darken",&value.darken},{"FPSIndex",&value.fps_index},{"ShowShadowLayer",&value.show_shadow_layer},{"ShowAll",&value.show_all}})read(name,4,[&](auto p){*target=read_pref_i32(p);});
    read("BGColor",4,[&](auto p){value.background=std::uint32_t(read_pref_i32(p));});read("BlankColor",4,[&](auto p){value.blank_color=std::uint32_t(read_pref_i32(p));});
    read("ShowScoreLayer",2,[&](auto p){value.show_score_layer=std::uint16_t(unsigned(p[0])|unsigned(p[1])<<8);});return value;
}
MotionCheckSettings load_stylos_motion_preferences(std::span<const PrefRecord> records){
    auto value=load_motion_check_preferences(records);
    const auto bank=lookup_preference_record(records,ascii("ShowScoreLayer"),2);
    if(bank.status==PrefLookupStatus::Found)value.show_score_layer=std::uint16_t((unsigned(bank.payload[0])<<8)|bank.payload[1]);
    return value;
}
PrefArchive store_motion_check_preferences(const MotionCheckSettings& s,std::span<const PrefRecord> old){
    PrefArchive p;const auto put=[&](std::string_view name,Bytes bytes){p.push_back({ascii(name),std::move(bytes)});};put("Scale",pref_f64_payload(s.scale));
    for(const auto& [name,value]:std::initializer_list<std::pair<std::string_view,std::int32_t>>{{"Pause",s.pause},{"PauseTime",s.pause_time},{"ShowLightTable",s.show_light_table},{"SelectedArea",s.selected_area},{"BGColor",std::int32_t(s.background)},{"BlankColor",std::int32_t(s.blank_color)}})put(name,pref_i32_payload(value));
    put("ShowScoreLayer",Bytes{std::uint8_t(s.show_score_layer),std::uint8_t(s.show_score_layer>>8)});
    for(const auto& [name,value]:std::initializer_list<std::pair<std::string_view,std::int32_t>>{{"ShowVoice",s.show_voice},{"PlaySound",s.play_sound},{"Darken",s.darken},{"FPSIndex",s.fps_index},{"ShowShadowLayer",s.show_shadow_layer},{"ShowAll",s.show_all}})put(name,pref_i32_payload(value));
    return merge_preference_archives(p,old,PrefMergeMode::PreserveUnshadowed);
}
static Raster prepare_frame(const DocumentData& data,const MotionFrameRequest& request,std::stop_token stop,std::uint64_t retained){
    poll(stop);validate(request.settings);const auto& settings=request.settings;
    if(request.text_fonts!=MotionTextFonts::None&&request.text_fonts!=MotionTextFonts::Qt)throw std::invalid_argument("Invalid MotionCheck Text font provider");
    const auto area=settings.show_all?Rect{0,0,data.base.width(),data.base.height()}:request.fixed_area;
    if(area.width<=0||area.height<=0||area.width>20000||area.height>20000)throw std::invalid_argument("Invalid MotionCheck frame rectangle");
    const int width=settings.scale<100?scaled(area.width,settings.scale):area.width,height=settings.scale<100?scaled(area.height,settings.scale):area.height;
    if(std::uint64_t(width)*height>64ull*1024*1024)throw std::length_error("MotionCheck frame exceeds64Mi pixels");
    std::uint64_t storage=retained;modal_detail::data_charge(data,storage);
    if(request.light_table)light_table_charge(*request.light_table,storage);
    modal_detail::charge(storage,std::uint64_t((width+255)/256)*((height+255)/256)*65536*4,modal_detail::max_bytes,"Motion result exceeds aggregate512MiB");
    Raster result(width,height,32,data.base.dpi);const auto bg=color(settings.background);
    // Explicitly define the original unchecked empty-intersection case as a
    // background frame. The ordinary intersecting domain follows68C6D0.
    const int left=std::max(0,area.x),top=std::max(0,area.y),right=int(std::min<std::int64_t>(data.base.width(),std::int64_t(area.x)+area.width)),bottom=int(std::min<std::int64_t>(data.base.height(),std::int64_t(area.y)+area.height));
    std::uint64_t work=std::uint64_t(width)*height+std::uint64_t(std::max(0,right-left))*std::max(0,bottom-top);if(work>modal_detail::max_work)throw std::length_error("Motion frame work exceeds128Mi");
    modal_detail::charge(storage,std::uint64_t(std::max(0,right-left))*std::max(0,bottom-top)*sizeof(PreviewPixel),modal_detail::max_bytes,"Motion source cache exceeds aggregate512MiB");
    for(int y=0;y<height;++y){poll(stop);for(int x=0;x<width;++x)result.set_pixel(x,y,bg);}
    if(right<=left||bottom<=top)return result;
    PreviewOptions options;options.motion_check=true;options.motion_show_shadow=settings.show_shadow_layer;options.motion_only_line=request.only_line;options.resources={storage,work};
    options.motion_qt_text_fonts=request.text_fonts==MotionTextFonts::Qt;options.motion_stop=stop;
    const auto cache=PreviewScene(data,nullptr,{},options).render_layers({left,top,right-left,bottom-top});
    const auto resolved=[&](int x,int y){const auto c=cache.pixel(x,y);auto p=Pixel::rgba8(c.r,c.g,c.b,c.a);
        if(settings.show_light_table&&request.light_table&&x<request.light_table->width&&y<request.light_table->height)p=vector_render_detail::output_terminal::behind_white(p,request.light_table->pixel(x,y),32,false);
        return vector_render_detail::output_terminal::finish(p,32,bg,false);
    };
    // The bitmap cache is assembled independently for every physical source
    // tile intersection. Rounding and12-bit bins restart at each tile.
    for(int ty=top/256;ty<=(bottom-1)/256;++ty)for(int tx=left/256;tx<=(right-1)/256;++tx){poll(stop);
        const int l=std::max(left,tx*256),t=std::max(top,ty*256),r=std::min(right,(tx+1)*256),b=std::min(bottom,(ty+1)*256);
        const int dl=*legacy_rt_mul_div(l-left,width,right-left),dr=*legacy_rt_mul_div(r-left,width,right-left),dt=*legacy_rt_mul_div(t-top,height,bottom-top),db=*legacy_rt_mul_div(b-top,height,bottom-top);
        const int sw=r-l,sh=b-t,dw=dr-dl,dh=db-dt;if(dw<=0||dh<=0)continue;
        if(dw<sw){const auto sx=std::int64_t(sw)*4096/dw,sy=std::int64_t(sh)*4096/dh;
            for(int y=0;y<dh;++y){poll(stop);for(int x=0;x<dw;++x){const int a=int(x*sx/4096),z=int((x+1)*sx/4096),c=int(y*sy/4096),v=int((y+1)*sy/4096);if(z<=a||v<=c)continue;
                unsigned red{},green{},blue{},count{};for(int yy=c;yy<std::min(v,sh);++yy)for(int xx=a;xx<std::min(z,sw);++xx){const auto p=resolved(l+xx,t+yy);red+=p.r>>8;green+=p.g>>8;blue+=p.b>>8;++count;}
                if(count)result.set_pixel(dl+x,dt+y,Pixel::rgba8(red/count,green/count,blue/count));
            }}
        }else{const auto xs=stretch_indices(sw,dw),ys=stretch_indices(sh,dh);for(int y=0;y<dh;++y){poll(stop);for(int x=0;x<dw;++x)result.set_pixel(dl+x,dt+y,resolved(l+xs[x],t+ys[y]));}}
    }
    return result;
}
Raster prepare_motion_check_frame(const DocumentData& data,const MotionFrameRequest& request,std::stop_token stop){return prepare_frame(data,request,stop,0);}
MotionCheckSession::MotionCheckSession(CelSequence& sequence,MotionFrameRequest request,MotionCheckRange range,std::stop_token stop):sequence_(sequence),request_(std::move(request)),live_index_(sequence.index()){
    validate(request_.settings);if(request_.light_table){light_table_charge(*request_.light_table,storage_);auto copy=std::make_shared<Raster>(*request_.light_table);for(auto& [key,bytes]:copy->tiles){(void)key;poll(stop);bytes=std::make_shared<Bytes>(*bytes);}request_.light_table=std::move(copy);}const auto count=sequence.paths().size();if(count<2)throw std::invalid_argument("CEL MotionCheck requires at least two frames");if(range.last==0)range.last=int(count);
    if(range.first<1||range.last<1||range.first>=range.last||std::size_t(range.last)>count)throw std::invalid_argument("CEL MotionCheck requires a strict inclusive range");
    first_=std::size_t(range.first-1);last_=std::size_t(range.last-1);index_=first_;cache_.resize(count);
    // Motion Text always rebuilds from frozen raw payload. Do not retain an
    // uncharged, externally aliased UI/font cache through the modal session.
    modal_detail::data_charge(sequence.document().data,storage_);live_=modal_detail::frozen(sequence.document().data,stop,0,false);
    if(request_.fixed_area.width==0&&request_.fixed_area.height==0){if(request_.settings.selected_area&&sequence.document().region_selection.active())request_.fixed_area=sequence.document().region_selection.document_bounds();else request_.fixed_area={0,0,live_.base.width(),live_.base.height()};}
}
std::shared_ptr<const Raster> MotionCheckSession::frame(std::size_t index,std::stop_token stop){
    if(cache_[index])return cache_[index];poll(stop);std::optional<BatchDecodedImage> loaded;
    if(index!=live_index_)loaded=sequence_.load_frame(index,stop);
    DocumentData missing;missing.base=live_.base;auto frame_request=request_;if(index!=live_index_&&!loaded){frame_request.settings.show_all=0;frame_request.settings.show_light_table=0;frame_request.light_table.reset();}
    Raster raster=prepare_frame(index==live_index_?live_:loaded?loaded->data:missing,frame_request,stop,storage_);
    std::uint64_t bytes=sizeof(Raster);for(const auto& [tile,backing]:raster.tiles){(void)tile;bytes+=backing->size()+128;}
    if(bytes>512ull*1024*1024-storage_)throw std::length_error("MotionCheck session cache exceeds512MiB");auto cached=std::make_shared<const Raster>(std::move(raster));storage_+=bytes;
    return cache_[index]=std::move(cached);
}
MotionCheckEvent MotionCheckSession::tick(std::int64_t now,std::span<const int> commands,std::stop_token stop){
    Entry guard(entered_);try{poll(stop);if(closed_)throw std::logic_error("Closed MotionCheck session");if(now<0||(clock_started_&&now<last_clock_)||now>std::numeric_limits<std::int64_t>::max()-3600001)throw std::invalid_argument("MotionCheck clock must be monotonic milliseconds");last_clock_=now;clock_started_=true;
    bool changed=!started_;for(const auto command:commands){if(command==27){exited_=true;break;}if(warming_||blank_)continue;
        switch(command){case 1:case 3:index_=index_==first_?last_:index_-1;playing_=false;changed=true;break;case 2:case 4:index_=index_==last_?first_:index_+1;playing_=false;changed=true;break;case 5:index_=first_;playing_=false;changed=true;break;case 6:index_=last_;playing_=false;changed=true;break;case 7:if(!playing_){index_=index_==last_?first_:index_+1;changed=true;}playing_=true;deadline_=now+1000/fps[request_.settings.fps_index];break;case 8:playing_=false;break;case 9:hud_=!hud_;break;case 10:exited_=true;break;case 32:if(!playing_){index_=index_==last_?first_:index_+1;changed=true;}playing_=!playing_;deadline_=now+1000/fps[request_.settings.fps_index];break;default:if(command>=11&&command<=16){request_.settings.fps_index=command-11;playing_=true;deadline_=now+1000/fps[request_.settings.fps_index];}break;}
    }
    if(!exited_&&started_&&playing_&&now>=deadline_){
        if(blank_){blank_=false;index_=first_;changed=true;}
        else if(index_==last_){const bool first_cycle=warming_;warming_=false;if(!first_cycle&&request_.settings.pause&&request_.settings.pause_time){blank_=true;deadline_=now+std::int64_t(request_.settings.pause_time)*1000;}else{index_=first_;changed=true;}}
        else{++index_;changed=true;}
    }
    if(!exited_&&changed){deadline_=now+1000/fps[request_.settings.fps_index];(void)frame(index_,stop);started_=true;}
    return {index_,started_?cache_[index_]:nullptr,blank_,exited_,playing_,hud_,warming_,deadline_};
    }catch(...){closed_=true;exited_=true;throw;}
}
std::size_t MotionCheckSession::cached_frames()const noexcept{return std::count_if(cache_.begin(),cache_.end(),[](const auto& p){return bool(p);});}
PrefArchive MotionCheckSession::close(std::span<const PrefRecord> old){Entry guard(entered_);if(closed_)throw std::logic_error("Closed MotionCheck session");auto result=store_motion_check_preferences(request_.settings,old);closed_=true;exited_=true;return result;}
}
