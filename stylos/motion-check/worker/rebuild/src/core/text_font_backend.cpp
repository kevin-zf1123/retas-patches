#include "retas/text_font_backend.hpp"
#include "retas/document.hpp"
#include "retas/legacy_text.hpp"
#include "retas/numeric.hpp"
#include "retas/text_tool.hpp"
#include "resolution_raster_internal.hpp"
#include "text_font_backend_internal.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#ifdef RETAS_HAS_QT_TEXT
#include <QFont>
#include <QFontMetrics>
#include <QDir>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>
#include <QThread>
#if defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#include <sys/syscall.h>
#include <unistd.h>
#endif
#endif

namespace retas {
#ifdef RETAS_HAS_QT_TEXT
namespace {
using resolution_detail::Budget;
void main_thread(){
    // Qt's main-thread id is initially unset before the first adopted thread.
    // Establish it only after checking the OS on standalone supported hosts.
#if defined(__APPLE__)
    if(!pthread_main_np())throw std::logic_error("Text font backend requires the main thread");
    (void)QThread::currentThread();
#elif defined(__linux__)
    if(syscall(SYS_gettid)!=getpid())throw std::logic_error("Text font backend requires the main thread");
    (void)QThread::currentThread();
#endif
    if(!QThread::isMainThread())throw std::logic_error("Text fonts require an initialized Qt application main thread");
}
struct Application {
    int argc{3};
    char name[21]{"paintman-text-font"},platform[10]{"-platform"},offscreen[10]{"offscreen"};
    char* argv[4]{name,platform,offscreen,nullptr};
    std::unique_ptr<QGuiApplication> owned;
    Application(){
        if(auto* app=QCoreApplication::instance()){
            if(!qobject_cast<QGuiApplication*>(app))throw std::logic_error("Text fonts require QGuiApplication, not QCoreApplication");
        }else {
#ifdef RETAS_TEXT_QT_PLUGIN_PATH
            QCoreApplication::addLibraryPath(QString::fromUtf8(RETAS_TEXT_QT_PLUGIN_PATH));
#endif
            bool found=false;for(const auto& path:QCoreApplication::libraryPaths())
                if(!QDir(path+"/platforms").entryList({"*offscreen*"},QDir::Files).isEmpty())found=true;
            if(!found)throw std::runtime_error("Qt offscreen platform plugin is unavailable");
            owned=std::make_unique<QGuiApplication>(argc,argv);
        }
    }
};
std::shared_ptr<Application> application(){
    main_thread();static std::weak_ptr<Application> shared;
    auto result=shared.lock();if(!result){result=std::make_shared<Application>();shared=result;}return result;
}
int coordinate(double value){
    if(!std::isfinite(value)||std::abs(value)>1000000)throw std::length_error("Text coordinate exceeds the bounded font profile");
    return int(value); // native __ftol2 truncates the DPI-mapped anchor
}
QString unicode(std::span<const std::uint8_t> bytes){return QString::fromStdString(decode_cp936(bytes));}
QColor color(std::uint32_t value){return QColor(value&255,(value>>8)&255,(value>>16)&255);}
Bytes runtime_string(const Bytes& wire,Budget& budget){
    // RCArchive::ReadString tests i+2 and moves strlen(i+1), excluding NUL.
    // Keep the complete retained wire until after this mutation: a CR may
    // expose bytes behind its original first NUL. Tail storage is zero-filled.
    Bytes result(wire.begin(),wire.end());result.resize(wire.size()+std::size_t(std::ranges::count(wire,'\r'))+1,0);
    std::size_t i{};for(;i<result.size()&&result[i];++i){budget.visit(1);if(result[i]!='\r')continue;
        if(i+2>=result.size())throw std::invalid_argument("Native Text string expansion exceeds retained backing");
        if(result[i+2]){const auto end=std::find(result.begin()+i+1,result.end(),0);const auto n=std::size_t(end-(result.begin()+i+1));
            if(n>result.size()-i-2)throw std::invalid_argument("Native Text string move exceeds retained backing");
            budget.visit(n*2);std::memmove(result.data()+i+2,result.data()+i+1,n);}
        result[i+1]='\n';}
    if(i==result.size())throw std::invalid_argument("Native Text string expansion is unterminated");
    result.resize(i);return result;
}
struct Candidate {Layer* owner;Plane* plane;TextParameters parameters;};
struct Rendered {Raster image;std::array<int,2> origin;std::array<int,4> coordinates{};std::shared_ptr<const text_font_detail::ExportMasks> masks;};
int font_pixels(const TextParameters& p,double dpi){
    using numeric_detail::binary64;
    // 640410 truncates DPI before 7437C0 calls RCLength(size,5).
    const auto pixels=legacy_rt_double_to_int(binary64(binary64(double(int(dpi))*p.font_size)*(1.0/72.0)));
    if(p.font_size<1||p.font_size>4096||!pixels||*pixels<1||*pixels>8192)throw std::length_error("Text point/pixel size exceeds the bounded font profile");
    return *pixels;
}
struct FontLayout {QFont font;std::vector<QString> lines;int width{},height{},line_height{},ascent{};};
FontLayout font_layout(const TextParameters& p,int pixels,std::span<const std::uint8_t> expanded,Budget& budget){
    const auto glyph_work=std::max<std::uint64_t>(1,expanded.size())*std::uint64_t(pixels)*std::uint64_t(pixels);
    budget.visit(glyph_work);budget.allocate(glyph_work*4); // conservative font-engine scratch admission
    QFont font(unicode(runtime_string(p.font_name,budget)));font.setPixelSize(pixels);font.setStyleStrategy(QFont::NoAntialias);
    const QFontMetrics metrics(font);
    std::vector<QString> lines;int width{},height{};std::size_t begin{};
    for(std::size_t at=0;at<=expanded.size();++at)if(at==expanded.size()||expanded[at]=='\n'){
        auto end=at;if(end>begin&&expanded[end-1]=='\r')--end;
        auto line=unicode(expanded.subspan(begin,end-begin));
        width=std::max(width,metrics.horizontalAdvance(line));height=coordinate(double(height)+metrics.height());
        lines.push_back(std::move(line));begin=at+1;budget.poll();
    }
    return {std::move(font),std::move(lines),width,height,metrics.height(),metrics.ascent()};
}
Rendered render(const Candidate& item,double dpi,Budget& budget,std::uint64_t& admitted_pixels){
    const auto& p=item.parameters;const int pixels=font_pixels(p,dpi);
    const auto expanded=runtime_string(p.text,budget);
    if(expanded.empty())return {Raster(1,1,32,dpi),{},{},std::make_shared<const text_font_detail::ExportMasks>(text_font_detail::ExportMasks{{Raster(1,1,1,dpi),{}},{},p.text_color,p.line_color})}; // native empty-text guard also suppresses the leader
    const auto layout=font_layout(p,pixels,expanded,budget);const int width=std::max(1,layout.width),height=std::max(1,layout.height);
    const int tx=coordinate(p.text_anchor[0]),ty=coordinate(p.text_anchor[1]);
    const int px=coordinate(p.line_anchor[0]),py=coordinate(p.line_anchor[1]);
    const int left=p.line_enabled&&tx<px?coordinate(double(tx)-width):tx;
    const int right=coordinate(double(left)+width),bottom=coordinate(double(ty)+height),join_y=bottom-1;
    const QRect text_rect(left,ty,width,height);
    QRect bounds=text_rect;
    if(p.line_enabled){const QRect line_rect(std::min(tx,px),std::min(join_y,py),std::abs(tx-px),std::abs(join_y-py));
        if(line_rect.width()>0&&line_rect.height()>0)bounds=width>0&&height>0?text_rect.united(line_rect):line_rect;}
    const std::array<int,4> coordinates{std::min({tx,px,left,bounds.x()}),std::min({ty,py,bounds.y()}),std::max({tx,px,right,bounds.x()+bounds.width()}),std::max({ty,py,bottom,bounds.y()+bounds.height()})};
    const auto& owner=item.owner->common;
    if(std::int64_t(owner.x)+coordinates[0]<INT32_MIN||std::int64_t(owner.y)+coordinates[1]<INT32_MIN
       ||std::int64_t(owner.x)+coordinates[2]>INT32_MAX||std::int64_t(owner.y)+coordinates[3]>INT32_MAX)
        throw std::length_error("Text owner plus local coordinates exceed signed32");
    const int w=std::max(1,bounds.width()),h=std::max(1,bounds.height());
    if(w>100000||h>100000)throw std::length_error("Text cache dimensions exceed the bounded font profile");
    const auto backing=((std::uint64_t(w)+255)/256)*((std::uint64_t(h)+255)/256)*65536;
    const auto mask_count=p.line_enabled?2ull:1ull;
    if(backing*(1+mask_count)>32ull*1024*1024-admitted_pixels)throw std::length_error("Text caches exceed32Mi padded pixels");admitted_pixels+=backing*(1+mask_count);
    budget.allocate(backing*4+backing/8*mask_count+std::uint64_t(w)*h*4+backing/65536*128*(1+mask_count)+sizeof(text_font_detail::ExportMasks));budget.visit(std::uint64_t(w)*h*2);
    QImage image(w,h,QImage::Format_ARGB32);if(image.isNull())throw std::bad_alloc();image.fill(Qt::transparent);
    Raster result(w,h,32,dpi);
    text_font_detail::ExportMasks masks{{Raster(w,h,1,dpi),{bounds.x(),bounds.y()}},{},p.text_color,p.line_color};
    if(p.line_enabled)masks.leader=text_font_detail::Mask{Raster(w,h,1,dpi),{bounds.x(),bounds.y()}};
    const auto configure=[&](QPainter& painter){
        if(!painter.isActive())throw std::runtime_error("Cannot start offscreen Text painter");
        painter.setRenderHint(QPainter::TextAntialiasing,false);painter.setRenderHint(QPainter::Antialiasing,false);painter.setFont(layout.font);painter.translate(-bounds.x(),-bounds.y());
    };
    const auto transfer=[&](std::uint32_t pigment,Raster& mask){const auto c=color(pigment);
        for(int y=0;y<h;++y){budget.poll();const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
            for(int x=0;x<w;++x)if(qAlpha(row[x])){result.set_pixel(x,y,Pixel::rgba8(c.red(),c.green(),c.blue()));
                auto& tile=mask.tiles[{x/256,y/256}];if(!tile)tile=std::make_shared<Bytes>(8192,0);
                (*tile)[std::size_t(y%256)*32+x%256/8]|=std::uint8_t(0x80u>>(x%8));}}
    };
    // Native glyphs are visited before the line, and opaque destination pixels
    // win. Drawing the line first reproduces that priority at density100.
    if(p.line_enabled&&bounds.width()>0&&bounds.height()>0){
        {QPainter painter(&image);configure(painter);painter.setClipRect(bounds);painter.setPen(QPen(Qt::black,1,Qt::SolidLine,Qt::SquareCap));
            painter.drawLine(px,py,tx,join_y);painter.drawLine(left,join_y,right,join_y);}
        transfer(p.line_color,masks.leader->raster);image.fill(Qt::transparent);
    }
    {QPainter painter(&image);configure(painter);painter.setClipRect(text_rect);painter.setPen(Qt::black);
        int y=ty;for(const auto& line:layout.lines){budget.poll();if(!line.isEmpty())painter.drawText(QPoint(left,y+layout.ascent),line);y+=layout.line_height;}}
    transfer(p.text_color,masks.glyph.raster); // font color layers cannot override native COLORREF
    return {std::move(result),{bounds.x(),bounds.y()},coordinates,std::make_shared<const text_font_detail::ExportMasks>(std::move(masks))};
}
text_font_detail::Mask bitmap_mask(const QRect& bounds,double dpi,Budget& budget,std::uint64_t& padded_pixels,const std::function<void(QPainter&)>& draw){
    const int w=bounds.width(),h=bounds.height();
    if(w<1||h<1||w>100000||h>100000)throw std::length_error("Text mask dimensions exceed the bounded font profile");
    const auto backing=((std::uint64_t(w)+255)/256)*((std::uint64_t(h)+255)/256)*65536;
    if(backing>32ull*1024*1024-padded_pixels)throw std::length_error("Text masks exceed32Mi padded pixels");padded_pixels+=backing;
    budget.allocate(backing/8+backing/65536*128+std::uint64_t(w)*h*4);budget.visit(std::uint64_t(w)*h);
    QImage image(w,h,QImage::Format_ARGB32);if(image.isNull())throw std::bad_alloc();image.fill(Qt::transparent);
    {QPainter painter(&image);if(!painter.isActive())throw std::runtime_error("Cannot start offscreen Text mask painter");
        painter.setRenderHint(QPainter::TextAntialiasing,false);painter.setRenderHint(QPainter::Antialiasing,false);painter.translate(-bounds.x(),-bounds.y());draw(painter);}
    Raster raster(w,h,1,dpi);
    for(int y=0;y<h;++y){budget.poll();const auto* row=reinterpret_cast<const QRgb*>(image.constScanLine(y));
        for(int x=0;x<w;++x)if(qAlpha(row[x])){auto& tile=raster.tiles[{x/256,y/256}];if(!tile)tile=std::make_shared<Bytes>(8192,0);
            (*tile)[std::size_t(y%256)*32+x%256/8]|=std::uint8_t(0x80u>>(x%8));}}
    return {std::move(raster),{bounds.x(),bounds.y()}};
}
}
struct TextFontBackend::Impl {std::shared_ptr<Application> app=application();};
bool text_font_backend_available() noexcept{return true;}
TextFontBackend::TextFontBackend():impl_(std::make_unique<Impl>()){}
TextFontBackend::~TextFontBackend()=default;
std::size_t TextFontBackend::prepare(DocumentData& data,std::stop_token stop) const {
    return prepare_impl(data,stop,false);
}
std::size_t TextFontBackend::prepare_image(DocumentData& data,std::stop_token stop) const {
    return prepare_impl(data,stop,true);
}
std::size_t TextFontBackend::prepare_impl(DocumentData& data,std::stop_token stop,bool image) const {
    main_thread();Budget budget{0,0,stop};budget.poll();
    if(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))throw std::logic_error("Text font application lifetime has ended");
    const double dpi=data.base.dpi;if(!std::isfinite(dpi)||dpi<1||dpi>9600)throw std::invalid_argument("Invalid Text document DPI");
    std::uint64_t pixels{};std::vector<Candidate> candidates;
    for(auto& owner:data.layers)for(auto& plane:owner.planes){budget.visit(1);
        if(owner.opaque||plane.opaque||plane.family!=0x40000004)continue;
        if(candidates.size()==100000)throw std::length_error("Too many Text planes");
        if((owner.family!=0x40000800&&owner.family!=0x48040800)||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0
           ||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0||(!image&&plane.common.resolution!=dpi)
           ||owner.common.opacity<0||owner.common.opacity>100||plane.raster)
            throw std::invalid_argument("Unsupported ordinary Text font source or consumer DPI");
        budget.allocate(sizeof(Candidate)+sizeof(TextRasterCache)+sizeof(std::shared_ptr<const TextRasterCache>));
        // Decoder ownership, cache fingerprint, CP936 expansion, Unicode and
        // line containers are charged before any payload/string allocation.
        if(plane.specific.size()>16ull*1024*1024)throw std::length_error("Text payload exceeds16MiB");
        budget.allocate(plane.specific.size()*64ull+4096);budget.visit(plane.specific.size());
        candidates.push_back({&owner,&plane,decode_text_parameters(plane.specific)});
    }
    std::vector<std::shared_ptr<const TextRasterCache>> prepared;prepared.reserve(candidates.size());
    for(const auto& item:candidates){
        const bool mixed=image&&item.plane->common.resolution!=dpi;
        Rendered rendered;
        if(mixed){
            (void)font_pixels(item.parameters,dpi);
            budget.allocate(sizeof(text_font_detail::ExportMasks));
            auto masks=runtime_string(item.parameters.text,budget).empty()
                ?text_font_detail::ExportMasks{{Raster(1,1,1,dpi),{}},{},item.parameters.text_color,item.parameters.line_color}
                :text_font_detail::export_masks(*this,*item.owner,*item.plane,dpi,budget,pixels);
            rendered={Raster(1,1,32,dpi),{},{},std::make_shared<const text_font_detail::ExportMasks>(std::move(masks))};
        }else rendered=render(item,dpi,budget,pixels);
        prepared.push_back(std::make_shared<const TextRasterCache>(TextRasterCache{item.plane->specific,item.plane->common.width,item.plane->common.height,item.plane->common.resolution,std::move(rendered.image),rendered.origin,true,rendered.coordinates,std::move(rendered.masks),mixed,{item.owner->common.x,item.owner->common.y}}));}
    budget.poll();for(std::size_t i=0;i<candidates.size();++i)candidates[i].plane->runtime_text_cache=std::move(prepared[i]);
    return candidates.size();
}
text_font_detail::ExportMasks text_font_detail::export_masks(const TextFontBackend& backend,const Layer& owner,const Plane& plane,double dpi,Budget& budget,std::uint64_t& padded_pixels){
    (void)backend;main_thread();budget.poll();
    if(!qobject_cast<QGuiApplication*>(QCoreApplication::instance()))throw std::logic_error("Text font application lifetime has ended");
    if(plane.opaque||plane.family!=0x40000004||plane.raster||!std::isfinite(dpi)||dpi<1||dpi>9600||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0)
        throw std::invalid_argument("Unsupported Text export mask source");
    if(plane.specific.size()>16ull*1024*1024)throw std::length_error("Text payload exceeds16MiB");
    budget.allocate(plane.specific.size()*64ull+4096+sizeof(ExportMasks));budget.visit(plane.specific.size());
    const auto p=decode_text_parameters(plane.specific);const auto text=runtime_string(p.text,budget);
    const auto layout=font_layout(p,font_pixels(p,dpi),text,budget);
    const int width=std::max(1,layout.width),height=std::max(1,layout.height);
    using numeric_detail::binary64;const double ratio=binary64(dpi/plane.common.resolution);
    const auto projected=[&](const std::array<int,2>& point){std::array<int,2> result{};
        for(int i=0;i<2;++i){const auto sum=std::int64_t(i?owner.common.y:owner.common.x)+point[std::size_t(i)];
            if(sum<INT32_MIN||sum>INT32_MAX)throw std::length_error("Text owner plus anchor exceeds signed32");
            result[std::size_t(i)]=coordinate(binary64(double(sum)*ratio));}return result;};
    const auto anchor=projected(p.text_anchor);const auto endpoint=p.line_enabled?projected(p.line_anchor):anchor;
    const int left=p.line_enabled&&anchor[0]<endpoint[0]?coordinate(double(anchor[0])-width):anchor[0];
    const int right=coordinate(double(left)+width),bottom=coordinate(double(anchor[1])+height),join_y=bottom-1;
    const QRect glyph_rect(left,anchor[1],width,height);
    ExportMasks result;result.text_color=p.text_color;result.line_color=p.line_color;
    result.glyph=bitmap_mask(glyph_rect,dpi,budget,padded_pixels,[&](QPainter& painter){
        painter.setClipRect(glyph_rect);painter.setFont(layout.font);painter.setPen(Qt::black);int y=anchor[1];
        for(const auto& line:layout.lines){budget.poll();if(!line.isEmpty())painter.drawText(QPoint(left,y+layout.ascent),line);y+=layout.line_height;}
    });
    if(p.line_enabled){
        const QRect line_rect(std::min(anchor[0],endpoint[0]),std::min(join_y,endpoint[1]),std::abs(anchor[0]-endpoint[0]),std::abs(join_y-endpoint[1]));
        const QRect bounds=line_rect.width()>0&&line_rect.height()>0?glyph_rect.united(line_rect):glyph_rect;
        if(!std::isfinite(ratio)||ratio>8192)throw std::length_error("Text leader width exceeds the bounded font profile");
        result.leader=bitmap_mask(bounds,dpi,budget,padded_pixels,[&](QPainter& painter){
            painter.setClipRect(bounds);painter.setPen(QPen(Qt::black,std::max(1,int(ratio)),Qt::SolidLine,Qt::SquareCap));
            painter.drawLine(endpoint[0],endpoint[1],anchor[0],join_y);painter.drawLine(left,join_y,right,join_y);
        });
    }
    return result;
}
#else
struct TextFontBackend::Impl {};
bool text_font_backend_available() noexcept{return false;}
TextFontBackend::TextFontBackend(){throw std::runtime_error("Qt Text font backend is unavailable in this build");}
TextFontBackend::~TextFontBackend()=default;
std::size_t TextFontBackend::prepare(DocumentData&,std::stop_token) const {throw std::runtime_error("Qt Text font backend is unavailable in this build");}
std::size_t TextFontBackend::prepare_image(DocumentData&,std::stop_token) const {throw std::runtime_error("Qt Text font backend is unavailable in this build");}
text_font_detail::ExportMasks text_font_detail::export_masks(const TextFontBackend&,const Layer&,const Plane&,double,resolution_detail::Budget&,std::uint64_t&){throw std::runtime_error("Qt Text font backend is unavailable in this build");}
#endif
}
