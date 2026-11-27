#include "retas/image_codecs.hpp"
#include <array>

namespace retas {
Bytes encode_yuv(const Raster& image,YuvSettings settings){
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||image.width<=0||image.height<=0||image.width>65535||image.height>65535)
        throw std::invalid_argument("Unsupported YUV input dimensions or layout");
    constexpr int width=720;const int height=settings.pal?576:486;
    auto resized=[&](int x,int y){
        const int x0=int(std::int64_t(image.width)*x/width),x1=int(std::int64_t(image.width)*(x+1)/width);
        const int y0=int(std::int64_t(image.height)*y/height),y1=int(std::int64_t(image.height)*(y+1)/height);
        std::array<std::uint64_t,3> sum{};std::uint64_t count=0;
        auto add=[&](int sx,int sy){const auto p=image.pixel(sx,sy);sum[0]+=p.r>>8;sum[1]+=p.g>>8;sum[2]+=p.b>>8;++count;};
        // CYUV::ChangeResolution includes the seed again in each downsample pass.
        add(x0,y0);
        if(width<image.width)for(int sx=x0;sx<x1;++sx)add(sx,y0);
        if(height<image.height)for(int sy=y0;sy<y1;++sy)for(int sx=x0;sx<x1;++sx)add(sx,sy);
        return std::array<std::int32_t,3>{std::int32_t(sum[0]/count),std::int32_t(sum[1]/count),std::int32_t(sum[2]/count)};
    };
    Writer w;w.data.reserve(std::size_t(width)*height*2);
    auto dot=[](auto p,int r,int g,int b){return p[0]*r+p[1]*g+p[2]*b;};
    auto row=[&](int y){
        std::uint16_t yf=0,uf=0,vf=0;std::int32_t prev_u=0,prev_v=0;
        for(int x=0;x<width;x+=2){const auto p0=resized(x,y),p1=resized(x+1,y);
            const auto ya0=yf+dot(p0,16829,33039,6416),ya1=std::uint16_t(ya0)+dot(p1,16829,33039,6416);
            const auto u1=dot(p1,-2426,-4765,7191),v1=dot(p1,7193,-6023,-1170);
            const auto ua=uf+prev_u+dot(p0,-4853,-9530,14383)+u1;
            const auto va=vf+prev_v+dot(p0,14386,-12046,-2340)+v1;
            w.u8(std::uint8_t((ua>>16)+128));w.u8(std::uint8_t((ya0>>16)+16));
            w.u8(std::uint8_t((va>>16)+128));w.u8(std::uint8_t((ya1>>16)+16));
            yf=std::uint16_t(ya1);uf=std::uint16_t(ua);vf=std::uint16_t(va);prev_u=u1;prev_v=v1;
        }
    };
    if(settings.interlaced){for(int field=0;field<2;++field)for(int y=field;y<height;y+=2)row(y);}
    else for(int y=0;y<height;++y)row(y);
    return std::move(w.data);
}
}
