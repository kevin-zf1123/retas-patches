#include "retas/model_thumbnail.hpp"
#include "retas/retas_resources.hpp"

#include "retas/tga.hpp"
#include "../core/vector_line_output.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <limits>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace retas {
namespace {

constexpr std::size_t max_archive_bytes = 512ull * 1024 * 1024;
constexpr std::uint32_t max_ff06_color_records = 100000;
constexpr std::array<std::uint8_t, 8> canonical_header{
    'R', 'E', 'T', 'A', 'S', 0, 1, 0};

struct DirectoryEntry {
    std::uint16_t id{};
    std::uint32_t length{};
    std::uint32_t offset{};
};

std::uint16_t little_u16(std::span<const std::uint8_t> data, std::size_t offset) {
    return std::uint16_t(data[offset]) |
           std::uint16_t(std::uint16_t(data[offset + 1]) << 8);
}

std::uint32_t little_u32(std::span<const std::uint8_t> data, std::size_t offset) {
    return std::uint32_t(data[offset]) |
           (std::uint32_t(data[offset + 1]) << 8) |
           (std::uint32_t(data[offset + 2]) << 16) |
           (std::uint32_t(data[offset + 3]) << 24);
}

std::uint32_t big_u32(std::span<const std::uint8_t> data, std::size_t offset) {
    return (std::uint32_t(data[offset]) << 24) |
           (std::uint32_t(data[offset + 1]) << 16) |
           (std::uint32_t(data[offset + 2]) << 8) |
           std::uint32_t(data[offset + 3]);
}

void append_little_u16(Bytes& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void append_little_u32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 24));
}

void append_big_u32(Bytes& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>(value >> 24));
    out.push_back(static_cast<std::uint8_t>(value >> 16));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
    out.push_back(static_cast<std::uint8_t>(value));
}

bool starts_with_retas(std::span<const std::uint8_t> bytes) {
    constexpr std::array<std::uint8_t, 5> magic{'R', 'E', 'T', 'A', 'S'};
    return bytes.size() >= magic.size() &&
           std::equal(magic.begin(), magic.end(), bytes.begin());
}

bool is_canonical_header(std::span<const std::uint8_t> bytes) {
    return bytes.size() >= canonical_header.size() &&
           std::equal(canonical_header.begin(), canonical_header.end(), bytes.begin());
}

std::array<DirectoryEntry, retas_resource_directory_capacity> read_entries(
    std::span<const std::uint8_t> file,
    std::size_t directory_offset) {
    std::array<DirectoryEntry, retas_resource_directory_capacity> entries{};
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::size_t at = directory_offset + canonical_header.size() + i * 10;
        entries[i] = {little_u16(file, at), little_u32(file, at + 2),
                      little_u32(file, at + 6)};
    }
    return entries;
}

std::size_t checked_mask_size(int width, int height) {
    if (width <= 0 || height <= 0 || width > 100000 || height > 100000)
        throw std::invalid_argument("Invalid RETAS mask dimensions");
    const std::size_t stride = (static_cast<std::size_t>(width) + 7) / 8;
    if (stride > max_archive_bytes / static_cast<std::size_t>(height))
        throw std::length_error("RETAS mask exceeds supported size");
    return stride * static_cast<std::size_t>(height);
}

void add_unrestored(RetasDecodedResources& decoded, std::uint16_t id) {
    if (std::find(decoded.unrestored_resource_ids.begin(),
                  decoded.unrestored_resource_ids.end(), id) ==
        decoded.unrestored_resource_ids.end())
        decoded.unrestored_resource_ids.push_back(id);
}

Raster raster32_copy(const Raster& source) {
    Raster result(source.width, source.height, 32, source.resolution);
    const Pixel empty;
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            const auto pixel = source.pixel(x, y);
            if (pixel != empty) result.set_pixel(x, y, pixel);
        }
    }
    result.prune_empty();
    return result;
}

Bytes decode_packbits(std::span<const std::uint8_t> encoded,
                      std::size_t expected_size,
                      std::size_t error_offset) {
    Bytes decoded;
    decoded.reserve(expected_size);
    std::size_t position = 0;
    while (decoded.size() < expected_size) {
        if (position >= encoded.size())
            throw FormatError(error_offset + position, "Truncated RETAS ff06 PackBits data");
        const std::uint8_t control = encoded[position++];
        const std::size_t count = std::size_t(control & 0x7f) + 1;
        if (count > expected_size - decoded.size())
            throw FormatError(error_offset + position - 1,
                              "RETAS ff06 PackBits packet exceeds bitmap");
        if (control & 0x80) {
            if (position >= encoded.size())
                throw FormatError(error_offset + position,
                                  "Truncated RETAS ff06 PackBits run");
            decoded.insert(decoded.end(), count, encoded[position++]);
        } else {
            if (count > encoded.size() - position)
                throw FormatError(error_offset + position,
                                  "Truncated RETAS ff06 PackBits literal");
            decoded.insert(decoded.end(), encoded.begin() + static_cast<std::ptrdiff_t>(position),
                           encoded.begin() + static_cast<std::ptrdiff_t>(position + count));
            position += count;
        }
    }
    if (position != encoded.size())
        throw FormatError(error_offset + position, "Trailing RETAS ff06 PackBits data");
    return decoded;
}

Bytes encode_packbits_rows(std::span<const std::uint8_t> raw,
                           std::size_t stride,
                           std::size_t rows) {
    Bytes encoded;
    for (std::size_t y = 0; y < rows; ++y) {
        const auto row = raw.subspan(y * stride, stride);
        std::size_t x = 0;
        while (x < row.size()) {
            std::size_t run = 1;
            while (run < 128 && x + run < row.size() && row[x + run] == row[x]) ++run;
            if (run >= 2) {
                encoded.push_back(static_cast<std::uint8_t>(0x80 | (run - 1)));
                encoded.push_back(row[x]);
                x += run;
                continue;
            }

            const std::size_t start = x++;
            while (x < row.size() && x - start < 128) {
                run = 1;
                while (run < 128 && x + run < row.size() && row[x + run] == row[x])
                    ++run;
                if (run >= 2) break;
                ++x;
            }
            encoded.push_back(static_cast<std::uint8_t>(x - start - 1));
            encoded.insert(encoded.end(), row.begin() + static_cast<std::ptrdiff_t>(start),
                           row.begin() + static_cast<std::ptrdiff_t>(x));
        }
    }
    return encoded;
}

void attach_document_raster(DocumentData& document,
                            const RetasDecodedResources& resources) {
    if (!resources.document_raster) return;
    const auto& raster = *resources.document_raster;
    Layer shadow;
    shadow.family = 0x10040000;
    shadow.common = document.layers.front().common;
    shadow.common.runtime_identity=next_element_identity();
    shadow.common.name = ascii("Shadow");
    shadow.common.depth = 32;
    // The host import path explicitly initializes this auxiliary layer at 50%.
    shadow.common.opacity = 50;
    shadow.current_plane = 0;

    Plane plane;
    plane.family = 0x12400001;
    plane.common = shadow.common;
    plane.common.runtime_identity=next_element_identity();
    plane.common.name = ascii("Shadow Paint");
    plane.common.opacity = 100;
    plane.common.opacity_mode = 5;
    plane.raster = raster;
    plane.pixels_changed = true;
    shadow.planes.push_back(std::move(plane));

    Plane boundary;
    boundary.family = 0x11410001;
    boundary.common = shadow.common;
    boundary.common.runtime_identity=next_element_identity();
    boundary.common.name = ascii("Boundary Line");
    boundary.common.opacity = 100;
    boundary.common.opacity_mode = 5;
    boundary.raster.emplace(raster.width, raster.height, 32, raster.resolution);
    boundary.pixels_changed = true;
    shadow.planes.push_back(std::move(boundary));

    document.layers.insert(document.layers.begin(), std::move(shadow));
    ++document.current_layer;
}

struct ExportPlane {
    const Layer* layer{};
    const Plane* plane{};
};

std::optional<ExportPlane> exported_mainline(const DocumentData& document) {
    for (auto layer = document.layers.rbegin(); layer != document.layers.rend(); ++layer) {
        if (layer->family != 0x11000800) continue;
        const auto plane = std::find_if(layer->planes.begin(), layer->planes.end(),
                                        [](const auto& value) {
                                            return value.family == 0x11110002;
                                        });
        return plane != layer->planes.end() && plane->raster
            ? std::optional<ExportPlane>{{&*layer, &*plane}}
            : std::nullopt;
    }
    return std::nullopt;
}

std::optional<ExportPlane> exported_document_raster(const DocumentData& document) {
    for (const auto& layer : document.layers) {
        if (layer.family != 0x10040000) continue;
        const auto plane = std::find_if(layer.planes.begin(), layer.planes.end(),
                                        [](const auto& value) {
                                            return value.family == 0x12400001;
                                        });
        if (plane != layer.planes.end() && plane->raster)
            return ExportPlane{&layer, &*plane};
        return std::nullopt;
    }
    return std::nullopt;
}

bool structural_resource_plane(const ExportPlane& binding,
                               const Raster& canvas) {
    const auto& layer = *binding.layer;
    const auto& plane = *binding.plane;
    const auto& raster = *plane.raster;
    return layer.common.x == 0 && layer.common.y == 0 &&
           plane.common.x == 0 && plane.common.y == 0 &&
           layer.common.width == canvas.width && layer.common.height == canvas.height &&
           plane.common.width == canvas.width && plane.common.height == canvas.height &&
           raster.width == canvas.width && raster.height == canvas.height;
}

void add_unsupported(RetasPreparedImageResources& prepared, std::uint16_t id) {
    if (std::find(prepared.unsupported_existing_ids.begin(),
                  prepared.unsupported_existing_ids.end(), id) ==
        prepared.unsupported_existing_ids.end())
        prepared.unsupported_existing_ids.push_back(id);
}

Raster make_preview(const Raster& source, int width, int height,
                    bool& matches_native) {
    Raster preview(width, height, 32, source.resolution);
    matches_native = false;
    int fitted_width = width;
    int fitted_height = height;
    if (std::int64_t(width) * source.height <=
        std::int64_t(height) * source.width) {
        fitted_height = static_cast<int>(
            std::int64_t(width) * source.height / source.width);
    } else {
        fitted_width = static_cast<int>(
            std::int64_t(height) * source.width / source.height);
    }

    // RCRect::CalcDestRect removes the same integer margin from both sides.
    // When the unused extent is odd, the resulting rectangle is one pixel
    // larger than the truncated fitted extent.
    const int left = (width - fitted_width) / 2;
    const int top = (height - fitted_height) / 2;
    const int right = width - left;
    const int bottom = height - top;
    const int output_width = right - left;
    const int output_height = bottom - top;
    if (output_width <= 0 || output_height <= 0) return preview;

    auto flattened_channel = [](std::uint16_t channel, std::uint16_t alpha) {
        // sub_440500's standard-image offscreen has an opaque white display
        // base by the time its preview is reduced.
        return static_cast<std::uint8_t>(
            (std::uint64_t(channel) * alpha +
             std::uint64_t(65535) * (65535 - alpha)) /
            65535 / 257);
    };

    // The proven production path is a reduction. ChangeResolutionLow's
    // enlargement edge behavior reads outside a consumed one-pixel bucket;
    // retain a bounded nearest result for that rare case and report it rather
    // than claiming native preview identity.
    if (output_width > source.width || output_height > source.height) {
        for (int y = top; y < bottom; ++y) {
            const int source_y = std::min(
                source.height - 1,
                static_cast<int>(std::int64_t(y - top) * source.height /
                                 output_height));
            for (int x = left; x < right; ++x) {
                const int source_x = std::min(
                    source.width - 1,
                    static_cast<int>(std::int64_t(x - left) * source.width /
                                     output_width));
                const auto pixel = source.pixel(source_x, source_y);
                preview.set_pixel(x, y, Pixel::rgba8(
                    flattened_channel(pixel.r, pixel.a),
                    flattened_channel(pixel.g, pixel.a),
                    flattened_channel(pixel.b, pixel.a)));
            }
        }
        return preview;
    }
    matches_native = true;

    // CHANGE_RESOLUTION_MODE 0 selects ChangeResolutionLow. It partitions
    // each 256x256 source block independently into integer destination
    // buckets, averages byte channels with truncating division, and writes
    // the buckets in block order. The per-block partition is observable at
    // block boundaries and must not be replaced with one global box filter.
    for (int block_top = 0; block_top < source.height;
         block_top += Raster::tile_side) {
        const int block_bottom = std::min(source.height,
                                          block_top + Raster::tile_side);
        const int destination_top = top + static_cast<int>(
            std::int64_t(output_height) * block_top / source.height);
        const int destination_bottom = top + static_cast<int>(
            std::int64_t(output_height) * block_bottom / source.height);
        if (destination_bottom <= destination_top) continue;

        for (int block_left = 0; block_left < source.width;
             block_left += Raster::tile_side) {
            const int block_right = std::min(source.width,
                                              block_left + Raster::tile_side);
            const int destination_left = left + static_cast<int>(
                std::int64_t(output_width) * block_left / source.width);
            const int destination_right = left + static_cast<int>(
                std::int64_t(output_width) * block_right / source.width);
            if (destination_right <= destination_left) continue;

            int source_top = block_top;
            for (int y = destination_top; y < destination_bottom; ++y) {
                int source_bottom = block_top + static_cast<int>(
                    std::int64_t(block_bottom - block_top) *
                    (y - destination_top + 1) /
                    (destination_bottom - destination_top));
                if (source_bottom == source_top) ++source_bottom;
                source_bottom = std::min(source_bottom, block_bottom);

                int source_left = block_left;
                for (int x = destination_left; x < destination_right; ++x) {
                    int source_right = block_left + static_cast<int>(
                        std::int64_t(block_right - block_left) *
                        (x - destination_left + 1) /
                        (destination_right - destination_left));
                    if (source_right == source_left) ++source_right;
                    source_right = std::min(source_right, block_right);

                    std::uint64_t red = 0;
                    std::uint64_t green = 0;
                    std::uint64_t blue = 0;
                    for (int source_y = source_top;
                         source_y < source_bottom; ++source_y) {
                        for (int source_x = source_left;
                             source_x < source_right; ++source_x) {
                            const auto pixel = source.pixel(source_x, source_y);
                            red += flattened_channel(pixel.r, pixel.a);
                            green += flattened_channel(pixel.g, pixel.a);
                            blue += flattened_channel(pixel.b, pixel.a);
                        }
                    }
                    const auto count = std::uint64_t(source_right - source_left) *
                                       (source_bottom - source_top);
                    preview.set_pixel(x, y, Pixel::rgba8(
                        static_cast<unsigned>(red / count),
                        static_cast<unsigned>(green / count),
                        static_cast<unsigned>(blue / count)));
                    source_left = source_right;
                }
                source_top = source_bottom;
            }
        }
    }
    return preview;
}

} // namespace

const RetasResource* RetasResourceArchive::find(std::uint16_t id) const {
    const auto found = std::find_if(resources.begin(), resources.end(),
                                    [id](const auto& value) { return value.id == id; });
    return found == resources.end() ? nullptr : &*found;
}

std::optional<RetasResourceArchive> decode_retas_resource_archive(
    std::span<const std::uint8_t> file) {
    if (file.size() < retas_resource_directory_size) return std::nullopt;
    if (file.size() > max_archive_bytes)
        throw FormatError(0, "RETAS resource archive exceeds supported size");

    const std::size_t directory_offset = file.size() - retas_resource_directory_size;
    const auto directory = file.subspan(directory_offset, retas_resource_directory_size);
    if (!starts_with_retas(directory)) return std::nullopt;
    if (!is_canonical_header(directory))
        throw FormatError(directory_offset, "Noncanonical RETAS resource directory");
    if (directory_offset < canonical_header.size())
        throw FormatError(directory_offset,
                          "RETAS resource directory has no encoded image prefix");

    const auto entries = read_entries(file, directory_offset);
    struct Span {
        std::size_t begin{};
        std::size_t end{};
        std::size_t directory_index{};
    };
    std::vector<Span> spans;
    std::set<std::uint16_t> ids;
    RetasResourceArchive archive;
    archive.directory_offset = directory_offset;
    archive.major = directory[6];
    archive.minor = directory[7];

    for (std::size_t i = 0; i < entries.size(); ++i) {
        const auto& entry = entries[i];
        const std::size_t entry_offset = directory_offset + canonical_header.size() + i * 10;
        if (!entry.id) {
            if (entry.length || entry.offset)
                throw FormatError(entry_offset, "Nonempty RETAS free directory entry");
            continue;
        }
        if (!ids.insert(entry.id).second)
            throw FormatError(entry_offset, "Duplicate RETAS resource id");

        const std::uint64_t record_end = std::uint64_t(entry.offset) + 2 + entry.length;
        if (entry.offset < canonical_header.size() || record_end > directory_offset)
            throw FormatError(entry_offset, "RETAS resource record is out of bounds");
        if ((std::uint16_t(file[entry.offset]) << 8 | file[entry.offset + 1]) != entry.id)
            throw FormatError(entry.offset, "RETAS resource prefix does not match directory");

        spans.push_back({entry.offset, static_cast<std::size_t>(record_end), i});
        RetasResource resource;
        resource.id = entry.id;
        resource.directory_index = i;
        resource.record_offset = entry.offset;
        archive.resources.push_back(std::move(resource));
    }

    std::sort(spans.begin(), spans.end(), [](const auto& left, const auto& right) {
        return left.begin < right.begin;
    });
    for (std::size_t i = 1; i < spans.size(); ++i) {
        if (spans[i].begin < spans[i - 1].end) {
            const auto entry_offset = directory_offset + canonical_header.size() +
                                      spans[i].directory_index * 10;
            throw FormatError(entry_offset, "Overlapping RETAS resource records");
        }
    }
    // Validate all ranges before copying any payload. A malformed directory
    // can otherwise amplify one overlapping input range into256 allocations.
    for (auto& resource : archive.resources) {
        const auto& entry = entries[resource.directory_index];
        const auto payload = file.subspan(static_cast<std::size_t>(entry.offset) + 2,
                                         entry.length);
        resource.payload.assign(payload.begin(), payload.end());
    }
    return archive;
}

Bytes rewrite_retas_resource_archive(
    std::span<const std::uint8_t> file,
    std::span<const RetasResourceUpdate> updates) {
    const auto archive = decode_retas_resource_archive(file);
    if (!archive && updates.empty()) return Bytes(file.begin(), file.end());

    std::array<DirectoryEntry, retas_resource_directory_capacity> entries{};
    std::size_t prefix_size = file.size();
    if (archive) {
        prefix_size = archive->directory_offset;
        for (const auto& resource : archive->resources) {
            entries[resource.directory_index] = {
                resource.id, static_cast<std::uint32_t>(resource.payload.size()),
                resource.record_offset};
        }
    }
    if (!updates.empty() && prefix_size < canonical_header.size())
        throw std::invalid_argument("RETAS resources require an encoded image prefix");
    if (prefix_size > max_archive_bytes - retas_resource_directory_size)
        throw std::length_error("RETAS resource archive exceeds supported size");

    Bytes output(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(prefix_size));
    std::set<std::uint16_t> update_ids;
    for (const auto& update : updates) {
        if (!update.id) throw std::invalid_argument("RETAS resource id zero is reserved");
        if (!update_ids.insert(update.id).second)
            throw std::invalid_argument("Duplicate RETAS resource update id");
        if (update.payload.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("RETAS resource payload exceeds 32-bit size");
        if (output.size() > std::numeric_limits<std::uint32_t>::max())
            throw std::length_error("RETAS resource offset exceeds 32-bit size");
        if (update.payload.size() + 2 >
            max_archive_bytes - retas_resource_directory_size - output.size())
            throw std::length_error("RETAS resource archive exceeds supported size");

        auto slot = std::find_if(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.id == update.id;
        });
        if (slot == entries.end())
            slot = std::find_if(entries.begin(), entries.end(),
                                [](const auto& entry) { return entry.id == 0; });
        if (slot == entries.end()) throw std::length_error("RETAS resource directory is full");

        const auto offset = static_cast<std::uint32_t>(output.size());
        output.push_back(static_cast<std::uint8_t>(update.id >> 8));
        output.push_back(static_cast<std::uint8_t>(update.id));
        output.insert(output.end(), update.payload.begin(), update.payload.end());
        *slot = {update.id, static_cast<std::uint32_t>(update.payload.size()), offset};
    }

    output.insert(output.end(), canonical_header.begin(), canonical_header.end());
    for (const auto& entry : entries) {
        append_little_u16(output, entry.id);
        append_little_u32(output, entry.length);
        append_little_u32(output, entry.offset);
    }
    return output;
}

Bytes encode_retas_nonwhite_mask(const Raster& mainline) {
    const auto size = checked_mask_size(mainline.width, mainline.height);
    const std::size_t stride = (static_cast<std::size_t>(mainline.width) + 7) / 8;
    Bytes mask(size, 0);
    for (int y = 0; y < mainline.height; ++y) {
        for (int x = 0; x < mainline.width; ++x) {
            const auto pixel = mainline.pixel(x, y);
            if (pixel.a &&
                (pixel.r != 65535 || pixel.g != 65535 || pixel.b != 65535)) {
                mask[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) / 8] |=
                    static_cast<std::uint8_t>(0x80u >> (x & 7));
            }
        }
    }
    return mask;
}

RetasFf06Raster decode_retas_ff06_raster(
    std::span<const std::uint8_t> payload,
    int expected_width,
    int expected_height,
    double resolution) {
    (void)checked_mask_size(expected_width, expected_height);
    if (payload.size() < 24) throw FormatError(0, "Truncated RETAS ff06 header");

    const auto header_size = big_u32(payload, 0);
    const auto mirrored_count = big_u32(payload, 4);
    const auto color_count = big_u32(payload, 8);
    const auto mark_count = big_u32(payload, 12);
    const auto width = big_u32(payload, 16);
    const auto height = big_u32(payload, 20);
    if (header_size != 24)
        throw FormatError(0, "Unsupported RETAS ff06 header size");
    if (mirrored_count != color_count)
        throw FormatError(4, "RETAS ff06 color counts disagree");
    if (width != static_cast<std::uint32_t>(expected_width) ||
        height != static_cast<std::uint32_t>(expected_height))
        throw FormatError(16, "RETAS ff06 geometry mismatch");
    if (color_count > max_ff06_color_records ||
        mark_count > max_ff06_color_records)
        throw FormatError(8, "RETAS ff06 record count exceeds supported limit");

    Raster raster(expected_width, expected_height, 32, resolution);
    std::size_t position = header_size;
    std::uint64_t decoded_bitmap_bytes = 0;
    std::uint64_t painted_pixels = 0;
    std::set<std::uint32_t> colors;
    for (std::uint32_t index = 0; index < color_count; ++index) {
        if (payload.size() - position < 32)
            throw FormatError(position, "Truncated RETAS ff06 color record");
        const auto record_size = big_u32(payload, position);
        const auto left = big_u32(payload, position + 4);
        const auto top = big_u32(payload, position + 8);
        const auto width_value = big_u32(payload, position + 12);
        const auto height_value = big_u32(payload, position + 16);
        const auto stride = big_u32(payload, position + 20);
        const auto color = big_u32(payload, position + 24);
        const auto encoded_size = big_u32(payload, position + 28);
        if (record_size != 32)
            throw FormatError(position, "Unsupported RETAS ff06 color-record size");
        if (!width_value || !height_value ||
            std::uint64_t(left) + width_value > width ||
            std::uint64_t(top) + height_value > height)
            throw FormatError(position + 4, "RETAS ff06 color rectangle is out of bounds");
        const std::size_t expected_stride = (std::size_t(width_value) + 7) / 8;
        if (stride != expected_stride)
            throw FormatError(position + 20, "RETAS ff06 bitmap stride mismatch");
        const std::uint64_t raw_size_64 = std::uint64_t(stride) * height_value;
        if (raw_size_64 > max_archive_bytes - decoded_bitmap_bytes)
            throw FormatError(position + 20, "RETAS ff06 bitmap exceeds supported size");
        decoded_bitmap_bytes += raw_size_64;
        if (encoded_size > payload.size() - position - record_size)
            throw FormatError(position + 28, "Truncated RETAS ff06 color bitmap");
        if (!colors.insert(color & 0x00ffffffu).second)
            throw FormatError(position + 24, "Duplicate RETAS ff06 color record");

        const auto encoded = payload.subspan(position + record_size, encoded_size);
        const auto mask = decode_packbits(encoded, static_cast<std::size_t>(raw_size_64),
                                          position + record_size);
        const unsigned used = width_value & 7;
        if (used) {
            const std::uint8_t padding_mask = static_cast<std::uint8_t>(0xffu >> used);
            for (std::uint32_t y = 0; y < height_value; ++y) {
                if (mask[std::size_t(y) * stride + stride - 1] & padding_mask)
                    throw FormatError(position + record_size,
                                      "RETAS ff06 bitmap has set padding bits");
            }
        }

        const auto pixel = Pixel::rgba8(color & 0xff, (color >> 8) & 0xff,
                                        (color >> 16) & 0xff);
        std::uint64_t record_pixels = 0;
        for (std::uint32_t y = 0; y < height_value; ++y) {
            const auto row = std::span(mask).subspan(std::size_t(y) * stride, stride);
            for (std::uint32_t byte_index = 0; byte_index < stride; ++byte_index) {
                unsigned valid_bits = 8;
                std::uint8_t value = row[byte_index];
                if (byte_index + 1 == stride && used) {
                    valid_bits = used;
                    value &= static_cast<std::uint8_t>(0xffu << (8 - used));
                }
                record_pixels += valid_bits - std::popcount(value);
            }
        }
        const auto canvas_pixels = std::uint64_t(width) * height;
        if (record_pixels > canvas_pixels - painted_pixels)
            throw FormatError(position + record_size,
                              "RETAS ff06 color masks overlap or exceed the canvas");
        painted_pixels += record_pixels;
        for (std::uint32_t y = 0; y < height_value; ++y) {
            for (std::uint32_t x = 0; x < width_value; ++x) {
                const auto byte = mask[std::size_t(y) * stride + x / 8];
                if (!(byte & (0x80u >> (x & 7)))) {
                    const int destination_x = static_cast<int>(left + x);
                    const int destination_y = static_cast<int>(top + y);
                    if (raster.pixel(destination_x, destination_y).a)
                        throw FormatError(position + record_size,
                                          "Overlapping RETAS ff06 color masks");
                    raster.set_pixel(static_cast<int>(left + x),
                                     static_cast<int>(top + y), pixel);
                }
            }
        }
        position += std::size_t(record_size) + encoded_size;
    }

    for (std::uint32_t index = 0; index < mark_count; ++index) {
        if (payload.size() - position < 12)
            throw FormatError(position, "Truncated RETAS ff06 mark record");
        const auto record_size = big_u32(payload, position);
        if (record_size < 12)
            throw FormatError(position, "Unsupported RETAS ff06 mark-record size");
        if (record_size > payload.size() - position)
            throw FormatError(position, "Truncated RETAS ff06 mark record");
        position += record_size;
    }
    if (position != payload.size())
        throw FormatError(position, "Trailing RETAS ff06 data");
    raster.prune_empty();
    return {std::move(raster), mark_count != 0};
}

Bytes encode_retas_ff06_raster(const Raster& raster) {
    struct ColorBounds {
        std::uint32_t color{};
        int left{}, top{}, right{}, bottom{};
    };
    std::vector<ColorBounds> colors;
    std::unordered_map<std::uint32_t, std::size_t> color_indices;
    for (int y = 0; y < raster.height; ++y) {
        for (int x = 0; x < raster.width; ++x) {
            const auto pixel = raster.pixel(x, y);
            if (!pixel.a ||
                (pixel.r == 65535 && pixel.g == 65535 && pixel.b == 65535))
                continue;
            if (pixel.a != 65535)
                throw std::invalid_argument("RETAS ff06 supports only opaque colors");
            const std::uint32_t color =
                (std::uint32_t(pixel.b / 257) << 16) |
                (std::uint32_t(pixel.g / 257) << 8) |
                std::uint32_t(pixel.r / 257);
            const auto [found, inserted] = color_indices.emplace(color, colors.size());
            if (inserted) {
                if (colors.size() >= max_ff06_color_records)
                    throw std::length_error("Too many RETAS ff06 colors");
                colors.push_back({color, x, y, x + 1, y + 1});
            }
            else {
                auto& bounds = colors[found->second];
                bounds.left = std::min(bounds.left, x);
                bounds.top = std::min(bounds.top, y);
                bounds.right = std::max(bounds.right, x + 1);
                bounds.bottom = std::max(bounds.bottom, y + 1);
            }
        }
    }
    Bytes output;
    append_big_u32(output, 24);
    append_big_u32(output, static_cast<std::uint32_t>(colors.size()));
    append_big_u32(output, static_cast<std::uint32_t>(colors.size()));
    append_big_u32(output, 0);
    append_big_u32(output, static_cast<std::uint32_t>(raster.width));
    append_big_u32(output, static_cast<std::uint32_t>(raster.height));

    for (const auto& bounds : colors) {
        const auto width = static_cast<std::uint32_t>(bounds.right - bounds.left);
        const auto height = static_cast<std::uint32_t>(bounds.bottom - bounds.top);
        const auto stride = (width + 7) / 8;
        const std::uint64_t raw_size_64 = std::uint64_t(stride) * height;
        if (raw_size_64 > max_archive_bytes)
            throw std::length_error("RETAS ff06 bitmap exceeds supported size");
        Bytes mask(static_cast<std::size_t>(raw_size_64), 0);
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto pixel = raster.pixel(bounds.left + static_cast<int>(x),
                                                bounds.top + static_cast<int>(y));
                const std::uint32_t color =
                    (std::uint32_t(pixel.b / 257) << 16) |
                    (std::uint32_t(pixel.g / 257) << 8) |
                    std::uint32_t(pixel.r / 257);
                const bool matches = pixel.a == 65535 && color == bounds.color;
                if (!matches)
                    mask[std::size_t(y) * stride + x / 8] |=
                        static_cast<std::uint8_t>(0x80u >> (x & 7));
            }
        }
        const auto encoded = encode_packbits_rows(mask, stride, height);
        if (encoded.size() > std::numeric_limits<std::uint32_t>::max() ||
            output.size() + 32 + encoded.size() > max_archive_bytes)
            throw std::length_error("RETAS ff06 payload exceeds supported size");
        append_big_u32(output, 32);
        append_big_u32(output, static_cast<std::uint32_t>(bounds.left));
        append_big_u32(output, static_cast<std::uint32_t>(bounds.top));
        append_big_u32(output, width);
        append_big_u32(output, height);
        append_big_u32(output, stride);
        append_big_u32(output, bounds.color);
        append_big_u32(output, static_cast<std::uint32_t>(encoded.size()));
        output.insert(output.end(), encoded.begin(), encoded.end());
    }
    return output;
}

Raster render_retas_image_base(const DocumentData& document) {
    return render_retas_image_base(document,32);
}
Raster render_retas_image_base(const DocumentData& document,int target_depth) {return render_retas_image_base(document,target_depth,false);}
Raster render_retas_image_base(const DocumentData& document,int target_depth,bool white_background) {
    white_background=white_background&&!std::ranges::any_of(document.layers,[](const Layer& l){return l.family==0x11001000u&&l.common.flags[1];});
    if(target_depth!=32&&target_depth!=64)throw std::invalid_argument("RETAS image output depth must be 32 or 64");
    DocumentData staged = document;
    for (auto& layer : staged.layers) {
        if (layer.family != 0x11000800 && layer.family != 0x11001000) continue;
        // The same host branch renders these production color families even
        // when their palette visibility is off, and temporarily uses 100%.
        layer.common.flags[1] = 1;
        layer.common.opacity = 100;
        for (auto& plane : layer.planes) plane.common.flags[1] = 1;
    }
    if(auto native=vector_render_detail::render_native_line_document(staged,target_depth,white_background?vector_render_detail::LineDocumentOutput::StandardImageWhite:vector_render_detail::LineDocumentOutput::StandardImage))return std::move(*native);
    const bool has_adjust=std::any_of(staged.layers.begin(),staged.layers.end(),[](const Layer& l){return l.family==0x40000200;});
    const auto excluded=[](const Layer& l){return l.family==0x10008000||l.family==0x10040000;};
    if(has_adjust){
        // 43FE40 consumes single-owner scope on the full saved stack, even
        // when the following request excludes that owner from drawing.
        for(auto& l:staged.layers)if(excluded(l)){Layer placeholder;placeholder.family=0x10000100;placeholder.common.flags[1]=0;l=std::move(placeholder);}
    }else std::erase_if(staged.layers,excluded);
    if (staged.has_unrendered_content())
        throw std::logic_error("Cannot render an incomplete RETAS document");
    return staged.composite(target_depth,{},white_background);
}

RetasPreparedImageResources prepare_retas_image_resources(
    const DocumentData& document,
    const RetasResourceArchive* existing) {
    RetasPreparedImageResources prepared;
    prepared.has_unrendered_document_content = document.has_unrendered_content();
    bool regenerate_model_thumbnail = false;
    if (existing) {
        for (const auto& resource : existing->resources) {
            if (resource.id == retas_resource_model_thumbnail) {
                bool valid_thumbnail = false;
                try { valid_thumbnail = decode_model_thumbnail(resource.payload).has_value(); }
                catch (const FormatError&) {}
                catch (const std::invalid_argument&) {}
                if (!valid_thumbnail) add_unsupported(prepared, resource.id);
                else regenerate_model_thumbnail = true;
                continue;
            }
            const bool regenerated = resource.id == retas_resource_preview_small ||
                                     resource.id == retas_resource_preview_large ||
                                     resource.id == retas_resource_mainline_mask ||
                                     resource.id == retas_resource_document_data;
            if (!regenerated) add_unsupported(prepared, resource.id);
        }
    }
    if (prepared.has_unrendered_document_content) return prepared;

    const auto composite = render_retas_image_base(document);
    if (regenerate_model_thumbnail) {
        const auto thumbnail = make_model_thumbnail(composite);
        if (thumbnail.image) prepared.updates.push_back({retas_resource_model_thumbnail, encode_model_thumbnail(thumbnail)});
        else add_unsupported(prepared, retas_resource_model_thumbnail);
    }
    bool small_matches_native = false;
    bool large_matches_native = false;
    auto small_preview = make_preview(composite, 64, 48, small_matches_native);
    auto large_preview = make_preview(composite, 160, 120, large_matches_native);
    prepared.updates.push_back({retas_resource_preview_small,
                                encode_tga(small_preview, {true, 32})});
    prepared.updates.push_back({retas_resource_preview_large,
                                encode_tga(large_preview, {true, 32})});
    prepared.previews_match_native_pixels =
        small_matches_native && large_matches_native;
    if (const auto mainline = exported_mainline(document)) {
        if (structural_resource_plane(*mainline, composite)) {
            prepared.updates.push_back(
                {retas_resource_mainline_mask,
                 encode_retas_nonwhite_mask(*mainline->plane->raster)});
        } else {
            add_unsupported(prepared, retas_resource_mainline_mask);
        }
    } else if (existing && existing->find(retas_resource_mainline_mask)) {
        add_unsupported(prepared, retas_resource_mainline_mask);
    }
    if (const auto raster = exported_document_raster(document)) {
        if (structural_resource_plane(*raster, composite)) {
            Bytes payload;
            bool can_regenerate = true;
            const auto* retained = existing
                ? existing->find(retas_resource_document_data)
                : nullptr;
            if (retained) {
                const auto decoded = decode_retas_ff06_raster(
                    retained->payload, composite.width, composite.height,
                    raster->plane->raster->resolution);
                if (decoded.has_unrestored_mark_records) {
                    add_unsupported(prepared, retas_resource_document_data);
                    can_regenerate = false;
                } else if (decoded.raster.same_pixels(*raster->plane->raster)) {
                    payload = retained->payload;
                }
            }
            if (payload.empty() && can_regenerate)
                payload = encode_retas_ff06_raster(*raster->plane->raster);
            if (!payload.empty())
                prepared.updates.push_back(
                    {retas_resource_document_data, std::move(payload)});
        } else {
            add_unsupported(prepared, retas_resource_document_data);
        }
    } else if (existing && existing->find(retas_resource_document_data)) {
        add_unsupported(prepared, retas_resource_document_data);
    }
    return prepared;
}

RetasDecodedResources decode_retas_image_resources(
    const RetasResourceArchive& archive,
    int image_width,
    int image_height,
    double image_resolution) {
    const std::size_t expected_mask_size = checked_mask_size(image_width, image_height);
    RetasDecodedResources decoded;
    for (const auto& resource : archive.resources) {
        switch (resource.id) {
            case retas_resource_model_thumbnail: {
                std::optional<ModelThumbnail> thumbnail;
                try { thumbnail = decode_model_thumbnail(resource.payload); }
                catch (const FormatError&) {} // Retain malformed preview bytes through ExactRaw.
                catch (const std::invalid_argument&) {}
                if (thumbnail && thumbnail->image)
                    decoded.auxiliary_rasters.emplace(resource.id, std::move(*thumbnail->image));
                else add_unrestored(decoded, resource.id);
                break;
            }
            case retas_resource_preview_small:
            case retas_resource_preview_large:
                decoded.auxiliary_rasters.emplace(resource.id, decode_tga(resource.payload));
                break;
            case retas_resource_grad_auxiliary_a:
            case retas_resource_grad_auxiliary_b:
                decoded.auxiliary_rasters.emplace(resource.id, decode_tga(resource.payload));
                add_unrestored(decoded, resource.id);
                break;
            case retas_resource_mainline_mask: {
                if (resource.payload.size() != expected_mask_size)
                    throw FormatError(resource.record_offset + 2,
                                      "RETAS main-line mask geometry mismatch");
                const unsigned used = static_cast<unsigned>(image_width & 7);
                if (used) {
                    const std::size_t stride = (static_cast<std::size_t>(image_width) + 7) / 8;
                    const std::uint8_t padding_mask = static_cast<std::uint8_t>(0xffu >> used);
                    for (int y = 0; y < image_height; ++y) {
                        if (resource.payload[static_cast<std::size_t>(y) * stride + stride - 1] &
                            padding_mask)
                            throw FormatError(resource.record_offset + 2,
                                              "RETAS main-line mask has set padding bits");
                    }
                }
                decoded.mainline_mask = resource.payload;
                break;
            }
            case retas_resource_document_data:
                decoded.opaque_document_data = resource.payload;
                {
                    auto ff06 = decode_retas_ff06_raster(resource.payload, image_width,
                                                         image_height, image_resolution);
                    if (ff06.has_unrestored_mark_records)
                        add_unrestored(decoded, resource.id);
                    decoded.document_raster = std::move(ff06.raster);
                }
                break;
            default:
                add_unrestored(decoded, resource.id);
                break;
        }
    }
    return decoded;
}

RetasRasterImport restore_retas_raster_document(
    const Raster& source,
    const RetasResourceArchive& archive,
    Bytes layer_name) {
    auto resources = decode_retas_image_resources(archive, source.width, source.height,
                                                  source.resolution);
    if (layer_name.empty()) layer_name = ascii("Imported Image");

    DocumentData document = make_document(source.width, source.height, 32,
                                          source.resolution);
    auto& layer = document.layers.front();
    layer.common.name = std::move(layer_name);
    layer.common.depth = 32;

    const bool grad_pair = archive.find(retas_resource_grad_auxiliary_a) &&
                           archive.find(retas_resource_grad_auxiliary_b);
    if (!resources.mainline_mask || grad_pair) {
        auto& plane = layer.planes.front();
        plane.common = layer.common;
        plane.common.runtime_identity=next_element_identity();
        plane.raster = raster32_copy(source);
        plane.pixels_changed = true;
        if (grad_pair && resources.mainline_mask)
            add_unrestored(resources, retas_resource_mainline_mask);
        document.unrestored_external_content = resources.has_unrestored_semantics();
        const bool restored_document_raster = resources.document_raster.has_value();
        attach_document_raster(document, resources);
        return {std::move(document), std::move(resources), false,
                restored_document_raster};
    }

    layer.family = 0x11000800;
    layer.current_plane = 0;
    layer.planes.clear();

    Plane color;
    color.family = 0x12100001;
    color.common = layer.common;
    color.common.runtime_identity=next_element_identity();
    color.common.name = ascii("Color");
    color.common.opacity_mode = 5;
    color.raster.emplace(source.width, source.height, 32, source.resolution);
    color.pixels_changed = true;

    Plane mainline;
    mainline.family = 0x11110002;
    mainline.common = layer.common;
    mainline.common.runtime_identity=next_element_identity();
    mainline.common.name = ascii("Main Line");
    mainline.common.opacity_mode = 5;
    mainline.raster.emplace(source.width, source.height, 32, source.resolution);
    mainline.pixels_changed = true;

    const auto& mask = *resources.mainline_mask;
    const std::size_t stride = (static_cast<std::size_t>(source.width) + 7) / 8;
    for (int y = 0; y < source.height; ++y) {
        for (int x = 0; x < source.width; ++x) {
            auto pixel = source.pixel(x, y);
            if (pixel.r == 65535 && pixel.g == 65535 && pixel.b == 65535) continue;
            pixel.a = 65535;
            const bool is_mainline =
                (mask[static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) / 8] &
                 static_cast<std::uint8_t>(0x80u >> (x & 7))) != 0;
            if (is_mainline)
                mainline.raster->set_pixel(x, y, pixel);
            else
                color.raster->set_pixel(x, y, pixel);
        }
    }
    color.raster->prune_empty();
    mainline.raster->prune_empty();
    layer.planes.push_back(std::move(color));
    layer.planes.push_back(std::move(mainline));
    document.unrestored_external_content = resources.has_unrestored_semantics();
    const bool restored_document_raster = resources.document_raster.has_value();
    attach_document_raster(document, resources);
    return {std::move(document), std::move(resources), true,
            restored_document_raster};
}

} // namespace retas
