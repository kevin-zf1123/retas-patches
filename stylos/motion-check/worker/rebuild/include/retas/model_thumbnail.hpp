#pragma once
#include "cel_folder.hpp"
#include "raster.hpp"
#include <optional>

namespace retas {
enum class ModelThumbnailContainer { Generic, Cel, Dga };
struct ModelThumbnailSize {
    int width{},height{};
    bool operator==(const ModelThumbnailSize&)const=default;
};
struct ModelThumbnail {
    int width{},height{},depth{32};
    // A generated zero extent is a present thumbnail without a Raster.
    // A missing file resource is represented by the outer optional instead.
    std::optional<Raster> image;
};
struct ModelThumbnailContext {
    // Borrowed cumulative counters; null selects a fresh per-call budget.
    CelFolderBudget* budget{};
    std::stop_token stop;
};
// Native CalcDestRect in a128x96 box, PC53/round-to-nearest operation order.
ModelThumbnailSize fit_model_thumbnail_size(int width,int height);
// Explicit target is limited to the CutFolder domain:0..128 x0..96.
ModelThumbnail make_model_thumbnail(const Raster&,ModelThumbnailSize,
    ModelThumbnailContext={});
ModelThumbnail make_model_thumbnail(const Raster&,ModelThumbnailContext={});
// Core-codec fallback with header/expansion admission before decoding.
// Extension is a lower-case dotted name, e.g. ".tga". Bytes remain borrowed.
Raster decode_model_thumbnail_source(std::span<const std::uint8_t>,
    std::string_view extension,ModelThumbnailContext={});
// Generic public FF07 decoder: fixed18-byte prefix, descriptor0, row-local
// RLE, native8-bit coverage semantics. Unsupported combined depth returns null.
std::optional<ModelThumbnail> decode_model_thumbnail(
    std::span<const std::uint8_t>,ModelThumbnailContext={});
// Generic first FF07 directory hit, or first CEL/DGA preview key1. CEL/DGA
// parsing stops immediately after that offscreen; full CEL data is not read.
std::optional<ModelThumbnail> read_model_thumbnail(
    std::span<const std::uint8_t>,ModelThumbnailContainer,
    ModelThumbnailContext={});
// Canonical bottom-up RLE BGRA32 payload. Zero extents cannot be encoded.
Bytes encode_model_thumbnail(const ModelThumbnail&,ModelThumbnailContext={});
// Explicit generic-file update using the shared canonical resource writer.
// File publication belongs to the caller; this function only returns bytes.
Bytes store_model_thumbnail(std::span<const std::uint8_t>,const ModelThumbnail&,
    ModelThumbnailContext={});
}
