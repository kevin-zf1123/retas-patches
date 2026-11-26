#pragma once

#include "document.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <vector>

namespace retas {

inline constexpr std::size_t retas_resource_directory_size = 0x0a08;
inline constexpr std::size_t retas_resource_directory_capacity = 256;

inline constexpr std::uint16_t retas_resource_preview_small = 0xff00;
inline constexpr std::uint16_t retas_resource_mainline_mask = 0xff01;
inline constexpr std::uint16_t retas_resource_preview_large = 0xff02;
inline constexpr std::uint16_t retas_resource_grad_auxiliary_a = 0xff03;
inline constexpr std::uint16_t retas_resource_grad_auxiliary_b = 0xff04;
inline constexpr std::uint16_t retas_resource_document_data = 0xff06;
inline constexpr std::uint16_t retas_resource_model_thumbnail = 0xff07;

struct RetasResource {
    std::uint16_t id{};
    std::size_t directory_index{};
    std::uint32_t record_offset{};
    Bytes payload;
};

struct RetasResourceArchive {
    std::size_t directory_offset{};
    std::uint8_t major{};
    std::uint8_t minor{};
    std::vector<RetasResource> resources;

    const RetasResource* find(std::uint16_t id) const;
};

struct RetasResourceUpdate {
    std::uint16_t id{};
    Bytes payload;
};

// Returns nullopt when the file has no canonical RETAS directory at EOF.
// A canonical-looking but malformed directory is rejected with FormatError.
std::optional<RetasResourceArchive> decode_retas_resource_archive(
    std::span<const std::uint8_t> file);

// Applies the host's append/update policy safely: bytes before the old
// directory are retained, replacement records are appended, the first
// matching/free entry is updated, and a canonical directory is emitted.
Bytes rewrite_retas_resource_archive(
    std::span<const std::uint8_t> file,
    std::span<const RetasResourceUpdate> updates);

// PaintMan's ff01 payload is a headerless, row-major, MSB-first bit mask.
// A bit is set for an opaque, non-white pixel in the explicit main-line plane.
Bytes encode_retas_nonwhite_mask(const Raster& mainline);

struct RetasFf06Raster {
    Raster raster;
    bool has_unrestored_mark_records{};
};

// ff06 stores colored document pixels as one PackBits-compressed bit mask per
// color. The trailing mark-record variant is retained but not interpreted.
RetasFf06Raster decode_retas_ff06_raster(
    std::span<const std::uint8_t> payload,
    int expected_width,
    int expected_height,
    double resolution = 144.0);
Bytes encode_retas_ff06_raster(const Raster& raster);

// Renders the standard-image pixels used by PaintMan's 0x322 save request:
// binary/gradation color layers are forced visible at full layer opacity, and
// selection and shadow-designation helper layers stay outside the base image.
Raster render_retas_image_base(const DocumentData& document);
// Same standard-image layer policy, with explicit 32/64-bit output precision.
Raster render_retas_image_base(const DocumentData& document,int target_depth);
// Native request0x40 is suppressed by a visible11001000 production-color owner.
Raster render_retas_image_base(const DocumentData&,int target_depth,bool white_background);

struct RetasPreparedImageResources {
    std::vector<RetasResourceUpdate> updates;
    std::vector<std::uint16_t> unsupported_existing_ids;
    bool has_unrendered_document_content{};
    // True when the host's 256-pixel block averaging path was used.
    bool previews_match_native_pixels{};

    bool can_write_compatible() const {
        return !has_unrendered_document_content && unsupported_existing_ids.empty();
    }
};

// Builds resources from explicit document planes. An existing ff06 payload is
// retained byte-for-byte while its decoded raster still matches the shadow
// plane; PaintMan keeps that raw handle on standard-image saves. Existing
// resources whose dependency semantics are not modeled are reported and
// deliberately omitted.
RetasPreparedImageResources prepare_retas_image_resources(
    const DocumentData& document,
    const RetasResourceArchive* existing = nullptr);

struct RetasDecodedResources {
    std::map<std::uint16_t, Raster> auxiliary_rasters;
    std::optional<Bytes> mainline_mask;
    std::optional<Bytes> opaque_document_data;
    std::optional<Raster> document_raster;
    std::vector<std::uint16_t> unrestored_resource_ids;

    bool has_unrestored_semantics() const {
        return !unrestored_resource_ids.empty();
    }
};

// Decodes only resource semantics established by the host producer and
// consumer. Original ff06 bytes remain available beside its decoded raster;
// unknown records are reported without interpretation.
RetasDecodedResources decode_retas_image_resources(
    const RetasResourceArchive& archive,
    int image_width,
    int image_height,
    double image_resolution = 144.0);

struct RetasRasterImport {
    DocumentData document;
    RetasDecodedResources resources;
    bool mainline_separated{};
    bool document_raster_restored{};
};

// Reconstructs the proven binary-color topology from ff01: color plane first,
// main-line plane second, plus the proven ff06 shadow raster when complete.
RetasRasterImport restore_retas_raster_document(
    const Raster& source,
    const RetasResourceArchive& archive,
    Bytes layer_name = {});

} // namespace retas
