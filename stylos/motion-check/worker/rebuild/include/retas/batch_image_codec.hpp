#pragma once
#include "retas_resources.hpp"
#include "image_codecs.hpp"
#include "png.hpp"
#include "tga.hpp"
#include "svd.hpp"
#include <functional>
#include <stop_token>
namespace retas {
struct BatchImageWriteOptions {
    TgaSettings tga;bool sgi_compressed{true},bmp_force24{true};int jpeg_quality{100};
    YuvSettings yuv;bool png_16bit{};PngFilter png_filter{PngFilter::None};
};
struct BatchImageContext {
    std::optional<RetasResourceArchive> resources;
    BatchImageWriteOptions write;
};
struct BatchDecodedImage {DocumentData data;BatchImageContext context;};
// Adapters operate only on already-admitted memory. The Batch runner owns
//file identity checks, destination policy and atomic publication.
struct BatchImageCodec {
    std::vector<std::string> extensions;
    std::uint32_t category{1};bool ordinary_cel{true};
    std::function<BatchDecodedImage(std::string_view,std::span<const std::uint8_t>,std::stop_token)> decode;
    std::function<Bytes(std::string_view,const DocumentData&,const BatchImageContext&,std::stop_token)> encode;
};
struct BatchImageCodecs {
    std::vector<BatchImageCodec> formats;
    const BatchImageCodec* find(std::string_view extension)const;
    // Checks descriptor sizes/uniqueness before the runner snapshots handles.
    std::size_t storage()const;
};
BatchImageCodecs default_batch_image_codecs();
BatchImageCodecs default_batch_image_codecs(const SvdImportSettings&);
}
