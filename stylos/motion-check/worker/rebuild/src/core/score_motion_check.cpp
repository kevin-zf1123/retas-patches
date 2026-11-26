#include "retas/score_motion_check.hpp"
#include "retas/score_sound_playback.hpp"
#include "retas/preview.hpp"
#include "retas/stylos_pixel.hpp"
#include "batch_text.hpp"
#include "retas/numeric.hpp"
#include "retas/mask_geometry.hpp"
#include "modal_document_guard.hpp"
#include "vector_line_output.hpp"
#include "overlay_lengths.hpp"
#include "../io/native_ascii_string.hpp"
#include <algorithm>
#include <bit>
#include <fstream>
#include <map>
#include <set>

namespace retas {
ScoreMotionAudio make_score_motion_audio(ScoreSoundPlayer& player,std::optional<ScoreSoundBinding> binding,std::optional<std::uint32_t> fixed){
    return {[&player,binding=std::move(binding),fixed](const ScoreMotionAudioAction& action){
        if(action.kind==ScoreMotionAudioAction::Kind::Stop){if(!action.only_if_playing||player.is_playing())player.stop();return;}
        if(!binding)throw std::logic_error("Score MotionCheck has no sound binding");
        if(action.mode<0||action.mode>1||action.start_ticks<0||action.end_ticks<0)throw std::invalid_argument("Invalid Score MotionCheck audio request");
        player.play(*binding,{std::uint16_t(action.mode),std::uint32_t(action.start_ticks),std::uint32_t(action.end_ticks),fixed});
    }};
}
namespace {
constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
struct Budget {
    std::uint64_t bytes{},work{};
    std::stop_token stop;
    void visit(std::uint64_t n){modal_detail::poll(stop);modal_detail::charge(work,n,max_work,"Score MotionCheck exceeds128Mi work");}
    void allocate(std::uint64_t n){modal_detail::charge(bytes,n,max_bytes,"Score MotionCheck exceeds512MiB");}
};
struct Entry {bool& busy;explicit Entry(bool& b):busy(b){if(b)throw std::logic_error("Reentrant Score MotionCheck");busy=true;}~Entry(){busy=false;}};
std::string prefix(std::span<const std::uint8_t> b){const auto p=native_ascii_detail::c_prefix(b);return {p.begin(),p.end()};}
std::string fold(std::string s){for(auto& c:s)if(c>='A'&&c<='Z')c+=32;return s;}
std::string extension(const std::filesystem::path& p){auto s=p.extension().string();if(!s.empty())s.erase(s.begin());return fold(std::move(s));}
Pixel color(std::uint32_t c){return Pixel::rgba8(c&255,(c>>8)&255,(c>>16)&255);}
//4C5910 parses a label, not a filename: dots are not stripped. Reject the
// native fixed-buffer/atol overflow domain instead of reproducing corruption.
std::optional<std::uint32_t> number(std::string text,bool localized=false){
    text.resize(text.find('\0')==std::string::npos?text.size():text.find('\0'));
    if(text.size()>255)throw std::length_error("Score source number exceeds255 bytes");
    for(auto& c:text){if(!localized&&static_cast<unsigned char>(c)>=128)throw std::invalid_argument("Score source numbers require the native ASCII profile");if(c>='a'&&c<='z')c-=32;}
    auto end=text.size();while(end&&(text[end-1]<'0'||text[end-1]>'9'))--end;
    if(!end)return {};auto begin=end;while(begin&&text[begin-1]>='0'&&text[begin-1]<='9')--begin;
    std::uint32_t value{};for(auto i=begin;i<end;++i){const auto digit=unsigned(text[i]-'0');if(value>(2147483647u-digit)/10)throw std::out_of_range("Score source number exceeds int32");value=value*10+digit;}
    if(!value)return {};value=std::min(value,6000u);
    const auto valid=[](char c){return (c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='+'||c=='-';};
    const unsigned a=end<text.size()&&valid(text[end])?unsigned(text[end]):0;
    const unsigned b=a&&end+1<text.size()&&valid(text[end+1])?unsigned(text[end+1]):0;
    return a?(value<<16)|(b<<8)|a:value;
}
BaseInfo paper(const Mn6BaseInfo& m){
    BaseInfo b;b.dpi=std::bit_cast<double>(m.dpi_bits);
    for(std::size_t i=0;i<16;++i)b.lengths[i]={std::bit_cast<double>(m.lengths[i].value_bits),std::bit_cast<std::int32_t>(m.lengths[i].unit),std::bit_cast<double>(m.lengths[i].dpi_bits)};
    return b;
}
int pixels(const BaseInfo& b,std::size_t i){return overlay_detail::pixels(b.lengths[i],b.dpi);}
void extent(int w,int h){if(w<1||h<1||w>20000||h>20000||std::uint64_t(w)*h>64ull*1024*1024)throw std::invalid_argument("Score MotionCheck extent exceeds supported range");}
std::uint64_t raster_storage(int w,int h){return std::uint64_t((w+255)/256)*((h+255)/256)*65536*4+sizeof(Raster)+std::uint64_t((w+255)/256)*((h+255)/256)*128;}
int scaled(int n,double s){const auto v=legacy_rt_double_to_int(numeric_detail::binary64(numeric_detail::binary64(n*s)*.01));if(!v)throw std::invalid_argument("Invalid Score MotionCheck scale");return *v;}
void settings_valid(const MotionCheckSettings& s){if(!std::isfinite(s.scale)||s.scale<=0||s.scale>10000||s.pause_time<0||s.pause_time>3600)throw std::invalid_argument("Invalid Score MotionCheck scale/pause");}
std::vector<int> stretch(int source,int destination){std::vector<int> indices(std::size_t(destination),0);int at{},error=destination;for(auto& i:indices){i=at;at+=source/destination;error-=source%destination;if(error<0){error+=destination;++at;}}return indices;}
//68C6D0, crop=null: complement cache -> explicit background -> per-physical
//256-tile integer reduction/stretch. Scale>=100 affects presentation only.
Raster bitmap(const Raster& raw,const MotionCheckSettings& settings,ScoreMotionRenderer renderer,Budget& budget){
    const int w=settings.scale<100?scaled(raw.width,settings.scale):raw.width,h=settings.scale<100?scaled(raw.height,settings.scale):raw.height;extent(w,h);
    budget.allocate(raster_storage(w,h));budget.visit(std::uint64_t(raw.width)*raw.height+std::uint64_t(w)*h);
    Raster result(w,h,32,raw.resolution);const auto background=color(settings.background);
    const auto sample=[&](int x,int y){
        const auto pixel=raw.pixel(x,y);
        if(renderer!=ScoreMotionRenderer::Stylos260)
            return vector_render_detail::output_terminal::finish(pixel,32,background,false);
        const unsigned a=pixel.a>>8;
        if(!a)return background;
        const auto channel=[&](unsigned value,unsigned bg){
            return unsigned(std::max(0,int(value>>8)-int(((255-a)*(255-(bg>>8)))>>8)));
        };
        return Pixel::rgba8(channel(pixel.r,background.r),channel(pixel.g,background.g),channel(pixel.b,background.b));
    };
    for(int y=0;y<h;++y){modal_detail::poll(budget.stop);for(int x=0;x<w;++x)result.set_pixel(x,y,background);}
    for(int t=0;t<raw.height;t+=256)for(int l=0;l<raw.width;l+=256){
        const int r=std::min(l+256,raw.width),b=std::min(t+256,raw.height);
        const int dl=*legacy_rt_mul_div(l,w,raw.width),dr=*legacy_rt_mul_div(r,w,raw.width),dt=*legacy_rt_mul_div(t,h,raw.height),db=*legacy_rt_mul_div(b,h,raw.height);
        const int sw=r-l,sh=b-t,dw=dr-dl,dh=db-dt;if(dw<=0||dh<=0)continue;
        if(dw<sw){const auto sx=std::int64_t(sw)*4096/dw,sy=std::int64_t(sh)*4096/dh;
            for(int y=0;y<dh;++y){modal_detail::poll(budget.stop);for(int x=0;x<dw;++x){unsigned red{},green{},blue{},count{};
                for(int yy=int(y*sy/4096);yy<std::min<int>(int((y+1)*sy/4096),sh);++yy)for(int xx=int(x*sx/4096);xx<std::min<int>(int((x+1)*sx/4096),sw);++xx){const auto p=sample(l+xx,t+yy);red+=p.r>>8;green+=p.g>>8;blue+=p.b>>8;++count;}
                if(count)result.set_pixel(dl+x,dt+y,Pixel::rgba8(red/count,green/count,blue/count));
            }}
        }else{const auto xs=stretch(sw,dw),ys=stretch(sh,dh);for(int y=0;y<dh;++y){modal_detail::poll(budget.stop);for(int x=0;x<dw;++x)result.set_pixel(dl+x,dt+y,sample(l+xs[x],t+ys[y]));}}
    }
    return result;
}
}
struct ScoreMotionCheckSession::State {
    struct SourceGroup {
        std::filesystem::path path;
        std::map<std::string,std::filesystem::path> exact;
        std::map<std::uint32_t,std::filesystem::path> numeric;
    };
    struct Column {std::optional<std::size_t> group;std::vector<std::string> labels;};
    MotionCheckSettings settings;
    ScoreMotionRenderer renderer{ScoreMotionRenderer::PaintMan};
    BatchImageCodecs codecs;
    ScoreMotionAudio sink;
    std::vector<SourceGroup> groups;
    std::vector<Column> columns;
    std::vector<std::size_t> ids,representatives;
    std::vector<std::shared_ptr<const Raster>> caches;
    std::vector<Bytes> voices;
    std::vector<ScoreMotionAudioAction> actions;
    std::uint64_t retained{};
    std::size_t first{},last{},index{};
    int score_fps{},fps{},width{},height{};
    double dpi{};
    std::int64_t last_clock{},epoch{},blank_end{};
    bool sound_bound{},busy{},clock_started{},started{},playing{true},hud{true},warming{true},blank{},exited{},stopped{};
    ~State(){try{stop();}catch(...){}}
    void audio(ScoreMotionAudioAction a){actions.push_back(a);if(sink.dispatch)sink.dispatch(a);}
    void play(){if(settings.play_sound&&sound_bound){const auto time=[&](std::size_t f){return std::int32_t(numeric_detail::binary64(double(f)*numeric_detail::binary64(600.0/score_fps)));};audio({ScoreMotionAudioAction::Kind::Play,1,time(index),time(last),false});}}
    void pause(){if(settings.play_sound&&sound_bound)audio({ScoreMotionAudioAction::Kind::Stop,1,0,0,true});}
    void stop(){if(stopped)return;stopped=true;audio({});}
    std::size_t relative()const{return index-first;}
    std::size_t next(std::size_t i,bool forward)const{return forward?(i==last?first:i+1):(i==first?last:i-1);}
    std::optional<std::filesystem::path> source(const Column& column,std::size_t frame,Budget& budget)const{
        budget.visit(1);if(!column.group||column.labels[frame-first].empty())return {};
        const auto& group=groups[*column.group];const auto& label=column.labels[frame-first];
        if(settings.show_score_layer==1){const auto found=group.exact.find(label);if(found!=group.exact.end())return found->second;}
        else if(const auto n=number(label,renderer==ScoreMotionRenderer::Stylos260)){const auto found=group.numeric.find(*n);if(found!=group.numeric.end())return found->second;}
        else return {}; // An unparseable SECOND-bank label is skipped natively.
        return std::filesystem::path{}; // matched group, missing entry
    }
    std::optional<BatchDecodedImage> load(const std::filesystem::path& path,Budget& budget,const SourceVisitor& visit={})const{
        modal_detail::poll(budget.stop);const auto status=std::filesystem::symlink_status(path);
        if(status.type()==std::filesystem::file_type::not_found)return {};
        if(!std::filesystem::is_regular_file(status))throw std::invalid_argument("Score MotionCheck source must be a direct regular file");
        std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open Score MotionCheck source");
        const auto size=file.tellg();if(size<0||size>std::streamoff(max_bytes))throw std::length_error("Score MotionCheck source exceeds512MiB");
        budget.allocate(std::uint64_t(size));budget.visit(std::uint64_t(size));Bytes bytes(static_cast<std::size_t>(size));file.seekg(0);
        if(!file.read(reinterpret_cast<char*>(bytes.data()),size))throw std::runtime_error("Short Score MotionCheck source read");
        if(visit)visit(path,bytes);
        const auto ext=extension(path);const auto* codec=codecs.find(ext);if(!codec||!codec->decode)throw std::invalid_argument("No Score MotionCheck source decoder");
        auto image=codec->decode(ext,bytes,budget.stop);modal_detail::poll(budget.stop);modal_detail::data_charge(image.data,budget.bytes);
        const auto& data=image.data;extent(data.base.width(),data.base.height());if(!std::isfinite(data.base.dpi)||data.base.dpi<=0||(renderer!=ScoreMotionRenderer::Stylos260&&data.has_unrendered_content())||data.unrestored_external_content)throw std::invalid_argument("Unsupported Score MotionCheck source content");
        for(const auto& layer:data.layers)for(const auto& plane:layer.planes)if(plane.raster){const auto& r=*plane.raster;
            const bool supported=(r.depth==1||r.depth==2||r.depth==8||r.depth==16)?r.color_order==0:
                r.depth==24?r.color_order==4:(r.depth==32||r.depth==64)&&r.color_order==3;
            if(!supported||r.width<1||r.height<1||r.width>20000||r.height>20000||!std::isfinite(r.resolution)||r.resolution<=0)throw std::invalid_argument("Invalid Score MotionCheck raster");
            for(const auto& [key,tile]:r.tiles)if(key.x<0||key.y<0||std::int64_t(key.x)*256>=r.width||std::int64_t(key.y)*256>=r.height||!tile||tile->size()!=r.tile_bytes())throw std::invalid_argument("Invalid Score MotionCheck raster tile");
        }
        return image;
    }
    Raster render(std::size_t frame_index,const SourceVisitor& visitor,std::stop_token stop)const{
        Budget budget{retained,0,stop};extent(width,height);budget.allocate(raster_storage(width,height));Raster raw(width,height,32,dpi);bool missing{};
        for(auto it=columns.rbegin();it!=columns.rend();++it){
            const auto path=source(*it,frame_index,budget);if(!path)continue;if(path->empty()){missing=true;continue;}
            const auto loaded=load(*path,budget,visitor);if(!loaded){missing=true;continue;}const auto& data=loaded->data;
            Rect area{0,0,data.base.width(),data.base.height()};
            if(!settings.show_all){const auto fw=pixels(data.base,4),fh=pixels(data.base,5);const auto x=std::int64_t(pixels(data.base,2))-fw/2,y=std::int64_t(pixels(data.base,3))-fh/2;
                if(fw<1||fh<1||x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX)throw std::invalid_argument("Invalid Score source Frame rectangle");
                area={int(x),int(y),fw,fh};
            }
            const int w=std::min(area.width,width),h=std::min(area.height,height);budget.visit(std::uint64_t(w)*h);
            // Native out-of-page read cursors return transparent black. Only
            // the in-page intersection can contribute to the white cache.
            const int left=std::max(0,area.x),top=std::max(0,area.y),right=int(std::min<std::int64_t>(data.base.width(),std::int64_t(area.x)+w)),bottom=int(std::min<std::int64_t>(data.base.height(),std::int64_t(area.y)+h));
            if(right<=left||bottom<=top)continue;
            budget.allocate(std::uint64_t(right-left)*(bottom-top)*sizeof(PreviewPixel));
            PreviewOptions options;options.motion_check=true;options.stylos_score_pixels=renderer==ScoreMotionRenderer::Stylos260;options.motion_show_shadow=settings.show_shadow_layer;options.resources={budget.bytes,budget.work};
            std::array<std::uint64_t,2> admission;
            const auto rendered=PreviewScene(data,nullptr,{},options).render_layers({left,top,right-left,bottom-top},admission);
            budget.bytes=admission[0];budget.work=admission[1];
            for(int y=top;y<bottom;++y){modal_detail::poll(stop);for(int x=left;x<right;++x){auto s=rendered.pixel(x,y);if(!s.a)continue;
                if(renderer==ScoreMotionRenderer::Stylos260){
                    const auto straight=vector_render_detail::byte_sink::straight(Pixel::rgba8(s.r,s.g,s.b,s.a));
                    s={std::uint8_t(straight.r>>8),std::uint8_t(straight.g>>8),std::uint8_t(straight.b>>8),s.a};
                }
                const auto p=raw.pixel(x-area.x,y-area.y);PreviewPixel d{std::uint8_t(p.r>>8),std::uint8_t(p.g>>8),std::uint8_t(p.b>>8),std::uint8_t(p.a>>8)};
                if(settings.darken)d={std::min(d.r,s.r),std::min(d.g,s.g),std::min(d.b,s.b),std::max(d.a,s.a)};
                else if(renderer==ScoreMotionRenderer::Stylos260)d=stylos260::behind(d,s);
                else if(d.a!=255)d=!d.a&&s.a==255?s:preview_behind(d,s);
                raw.set_pixel(x-area.x,y-area.y,Pixel::rgba8(d.r,d.g,d.b,d.a));
            }}
        }
        if(missing){
            // RCVOffscreen::DrawSegment width2/mode0 produces these six
            // integer vertices, then FillPolygon(flag1), without round dabs.
            budget.allocate(std::uint64_t((width+255)/256)*((height+255)/256)*8192);
            budget.visit(std::uint64_t(width)*height*3);BinaryMask mask(width,height);
            const std::array<MaskPoint,6> a{{{0,0},{1,0},{width+1,height},{width+1,height+1},{width,height+1},{0,1}}};
            const std::array<MaskPoint,6> b{{{0,height},{width,0},{width+1,0},{width+1,1},{1,height+1},{0,height+1}}};
            mask_polygon(mask,a,height+2,PolygonEdgeMode::Inclusive);mask_polygon(mask,b,height+2,PolygonEdgeMode::Inclusive);
            for(int y=0;y<height;++y){modal_detail::poll(stop);for(int x=0;x<width;++x)if(mask.test(x,y))raw.set_pixel(x,y,renderer==ScoreMotionRenderer::Stylos260?Pixel::rgba8(255,0,0):Pixel::rgba8(0,0,255));}
        }
        return bitmap(raw,settings,renderer,budget);
    }
    std::shared_ptr<const Raster> frame(std::stop_token stop){
        const auto id=ids[relative()];
        if(!caches[id]){
            auto result=render(index,{},stop);
            modal_detail::charge(retained,raster_storage(result.width,result.height),max_bytes,"Score MotionCheck cache exceeds512MiB");
            caches[id]=std::make_shared<const Raster>(std::move(result));
        }
        return caches[id];
    }
};
ScoreMotionSettingsResult accept_score_motion_settings(int result,bool enabled,const MotionCheckSettings& settings,std::span<const PrefRecord> old){
    if(result==2)return {PrefArchive(old.begin(),old.end()),false,false};
    settings_valid(settings);return {store_motion_check_preferences(settings,old),true,result==1219&&enabled};
}
ScoreMotionCheckSession::ScoreMotionCheckSession(const ScoreDocument& document,const CutFolderRoot& root,MotionCheckSettings settings,ScoreMotionRange range,BatchImageCodecs codecs,ScoreMotionAudio sink,std::stop_token stop,ScoreMotionRenderer renderer){
    settings_valid(settings);Budget budget{0,0,stop};modal_detail::poll(stop);const auto& data=document.data();validate_score(data);
    if(!data.general.fps)throw std::invalid_argument("Score MotionCheck requires positive Score FPS");
    if(range.first==0&&range.last==0)range.last=data.general.frame_count-1;
    if(range.first<0||range.first>range.last||range.last>=data.general.frame_count)throw std::out_of_range("Invalid inclusive Score MotionCheck range");
    auto state=std::make_unique<State>();auto& s=*state;s.settings=settings;s.renderer=renderer;s.first=std::size_t(range.first);s.last=std::size_t(range.last);s.index=s.first;s.score_fps=s.fps=data.general.fps;s.sound_bound=document.sound_binding().has_value();
    budget.allocate(codecs.storage());s.codecs=std::move(codecs);s.sink=std::move(sink);
    const auto base=paper(root.archive().info.base);s.dpi=base.dpi;if(!std::isfinite(s.dpi)||s.dpi<=0)throw std::invalid_argument("Invalid Score CutFolder DPI");s.width=pixels(base,4);s.height=pixels(base,5);
    std::map<std::string,std::size_t> group_names;
    if(settings.show_score_layer==1||settings.show_score_layer==2){
        // Native BG/common precedes Key/Inbetween; Layout is never searched.
        // Kind2 stores every child in Inbetween, including names-BG/-LO.
        const auto append=[&](std::uint32_t mask){
            for(const auto& view:root.child_folders(mask)){budget.visit(1);const auto name=fold(renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::utf8(view.path.filename()):view.path.filename().string());if(group_names.contains(name))continue;
                State::SourceGroup group;group.path=view.path;budget.allocate(sizeof(group)+name.size()+view.path.native().size()*sizeof(std::filesystem::path::value_type));
                for(const auto& entry:view.owner.index().entries){budget.visit(1);const auto file=renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::host_path(entry.filename,root.metadata_encoding()):std::filesystem::path(entry.filename);const auto p=view.path/file;
                    if(entry.filename.empty()||entry.filename=="."||entry.filename==".."||(renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::utf8(file):entry.filename).find_first_of("/\\:")!=std::string::npos)throw std::invalid_argument("Unsafe Score CF06 filename");
                    budget.allocate(entry.filename.size()*4+entry.number.size()*2+sizeof(std::filesystem::path)*2+256);
                    const auto label=renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::decode(entry.number,root.metadata_encoding()):entry.number;
                    if(settings.show_score_layer==1)group.exact.try_emplace(label,p);
                    else if(const auto n=number(label,renderer==ScoreMotionRenderer::Stylos260))group.numeric.try_emplace(*n,p);
                }
                group_names.emplace(name,s.groups.size());s.groups.push_back(std::move(group));
            }
        };
        if(root.archive().info.kind==1){append(2);append(settings.show_score_layer==1?4:8);}
        else if(root.archive().info.kind==2&&settings.show_score_layer==2)append(8);
    }
    const auto count=s.last-s.first+1;budget.allocate(count*(sizeof(Bytes)+sizeof(std::size_t)*3+sizeof(std::shared_ptr<const Raster>)));
    s.voices.resize(count);std::string held_voice;
    for(std::size_t f=0;f<=s.last;++f){budget.visit(1);const auto text=prefix(std::span(data.voice[f]).first(6));if(!text.empty())held_voice=text;if(f>=s.first&&settings.show_voice)s.voices[f-s.first]=Bytes(held_voice.begin(),held_voice.end());}
    const auto& bank=settings.show_score_layer==1?data.primary_layers:data.derived_layers;
    for(const auto& layer:bank){if(!layer.field)continue;State::Column column;budget.allocate(count*(sizeof(std::string)+8));column.labels.resize(count);
        const auto projected=native_ascii_detail::decode_string(layer.name,budget,renderer!=ScoreMotionRenderer::Stylos260);
        const auto name=renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::decode(prefix(projected),root.metadata_encoding()):prefix(projected);
        const auto found=group_names.find(fold(name));if(found!=group_names.end())column.group=found->second;
        std::string held;for(std::size_t f=0;f<=s.last;++f){budget.visit(1);const auto& cell=layer.cells[f];if(cell.marker&1)held=renderer==ScoreMotionRenderer::Stylos260?batch_text_detail::decode(prefix(cell.label),root.metadata_encoding()):prefix(cell.label);else if(cell.marker&8)held.clear();if(f>=s.first)column.labels[f-s.first]=held;}s.columns.push_back(std::move(column));
    }
    std::map<std::vector<std::string>,std::size_t> cache_ids;s.ids.reserve(count);
    for(std::size_t f=0;f<count;++f){std::vector<std::string> key;for(const auto& c:s.columns){budget.visit(1);key.push_back(fold(c.labels[f]));}
        auto found=cache_ids.find(key);if(found==cache_ids.end()){budget.allocate(key.size()*(sizeof(std::string)+8)+128);const auto id=cache_ids.size();cache_ids.emplace(std::move(key),id);s.representatives.push_back(f+s.first);s.ids.push_back(id);}else s.ids.push_back(found->second);
    }
    s.caches.resize(cache_ids.size());
    if(settings.show_all){int w{},h{};std::set<std::filesystem::path> seen;
        for(std::size_t f=s.first;f<=s.last;++f)for(const auto& column:s.columns){const auto p=s.source(column,f,budget);if(!p||p->empty()||!seen.insert(*p).second)continue;const auto loaded=s.load(*p,budget);if(loaded){w=std::max(w,loaded->data.base.width());h=std::max(h,loaded->data.base.height());}}
        if(w&&h){s.width=w;s.height=h;}
    }
    extent(s.width,s.height);const auto w=settings.scale<100?scaled(s.width,settings.scale):s.width,h=settings.scale<100?scaled(s.height,settings.scale):s.height;extent(w,h);
    s.retained=budget.bytes;modal_detail::poll(stop);state_=std::move(state);
}
Raster ScoreMotionCheckSession::render_frame(std::size_t frame,const SourceVisitor& visitor,std::stop_token stop)const{
    if(frame<state_->first||frame>state_->last)throw std::out_of_range("Score render frame outside range");
    return state_->render(frame,visitor,stop);
}
std::span<const std::size_t> ScoreMotionCheckSession::unique_frames()const noexcept{return state_->representatives;}
std::array<int,2> ScoreMotionCheckSession::unscaled_size()const noexcept{return {state_->width,state_->height};}
std::size_t ScoreMotionCheckSession::representative(std::size_t frame)const{
    if(frame<state_->first||frame>state_->last)throw std::out_of_range("Score frame outside range");
    return state_->representatives[state_->ids[frame-state_->first]];
}
ScoreMotionCheckSession::~ScoreMotionCheckSession(){if(state_)try{state_->stop();}catch(...){}}
ScoreMotionEvent ScoreMotionCheckSession::tick(std::int64_t now,std::span<const int> commands,std::stop_token stop){
    auto& s=*state_;Entry guard(s.busy);s.actions.clear();
    try{
        modal_detail::poll(stop);if(s.stopped)throw std::logic_error("Closed Score MotionCheck");
        if(now<0||(s.clock_started&&now<s.last_clock)||now>INT64_MAX-3600001)throw std::invalid_argument("Score MotionCheck requires a monotonic millisecond clock");
        s.last_clock=now;if(!s.clock_started){s.clock_started=true;s.epoch=now;}
        bool changed=!s.started,explicit_display{};
        for(int command:commands){
            if(command==27)command=10;if(command==10){s.exited=true;break;}
            if(s.warming||s.blank)continue;if(command==32)command=s.playing?8:7;
            if(command<1||command>17)continue;
            explicit_display=true;const bool was_playing=s.playing;
            // A playing display phase samples the input first, but its
            // first-frame audio request precedes the transition to paused.
            if(was_playing&&s.index==s.first)s.play();
            if(command>=1&&command<=6){
                const auto previous=s.index;
                if(!was_playing){
                    if(command==1||command==2){const auto id=s.ids[s.relative()];do{s.index=s.next(s.index,command==2);}while(s.index!=previous&&s.ids[s.relative()]==id);}
                    else if(command==3||command==4)s.index=s.next(s.index,command==4);
                    else s.index=command==5?s.first:s.last;
                    changed=true;
                }
                s.playing=false;
            }else if(command==7){if(!s.playing){s.index=s.next(s.index,true);changed=true;s.play();}s.playing=true;}
            else if(command==8)s.playing=false;
            else if(command==9)s.hud=!s.hud;
            else if(command>=11&&command<=17){constexpr int rates[]{30,25,24,12,10,8};s.fps=command==17?s.score_fps:rates[command-11];s.playing=true;}
            if(!was_playing&&s.playing&&s.index==s.first)s.play();
            if(!s.playing)s.pause();
        }
        if(!s.exited&&s.started&&s.playing&&!explicit_display){
            if(s.blank){if(now>=s.blank_end){s.blank=false;s.warming=false;s.index=s.first;s.epoch=now;changed=true;}}
            else if(now-s.epoch>=1000/s.fps){s.epoch=now;if(s.index==s.last){if(s.settings.pause){s.blank=true;s.blank_end=now+std::int64_t(s.settings.pause_time)*1000;}else{s.warming=false;s.index=s.first;changed=true;}}else{s.index=s.next(s.index,true);changed=true;}}
        }
        if(s.exited)s.stop();
        else {if(changed&&!explicit_display&&s.playing&&!s.warming&&!s.blank&&s.index==s.first)s.play();if(changed&&!s.blank){(void)s.frame(stop);s.started=true;}}
        ScoreMotionEvent event;event.frame_index=s.index;event.cache_id=s.ids[s.relative()]+1;event.frame=s.started?s.caches[event.cache_id-1]:nullptr;event.voice=s.voices[s.relative()];event.audio=std::move(s.actions);event.blank=s.blank;event.exited=s.exited;event.playing=s.playing;event.hud=s.hud;event.warming=s.warming;event.fps=s.fps;event.deadline_ms=s.blank?s.blank_end:s.epoch+1000/s.fps;return event;
    }catch(...){s.exited=true;try{s.stop();}catch(...){}throw;}
}
std::size_t ScoreMotionCheckSession::cached_frames()const noexcept{return std::count_if(state_->caches.begin(),state_->caches.end(),[](const auto& p){return bool(p);});}
const MotionCheckSettings& ScoreMotionCheckSession::settings()const noexcept{return state_->settings;}
std::vector<ScoreMotionAudioAction> ScoreMotionCheckSession::close(){auto& s=*state_;Entry guard(s.busy);s.actions.clear();s.exited=true;s.stop();return std::move(s.actions);}
}
