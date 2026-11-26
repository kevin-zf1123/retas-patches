#include "retas/adjust_plane.hpp"
#include <cmath>

namespace retas {
namespace {
bool parameterless(std::uint32_t kind){return kind==8||kind==16||kind==32||kind==64||kind==128||kind==256;}
}
AdjustParameters decode_adjust_parameters(std::span<const std::uint8_t> bytes){
    Reader wrapper(bytes);const auto kind=wrapper.u32();
    auto r=wrapper.section();AdjustParameters out;out.wrapper_suffix=wrapper.rest();
    if(kind==1){out.kind=FilterKind::Contrast;out.settings=ContrastSettings{r.i32(),r.i32()};}
    else if(kind==2){
        out.kind=FilterKind::Level;LevelSettings settings;const auto count=r.i32();r.require_count(count,28,4);
        for(int i=0;i<count;++i){auto c=r.section();settings.channels[i]={c.i32(),c.i32(),c.i32(),c.i32(),c.f64()};out.channel_suffix[i]=c.rest();}
        out.settings=settings;
    }else if(kind==4){
        out.kind=FilterKind::ToneCurve;ToneCurveSettings settings;settings.interpolation=r.i32();
        const auto count=r.i32();r.require_count(count,4,4);
        for(int i=0;i<count;++i){const auto n=r.i32();r.require_count(n,8,32);auto& points=settings.curves[i].points;points.clear();for(int j=0;j<n;++j)points.push_back({r.i32(),r.i32()});}
        out.settings=std::move(settings);
    }else if(kind==0x200){out.kind=FilterKind::GaussianBlur;out.settings=GaussianBlurSettings{r.f64(),r.i32(),true};}
    else if(kind==0x1000){out.kind=FilterKind::UnsharpMask;out.settings=UnsharpMaskSettings{r.i32(),r.i32(),r.i32()};}
    else if(kind==0x2000){out.kind=FilterKind::Hsv;out.settings=HsvSettings{r.f64(),r.f64(),r.f64()};}
    else if(kind==0x4000){out.kind=FilterKind::ColorBalance;out.settings=ColorBalanceSettings{r.f64(),r.f64(),r.f64(),r.i32()};}
    else if(kind==0x8000){out.kind=FilterKind::Dust;out.settings=DustSettings{static_cast<DustMode>(r.i32()),r.f64(),r.i32(),true};}
    else if(parameterless(kind)){out.parameterless_kind=kind;out.settings=std::monostate{};}
    else throw std::invalid_argument("Unknown Adjust helper");
    out.payload_suffix=r.rest();return out;
}
Bytes encode_adjust_parameters(const AdjustParameters& p){
    if(p.parameterless_kind){
        if(!parameterless(p.parameterless_kind)||!std::holds_alternative<std::monostate>(p.settings))throw std::invalid_argument("Invalid parameterless Adjust helper");
        Writer w;w.u32(p.parameterless_kind);w.section(p.payload_suffix);w.raw(p.wrapper_suffix);return std::move(w.data);
    }
    if(!settings_match_filter(p.kind,p.settings))throw std::invalid_argument("Adjust settings kind mismatch");
    // The same validation used by the filter settings dialog, on an empty sparse image.
    Raster probe(1,1);const auto validation=apply_filter(probe,p.kind,p.settings);
    if(validation.rejected())throw std::invalid_argument(validation.error);
    Writer w,r;std::uint32_t kind{};
    if(p.kind==FilterKind::Contrast){kind=1;const auto& s=std::get<ContrastSettings>(p.settings);r.i32(s.contrast);r.i32(s.brightness);}
    else if(p.kind==FilterKind::Level){kind=2;r.i32(4);const auto& s=std::get<LevelSettings>(p.settings);
        for(std::size_t i=0;i<4;++i){const auto& c=s.channels[i];Writer child;child.i32(c.high);child.i32(c.low);child.i32(c.output_high);child.i32(c.output_low);child.f64(c.gamma);child.raw(p.channel_suffix[i]);r.section(child.data);}}
    else if(p.kind==FilterKind::ToneCurve){kind=4;const auto& s=std::get<ToneCurveSettings>(p.settings);r.i32(s.interpolation);r.i32(4);
        for(const auto& curve:s.curves){if(curve.points.size()>32)throw std::invalid_argument("Adjust curve exceeds 32 points");r.i32(int(curve.points.size()));for(const auto& q:curve.points){r.i32(q.x);r.i32(q.y);}}}
    else if(p.kind==FilterKind::GaussianBlur){kind=0x200;const auto& s=std::get<GaussianBlurSettings>(p.settings);r.f64(s.size_mm);r.i32(s.size_px);}
    else if(p.kind==FilterKind::UnsharpMask){kind=0x1000;const auto& s=std::get<UnsharpMaskSettings>(p.settings);r.i32(s.range);r.i32(s.contain);r.i32(s.strength);}
    else if(p.kind==FilterKind::Hsv){kind=0x2000;const auto& s=std::get<HsvSettings>(p.settings);r.f64(s.hue);r.f64(s.saturation);r.f64(s.brightness);}
    else if(p.kind==FilterKind::ColorBalance){kind=0x4000;const auto& s=std::get<ColorBalanceSettings>(p.settings);r.f64(s.red);r.f64(s.green);r.f64(s.blue);r.i32(s.keep_luminosity);}
    else if(p.kind==FilterKind::Dust){kind=0x8000;const auto& s=std::get<DustSettings>(p.settings);r.i32(static_cast<int>(s.mode));r.f64(s.size_mm);r.i32(s.size_px);}
    else throw std::invalid_argument("Adjust helper has no native settings editor");
    r.raw(p.payload_suffix);w.u32(kind);w.section(r.data);w.raw(p.wrapper_suffix);return std::move(w.data);
}
bool editable_adjust_plane(const Plane& p) noexcept{
    if(p.opaque||p.family!=0x40000010)return false;
    try{const auto decoded=decode_adjust_parameters(p.specific);(void)encode_adjust_parameters(decoded);return true;}catch(...){return false;}
}
}
