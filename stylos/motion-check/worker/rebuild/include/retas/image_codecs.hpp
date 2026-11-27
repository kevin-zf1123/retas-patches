#pragma once
#include "raster.hpp"
#include <array>
namespace retas {
Raster decode_bmp(std::span<const std::uint8_t> bytes);
Bytes encode_bmp(const Raster& image,bool force24=true);
Raster decode_sgi(std::span<const std::uint8_t> bytes);
Bytes encode_sgi(const Raster& image,bool compressed=true,int channels=0);
Bytes encode_raw(const Raster& image,unsigned host_abi=184);
Raster decode_tiff(std::span<const std::uint8_t> bytes);
Bytes encode_tiff(const Raster& image);
struct YuvSettings {bool interlaced{};bool pal{};};
Bytes encode_yuv(const Raster& image,YuvSettings settings={});
Raster decode_lif(std::span<const std::uint8_t> bytes);
Raster decode_pic(std::span<const std::uint8_t> bytes);
Bytes encode_pic(const Raster& image);
struct PsdLayerInfo {Bytes name;int x{},y{},width{},height{};};
std::vector<PsdLayerInfo> psd_layers(std::span<const std::uint8_t> bytes);
Raster decode_psd(std::span<const std::uint8_t> bytes,int selected_layer=-1);
struct RlaImage {Raster raster;std::vector<float> z_depth;unsigned max_object_id{};bool has_z{};};
RlaImage decode_rla(std::span<const std::uint8_t> bytes,unsigned selection=0);
struct SkfImage {
    int width{},height{};
    double resolution{};
    std::array<Bytes,2> passes;
    std::array<int,2> opacity{100,100},enabled{1,1};
};
SkfImage decode_skf_data(std::span<const std::uint8_t> bytes);
void render_skf(const SkfImage& sketch,Raster& destination);
Raster decode_skf(std::span<const std::uint8_t> bytes);
}
