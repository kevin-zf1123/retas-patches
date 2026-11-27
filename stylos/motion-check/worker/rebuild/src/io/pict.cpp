#include "retas/pict.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace retas {
namespace {

constexpr std::size_t legacy_file_header_size = 512;
constexpr std::size_t maximum_input_size = 512ull * 1024 * 1024;
constexpr std::size_t maximum_source_pixels = maximum_input_size / sizeof(Pixel);
void admit(const PictReadContext& context,std::uint64_t work,std::size_t allocation=0) {
    if(context.admit)context.admit(work,allocation);
}

struct Rect {
    int top{};
    int left{};
    int bottom{};
    int right{};

    [[nodiscard]] int width() const { return right - left; }
    [[nodiscard]] int height() const { return bottom - top; }
    [[nodiscard]] bool contains(int x, int y) const {
        return x >= left && x < right && y >= top && y < bottom;
    }
};

std::int16_t signed_word(std::uint16_t value) {
    return std::bit_cast<std::int16_t>(value);
}

Rect read_rect(Reader& reader) {
    return {signed_word(reader.u16()), signed_word(reader.u16()),
            signed_word(reader.u16()), signed_word(reader.u16())};
}

void write_rect(Writer& writer, const Rect& rect) {
    writer.u16(static_cast<std::uint16_t>(rect.top));
    writer.u16(static_cast<std::uint16_t>(rect.left));
    writer.u16(static_cast<std::uint16_t>(rect.bottom));
    writer.u16(static_cast<std::uint16_t>(rect.right));
}

void require_rect(const Rect& rect, std::size_t offset, const char* description) {
    if (rect.width() <= 0 || rect.height() <= 0) {
        throw FormatError(offset, std::string("Invalid ") + description + " rectangle");
    }
}

std::uint16_t word_at(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return static_cast<std::uint16_t>((unsigned(bytes[offset]) << 8U) | bytes[offset + 1]);
}

bool has_v2_stanza(std::span<const std::uint8_t> bytes, std::size_t offset) {
    return offset <= bytes.size() && bytes.size() - offset >= 16 &&
           word_at(bytes, offset + 10) == 0x0011 &&
           word_at(bytes, offset + 12) == 0x02ff &&
           word_at(bytes, offset + 14) == 0x0c00;
}

std::size_t picture_offset(std::span<const std::uint8_t> bytes) {
    if (bytes.size() >= legacy_file_header_size &&
        has_v2_stanza(bytes, legacy_file_header_size)) {
        return legacy_file_header_size;
    }
    if (has_v2_stanza(bytes, 0)) return 0;
    throw FormatError(0, "Not a supported PICT version-2 picture");
}

void align_v2_opcode(Reader& reader, std::size_t payload_offset) {
    if ((reader.offset() - payload_offset) & 1U) {
        const auto padding_offset = reader.offset();
        if (reader.u8() != 0) {
            throw FormatError(padding_offset, "Nonzero PICT opcode alignment byte");
        }
    }
}

struct RegionBand {
    int top{};
    int bottom{};
    std::vector<std::pair<int, int>> spans;
};

struct Region {
    Rect bounds;
    bool rectangular{true};
    std::vector<RegionBand> bands;

    [[nodiscard]] bool contains(int x, int y) const {
        if (!bounds.contains(x, y)) return false;
        if (rectangular) return true;
        const auto band = std::upper_bound(
            bands.begin(), bands.end(), y,
            [](int value, const RegionBand& item) { return value < item.top; });
        if (band == bands.begin()) return false;
        const auto& candidate = *std::prev(band);
        if (y >= candidate.bottom) return false;
        const auto span = std::upper_bound(
            candidate.spans.begin(), candidate.spans.end(), x,
            [](int value, const auto& item) { return value < item.first; });
        if (span == candidate.spans.begin()) return false;
        const auto& candidate_span = *std::prev(span);
        return x < candidate_span.second;
    }
};

std::vector<std::pair<int, int>> endpoint_spans(const std::set<int>& endpoints,
                                                std::size_t offset) {
    if (endpoints.size() % 2U) {
        throw FormatError(offset, "PICT region has an odd endpoint count");
    }
    std::vector<std::pair<int, int>> spans;
    for (auto it = endpoints.begin(); it != endpoints.end();) {
        const int left = *it++;
        const int right = *it++;
        if (right <= left) throw FormatError(offset, "PICT region has an empty span");
        spans.emplace_back(left, right);
    }
    return spans;
}

Region read_region(Reader& reader,const PictReadContext& context) {
    const auto region_offset = reader.offset();
    const auto size = reader.u16();
    if (size < 10 || (size & 1U) || size - 2U > reader.remaining()) {
        throw FormatError(region_offset, "Invalid PICT region size");
    }
    Reader body(reader.take(size - 2U), Endian::Big, region_offset + 2U);
    Region result;
    result.bounds = read_rect(body);
    if (result.bounds.right < result.bounds.left ||
        result.bounds.bottom < result.bounds.top) {
        throw FormatError(region_offset + 2U, "Inverted PICT region bounds");
    }
    if (!body.remaining()) return result;

    result.rectangular = false;
    std::set<int> endpoints;
    int previous_y = result.bounds.top;
    bool terminated = false;
    while (body.remaining()) {
        admit(context,1);
        const auto event_offset = body.offset();
        const int y = signed_word(body.u16());
        if (y == 0x7fff) {
            terminated = true;
            break;
        }
        if (y < previous_y || y < result.bounds.top || y > result.bounds.bottom) {
            throw FormatError(event_offset, "PICT region scanlines are out of order");
        }
        if (y > previous_y && !endpoints.empty()) {
            admit(context,endpoints.size(),endpoints.size()*16+128);
            result.bands.push_back(
                {previous_y, y, endpoint_spans(endpoints, event_offset)});
        }

        int previous_x = std::numeric_limits<int>::min();
        bool row_terminated = false;
        while (body.remaining()) {
            admit(context,1);
            const auto x_offset = body.offset();
            const int x = signed_word(body.u16());
            if (x == 0x7fff) {
                row_terminated = true;
                break;
            }
            if (x <= previous_x) {
                throw FormatError(x_offset, "PICT region endpoints are not increasing");
            }
            previous_x = x;
            if (endpoints.contains(x)) endpoints.erase(x);
            else {admit(context,0,64);endpoints.insert(x);}
        }
        if (!row_terminated) throw FormatError(body.offset(), "Unterminated PICT region row");
        previous_y = y;
    }
    if (!terminated || body.remaining()) {
        throw FormatError(body.offset(), "Malformed PICT region terminator");
    }
    if (previous_y < result.bounds.bottom && !endpoints.empty()) {
        admit(context,endpoints.size(),endpoints.size()*16+128);
        result.bands.push_back(
            {previous_y, result.bounds.bottom, endpoint_spans(endpoints, body.offset())});
    }
    return result;
}

Pixel classic_color(std::uint32_t value) {
    switch (value) {
    case 30: return Pixel::rgba8(255, 255, 255);
    case 33: return Pixel::rgba8(0, 0, 0);
    case 69: return Pixel::rgba8(255, 255, 0);
    case 137: return Pixel::rgba8(255, 0, 255);
    case 205: return Pixel::rgba8(255, 0, 0);
    case 273: return Pixel::rgba8(0, 255, 255);
    case 341: return Pixel::rgba8(0, 255, 0);
    case 409: return Pixel::rgba8(0, 0, 255);
    default: throw FormatError(0, "Unsupported indexed QuickDraw color constant");
    }
}

Pixel read_rgb_color(Reader& reader) {
    const auto red = reader.u16();
    const auto green = reader.u16();
    const auto blue = reader.u16();
    return Pixel::rgba8(red >> 8U, green >> 8U, blue >> 8U);
}

Bytes unpack_packbits(std::span<const std::uint8_t> packed,
                      std::size_t expected_size, std::size_t unit_size,
                      std::size_t origin) {
    Reader reader(packed, Endian::Big, origin);
    Bytes output;
    output.reserve(expected_size);
    while (output.size() < expected_size) {
        const auto control_offset = reader.offset();
        const auto control = std::bit_cast<std::int8_t>(reader.u8());
        if (control >= 0) {
            const auto units = static_cast<std::size_t>(control) + 1U;
            if (units > (expected_size - output.size()) / unit_size) {
                throw FormatError(control_offset, "PICT PackBits literal exceeds row");
            }
            const auto literal = reader.take(units * unit_size);
            output.insert(output.end(), literal.begin(), literal.end());
        } else if (control != std::numeric_limits<std::int8_t>::min()) {
            const auto units = static_cast<std::size_t>(1 - int(control));
            if (units > (expected_size - output.size()) / unit_size) {
                throw FormatError(control_offset, "PICT PackBits run exceeds row");
            }
            const auto value = reader.take(unit_size);
            for (std::size_t i = 0; i < units; ++i) {
                output.insert(output.end(), value.begin(), value.end());
            }
        }
    }
    if (output.size() != expected_size) {
        throw FormatError(reader.offset(), "Incomplete PICT PackBits row");
    }
    while (reader.remaining()) {
        const auto padding_offset = reader.offset();
        if (reader.u8() != 0x80U) {
            throw FormatError(padding_offset, "PICT PackBits row has trailing data");
        }
    }
    return output;
}

Bytes read_packed_row(Reader& reader, std::size_t row_bytes,
                      std::size_t expected_size, std::size_t unit_size) {
    const auto packed_size = static_cast<std::size_t>(
        row_bytes > 250U ? reader.u16() : reader.u8());
    const auto origin = reader.offset();
    return unpack_packbits(reader.take(packed_size), expected_size, unit_size, origin);
}

struct PixelMap {
    Rect bounds;
    std::size_t row_bytes{};
    unsigned pack_type{};
    unsigned pixel_type{};
    unsigned pixel_size{};
    unsigned component_count{};
    unsigned component_size{};
    bool bitmap{};
    std::map<unsigned, Pixel> palette;
};

void validate_source_allocation(const PixelMap& map, std::size_t offset) {
    require_rect(map.bounds, offset, "PICT source bounds");
    const auto width = static_cast<std::uint64_t>(map.bounds.width());
    const auto height = static_cast<std::uint64_t>(map.bounds.height());
    if (width > maximum_source_pixels || height > maximum_source_pixels ||
        width * height > maximum_source_pixels) {
        throw FormatError(offset, "PICT source raster exceeds allocation limit");
    }
}

void read_pixmap_tail(Reader& reader, PixelMap& map, std::size_t offset) {
    const auto version = reader.u16();
    map.pack_type = reader.u16();
    reader.u32(); // packSize is unused for picture scanline packing.
    const auto horizontal_resolution = std::bit_cast<std::int32_t>(reader.u32());
    const auto vertical_resolution = std::bit_cast<std::int32_t>(reader.u32());
    map.pixel_type = reader.u16();
    map.pixel_size = reader.u16();
    map.component_count = reader.u16();
    map.component_size = reader.u16();
    reader.u32(); // planeBytes
    reader.u32(); // pmTable handle
    reader.u32(); // pmReserved
    if ((version != 0 && version != 4) || horizontal_resolution <= 0 ||
        vertical_resolution <= 0) {
        throw FormatError(offset, "Unsupported PICT PixMap version or resolution");
    }
}

PixelMap read_direct_pixmap(Reader& reader) {
    const auto offset = reader.offset();
    const auto base_address = reader.u32();
    const auto tagged_row_bytes = reader.u16();
    PixelMap map;
    map.row_bytes = tagged_row_bytes & 0x3fffU;
    map.bounds = read_rect(reader);
    read_pixmap_tail(reader, map, offset);
    validate_source_allocation(map, offset);
    const auto bytes_per_pixel = map.pixel_size / 8U;
    const auto minimum_row = static_cast<std::uint64_t>(map.bounds.width()) * bytes_per_pixel;
    // 0x000000ff is canonical.  Several classic little-endian writers stored
    // that placeholder byte-swapped as 0xff000000; it is never dereferenced.
    if ((base_address != 0 && base_address != 0xffU && base_address != 0xff000000U) ||
        !(tagged_row_bytes & 0x8000U) ||
        (tagged_row_bytes & 0x4000U) || !map.row_bytes || (map.row_bytes & 1U) ||
        minimum_row > map.row_bytes || map.pixel_type != 16 ||
        (map.pixel_size != 16 && map.pixel_size != 32) ||
        (map.pixel_size == 16 &&
         (map.component_count != 3 || map.component_size != 5)) ||
        (map.pixel_size == 32 &&
         ((map.component_count != 3 && map.component_count != 4) ||
          map.component_size != 8)) || map.pack_type > 4) {
        throw FormatError(offset, "Unsupported PICT direct PixMap layout");
    }
    return map;
}

PixelMap read_indexed_pixmap_or_bitmap(Reader& reader,const PictReadContext& context) {
    const auto offset = reader.offset();
    const auto tagged_row_bytes = reader.u16();
    PixelMap map;
    map.row_bytes = tagged_row_bytes & 0x3fffU;
    map.bounds = read_rect(reader);
    map.bitmap = !(tagged_row_bytes & 0x8000U);
    validate_source_allocation(map, offset);
    if (!map.row_bytes) throw FormatError(offset, "PICT bitmap has zero rowBytes");
    if (map.bitmap) {
        map.pixel_size = 1;
        map.component_count = 1;
        map.component_size = 1;
        const auto minimum_row = (static_cast<std::uint64_t>(map.bounds.width()) + 7U) / 8U;
        if (minimum_row > map.row_bytes) {
            throw FormatError(offset, "PICT bitmap rowBytes is too small");
        }
        return map;
    }
    if (tagged_row_bytes & 0x4000U) {
        throw FormatError(offset, "Unsupported PICT PixMap rowBytes flags");
    }
    read_pixmap_tail(reader, map, offset);
    if (map.pixel_type != 0 ||
        (map.pixel_size != 1 && map.pixel_size != 2 && map.pixel_size != 4 &&
         map.pixel_size != 8) ||
        map.component_count != 1 || map.component_size != map.pixel_size ||
        (map.pack_type != 0 && map.pack_type != 1)) {
        throw FormatError(offset, "Unsupported PICT indexed PixMap layout");
    }
    const auto minimum_row =
        (static_cast<std::uint64_t>(map.bounds.width()) * map.pixel_size + 7U) / 8U;
    if (minimum_row > map.row_bytes) {
        throw FormatError(offset, "PICT indexed PixMap rowBytes is too small");
    }

    reader.u32();
    const auto flags = reader.u16();
    const auto color_count = static_cast<std::size_t>(reader.u16()) + 1U;
    if ((flags & 0x4000U) || color_count > (std::size_t{1} << map.pixel_size)) {
        throw FormatError(reader.offset() - 4U, "Unsupported PICT color table");
    }
    const bool sequential = (flags & 0x8000U) != 0;
    admit(context,color_count,color_count*64);
    for (std::size_t i = 0; i < color_count; ++i) {
        const auto entry_offset = reader.offset();
        const auto stored_value = reader.u16();
        const auto value = sequential ? static_cast<unsigned>(i) : stored_value;
        const auto color = read_rgb_color(reader);
        if (value >= (1U << map.pixel_size) || map.palette.contains(value)) {
            throw FormatError(entry_offset, "Invalid or duplicate PICT color-table index");
        }
        map.palette.emplace(value, color);
    }
    return map;
}

std::vector<Pixel> read_direct_pixels(Reader& reader, const PixelMap& map,const PictReadContext& context) {
    const auto width = static_cast<std::size_t>(map.bounds.width());
    const auto height = static_cast<std::size_t>(map.bounds.height());
    const auto storage_pixels = map.row_bytes / (map.pixel_size / 8U);
    admit(context,width*height+map.row_bytes*height,width*height*sizeof(Pixel)+map.row_bytes*height*2);
    std::vector<Pixel> pixels(width * height);
    unsigned packing = map.row_bytes < 8U ? 1U : map.pack_type;
    if (packing == 0) packing = map.pixel_size == 16 ? 3U : 4U;
    if ((map.pixel_size == 16 && packing != 1 && packing != 3) ||
        (map.pixel_size == 32 && packing != 1 && packing != 2 && packing != 4) ||
        (packing == 2 && map.component_count != 3)) {
        throw FormatError(reader.offset(), "Unsupported PICT direct packing type");
    }

    for (std::size_t y = 0; y < height; ++y) {
        admit(context,0);
        Bytes row;
        if (packing == 1) row = reader.bytes(map.row_bytes);
        else if (packing == 2) row = reader.bytes(storage_pixels * 3U);
        else if (packing == 3) row = read_packed_row(reader, map.row_bytes, map.row_bytes, 2);
        else row = read_packed_row(reader, map.row_bytes,
                                   storage_pixels * map.component_count, 1);

        for (std::size_t x = 0; x < width; ++x) {
            Pixel pixel;
            if (map.pixel_size == 16) {
                const auto value = static_cast<unsigned>(row[x * 2U]) << 8U |
                                   row[x * 2U + 1U];
                const auto expand = [](unsigned component) {
                    return (component << 3U) | (component >> 2U);
                };
                pixel = Pixel::rgba8(expand((value >> 10U) & 31U),
                                     expand((value >> 5U) & 31U), expand(value & 31U));
            } else if (packing == 2) {
                pixel = Pixel::rgba8(row[x * 3U], row[x * 3U + 1U], row[x * 3U + 2U]);
            } else if (packing == 4) {
                const auto alpha_offset = map.component_count == 4 ? 0U : std::size_t(-1);
                const auto red_offset = map.component_count == 4 ? storage_pixels : 0U;
                const auto green_offset = red_offset + storage_pixels;
                const auto blue_offset = green_offset + storage_pixels;
                pixel = Pixel::rgba8(row[red_offset + x], row[green_offset + x],
                                     row[blue_offset + x],
                                     map.component_count == 4 ? row[alpha_offset + x] : 255U);
            } else {
                const auto base = x * 4U;
                pixel = Pixel::rgba8(row[base + 1U], row[base + 2U], row[base + 3U],
                                     map.component_count == 4 ? row[base] : 255U);
            }
            pixels[y * width + x] = pixel;
        }
    }
    return pixels;
}

std::vector<Pixel> read_indexed_pixels(Reader& reader, const PixelMap& map,
                                       bool packed, Pixel foreground, Pixel background,const PictReadContext& context) {
    const auto width = static_cast<std::size_t>(map.bounds.width());
    const auto height = static_cast<std::size_t>(map.bounds.height());
    if (!packed && map.row_bytes >= 8U) {
        throw FormatError(reader.offset(), "BitsRect requires rowBytes below eight");
    }
    admit(context,width*height+map.row_bytes*height,width*height*sizeof(Pixel)+map.row_bytes*height*2);
    std::vector<Pixel> pixels(width * height);
    for (std::size_t y = 0; y < height; ++y) {
        admit(context,0);
        Bytes row = packed && map.row_bytes >= 8U
                        ? read_packed_row(reader, map.row_bytes, map.row_bytes, 1)
                        : reader.bytes(map.row_bytes);
        for (std::size_t x = 0; x < width; ++x) {
            const auto bit = x * map.pixel_size;
            const auto shift = 8U - map.pixel_size - unsigned(bit % 8U);
            const auto value = (row[bit / 8U] >> shift) & ((1U << map.pixel_size) - 1U);
            if (map.bitmap) pixels[y * width + x] = value ? foreground : background;
            else {
                const auto found = map.palette.find(value);
                if (found == map.palette.end()) {
                    throw FormatError(reader.offset(),
                                      "PICT pixel references a missing color-table entry");
                }
                pixels[y * width + x] = found->second;
            }
        }
    }
    return pixels;
}

void blit(Raster& destination, const Rect& frame, const PixelMap& source_map,
          const std::vector<Pixel>& source, const Rect& source_rect,
          const Rect& destination_rect, const Region& clip,
          const std::optional<Region>& mask, unsigned mode, std::size_t offset,const PictReadContext& context) {
    require_rect(source_rect, offset, "PICT source");
    require_rect(destination_rect, offset, "PICT destination");
    if (source_rect.left < source_map.bounds.left ||
        source_rect.top < source_map.bounds.top ||
        source_rect.right > source_map.bounds.right ||
        source_rect.bottom > source_map.bounds.bottom) {
        throw FormatError(offset, "PICT source rectangle exceeds PixMap bounds");
    }
    if (mode != 0 && mode != 64) {
        throw FormatError(offset, "Unsupported PICT bitmap transfer mode");
    }

    const int top = std::max(destination_rect.top, frame.top);
    const int left = std::max(destination_rect.left, frame.left);
    const int bottom = std::min(destination_rect.bottom, frame.bottom);
    const int right = std::min(destination_rect.right, frame.right);
    admit(context,std::uint64_t(std::max(0,right-left))*std::max(0,bottom-top));
    const auto source_width = static_cast<std::size_t>(source_map.bounds.width());
    for (int y = top; y < bottom; ++y) {
        admit(context,0);
        if (!clip.contains(left, y) && clip.rectangular &&
            (y < clip.bounds.top || y >= clip.bounds.bottom)) continue;
        const auto source_y = source_rect.top +
            static_cast<int>(std::int64_t(y - destination_rect.top) * source_rect.height() /
                             destination_rect.height());
        for (int x = left; x < right; ++x) {
            if (!clip.contains(x, y) || (mask && !mask->contains(x, y))) continue;
            const auto source_x = source_rect.left +
                static_cast<int>(std::int64_t(x - destination_rect.left) * source_rect.width() /
                                 destination_rect.width());
            const auto index = static_cast<std::size_t>(source_y - source_map.bounds.top) *
                                   source_width +
                               static_cast<std::size_t>(source_x - source_map.bounds.left);
            destination.set_pixel(x - frame.left, y - frame.top, source[index]);
        }
    }
}

struct QuickTimeDescription {
    std::uint32_t codec{};
    int width{};
    int height{};
    std::uint32_t data_size{};
    unsigned depth{};
};

QuickTimeDescription read_quicktime_description(Reader& reader) {
    const auto description_offset = reader.offset();
    const auto size = reader.u32();
    if (size < 86 || size - 4U > reader.remaining()) {
        throw FormatError(description_offset, "Invalid QuickTime image description size");
    }
    Reader description(reader.take(size - 4U), Endian::Big, description_offset + 4U);
    QuickTimeDescription result;
    result.codec = description.u32();
    description.u32(); // reserved
    description.u16(); // reserved
    description.u16(); // data reference index
    const auto version = description.u16();
    description.u16(); // revision level
    description.u32(); // vendor
    description.u32(); // temporal quality
    description.u32(); // spatial quality
    result.width = description.u16();
    result.height = description.u16();
    const auto horizontal_resolution = std::bit_cast<std::int32_t>(description.u32());
    const auto vertical_resolution = std::bit_cast<std::int32_t>(description.u32());
    result.data_size = description.u32();
    const auto frame_count = description.u16();
    description.take(32); // Pascal compressor name
    result.depth = description.u16();
    description.u16(); // color-table id
    description.take(description.remaining()); // Size-bounded ImageDescription extensions.
    if (version > 1 || result.width <= 0 || result.height <= 0 ||
        horizontal_resolution <= 0 || vertical_resolution <= 0 || frame_count != 1 ||
        (result.depth != 16 && result.depth != 24 && result.depth != 32) ||
        static_cast<std::uint64_t>(result.width) *
                static_cast<std::uint64_t>(result.height) >
            maximum_source_pixels) {
        throw FormatError(description_offset,
                          "Unsupported QuickTime Animation image description");
    }
    return result;
}

Pixel read_animation_pixel(Reader& reader, unsigned depth) {
    if (depth == 16) {
        const auto value = reader.u16();
        const auto expand = [](unsigned component) {
            return (component << 3U) | (component >> 2U);
        };
        return Pixel::rgba8(expand((value >> 10U) & 31U),
                           expand((value >> 5U) & 31U), expand(value & 31U));
    }
    if (depth == 24) {
        const auto red = reader.u8();
        const auto green = reader.u8();
        const auto blue = reader.u8();
        return Pixel::rgba8(red, green, blue);
    }
    const auto alpha = reader.u8();
    const auto red = reader.u8();
    const auto green = reader.u8();
    const auto blue = reader.u8();
    return Pixel::rgba8(red, green, blue, alpha);
}

std::vector<Pixel> decode_quicktime_animation(std::span<const std::uint8_t> bytes,
                                              std::size_t origin, int width, int height,
                                              unsigned depth,const PictReadContext& context) {
    Reader reader(bytes, Endian::Big, origin);
    const auto declared = reader.u32() & 0x3fffffffU;
    if (declared < 6 || declared != bytes.size()) {
        throw FormatError(origin, "Invalid QuickTime Animation chunk size");
    }
    const auto header = reader.u16();
    int start_line = 0;
    int lines = height;
    if (header & 8U) {
        start_line = reader.u16();
        reader.u16();
        lines = reader.u16();
        reader.u16();
    }
    if ((header & ~8U) || start_line != 0 || lines != height) {
        throw FormatError(origin + 4U,
                          "Conditional QuickTime Animation frames are unsupported in PICT");
    }

    const auto pixel_width = static_cast<std::size_t>(width);
    const auto pixel_height = static_cast<std::size_t>(height);
    admit(context,pixel_width*pixel_height+bytes.size(),pixel_width*pixel_height*sizeof(Pixel));
    std::vector<Pixel> pixels(pixel_width * pixel_height);
    for (int y = 0; y < height; ++y) {
        admit(context,0);
        const auto line_offset = reader.offset();
        if (reader.u8() != 1) {
            throw FormatError(line_offset,
                              "QuickTime Animation keyframe begins with a pixel skip");
        }
        int x = 0;
        bool end_of_line = false;
        while (reader.remaining()) {
            const auto packet_offset = reader.offset();
            const auto code = std::bit_cast<std::int8_t>(reader.u8());
            if (code == -1) {
                end_of_line = true;
                break;
            }
            if (code == 0) {
                throw FormatError(packet_offset,
                                  "Conditional QuickTime Animation pixel skips are unsupported");
            }
            const int count = code < 0 ? -int(code) : int(code);
            if (count > width - x) {
                throw FormatError(packet_offset,
                                  "QuickTime Animation packet crosses scanline bounds");
            }
            if (code < 0) {
                const auto pixel = read_animation_pixel(reader, depth);
                std::fill_n(pixels.begin() + static_cast<std::ptrdiff_t>(
                                static_cast<std::size_t>(y) * pixel_width +
                                static_cast<std::size_t>(x)),
                            count, pixel);
                x += count;
            } else {
                for (int i = 0; i < count; ++i, ++x) {
                    pixels[static_cast<std::size_t>(y) * pixel_width +
                           static_cast<std::size_t>(x)] =
                        read_animation_pixel(reader, depth);
                }
            }
        }
        if (!end_of_line || x != width) {
            throw FormatError(line_offset,
                              "Incomplete QuickTime Animation keyframe scanline");
        }
    }
    if (reader.remaining()) {
        if (reader.remaining() == 1) {
            const auto padding_offset = reader.offset();
            if (reader.u8() == 0) return pixels;
            throw FormatError(padding_offset,
                              "Nonzero QuickTime Animation alignment byte");
        }
        throw FormatError(reader.offset(), "Trailing QuickTime Animation frame data");
    }
    return pixels;
}

void decode_compressed_quicktime(Reader& reader, Raster& destination,
                                 const Rect& frame, const Region& clip,
                                 std::size_t opcode_offset,const PictReadContext& context) {
    const auto payload_size = reader.u32();
    const auto payload_offset = reader.offset();
    if (payload_size > reader.remaining()) {
        throw FormatError(payload_offset, "Truncated PICT CompressedQuickTime payload");
    }
    Reader quicktime(reader.take(payload_size), Endian::Big, payload_offset);
    const auto version = quicktime.u16();
    std::array<std::uint32_t, 9> matrix{};
    for (auto& value : matrix) value = quicktime.u32();
    const auto matte_size = quicktime.u32();
    read_rect(quicktime); // matte rectangle
    const auto mode = quicktime.u16();
    const auto source_rect = read_rect(quicktime);
    quicktime.u32(); // preferred codec accuracy
    const auto mask_size = quicktime.u32();
    constexpr std::array<std::uint32_t, 9> identity{
        0x00010000U, 0, 0, 0, 0x00010000U, 0, 0, 0, 0x40000000U};
    if (version != 0 || matrix != identity || matte_size != 0) {
        throw FormatError(payload_offset,
                          "Unsupported transformed or matted PICT QuickTime image");
    }

    std::optional<Region> mask;
    if (mask_size) {
        const auto mask_offset = quicktime.offset();
        if (mask_size > quicktime.remaining()) {
            throw FormatError(mask_offset, "Truncated PICT QuickTime mask region");
        }
        Reader mask_reader(quicktime.take(mask_size), Endian::Big, mask_offset);
        mask = read_region(mask_reader,context);
        if (mask_reader.remaining()) {
            throw FormatError(mask_reader.offset(), "Trailing PICT QuickTime mask data");
        }
    }

    const auto description = read_quicktime_description(quicktime);
    if (description.codec != 0x726c6520U) {
        throw FormatError(quicktime.offset(),
                          "Unsupported PICT QuickTime compression codec");
    }
    const auto data_size = description.data_size ? description.data_size : quicktime.remaining();
    if (data_size > quicktime.remaining()) {
        throw FormatError(quicktime.offset(), "Truncated PICT QuickTime image data");
    }
    const auto data_offset = quicktime.offset();
    const auto pixels = decode_quicktime_animation(
        quicktime.take(data_size), data_offset, description.width, description.height,
        description.depth,context);
    if (quicktime.remaining()) {
        throw FormatError(quicktime.offset(), "Trailing PICT QuickTime opcode data");
    }

    PixelMap map;
    map.bounds = {0, 0, description.height, description.width};
    validate_source_allocation(map, opcode_offset);
    blit(destination, frame, map, pixels, source_rect, source_rect, clip, mask,
         mode, opcode_offset,context);
}

Bytes pack_packbits(std::span<const std::uint8_t> input) {
    Bytes output;
    output.reserve(input.size() + (input.size() + 127U) / 128U);
    std::size_t position = 0;
    while (position < input.size()) {
        std::size_t repeat = 1;
        while (position + repeat < input.size() && repeat < 128U &&
               input[position + repeat] == input[position]) ++repeat;
        if (repeat >= 3U) {
            output.push_back(static_cast<std::uint8_t>(257U - repeat));
            output.push_back(input[position]);
            position += repeat;
            continue;
        }

        const auto literal_start = position;
        position += repeat;
        while (position < input.size() && position - literal_start < 128U) {
            repeat = 1;
            while (position + repeat < input.size() && repeat < 128U &&
                   input[position + repeat] == input[position]) ++repeat;
            if (repeat >= 3U) break;
            const auto available = 128U - (position - literal_start);
            position += std::min(repeat, available);
        }
        const auto count = position - literal_start;
        output.push_back(static_cast<std::uint8_t>(count - 1U));
        output.insert(output.end(), input.begin() + static_cast<std::ptrdiff_t>(literal_start),
                      input.begin() + static_cast<std::ptrdiff_t>(position));
    }
    return output;
}

void write_packed_row(Writer& writer, std::size_t row_bytes,
                      std::span<const std::uint8_t> row) {
    const auto packed = pack_packbits(row);
    if (row_bytes > 250U) {
        if (packed.size() > std::numeric_limits<std::uint16_t>::max()) {
            throw std::length_error("PICT packed row exceeds 16-bit length");
        }
        writer.u16(static_cast<std::uint16_t>(packed.size()));
    } else {
        if (packed.size() > std::numeric_limits<std::uint8_t>::max()) {
            throw std::length_error("PICT packed row exceeds 8-bit length");
        }
        writer.u8(static_cast<std::uint8_t>(packed.size()));
    }
    writer.raw(packed);
}

void skip_word_sized_payload(Reader& reader) {
    const auto size = static_cast<std::size_t>(reader.u16());
    reader.take(size);
}

void skip_long_sized_payload(Reader& reader) {
    const auto size = static_cast<std::size_t>(reader.u32());
    reader.take(size);
}

void skip_polygon(Reader& reader) {
    const auto polygon_offset = reader.offset();
    const auto size = static_cast<std::size_t>(reader.u16());
    if (size < 10U || ((size - 10U) % 4U) != 0U || size - 2U > reader.remaining()) {
        throw FormatError(polygon_offset, "Invalid PICT polygon size");
    }
    Reader polygon(reader.take(size - 2U), Endian::Big, polygon_offset + 2U);
    const auto bounds = read_rect(polygon);
    if (bounds.right < bounds.left || bounds.bottom < bounds.top) {
        throw FormatError(polygon_offset + 2U, "Inverted PICT polygon bounds");
    }
    polygon.take(polygon.remaining());
}

} // namespace

Raster decode_pict(std::span<const std::uint8_t> bytes) {
    return decode_pict(bytes,{});
}
Raster decode_pict(std::span<const std::uint8_t> bytes,const PictReadContext& input_context) {
    const PictReadContext context=input_context;
    if (bytes.size() > maximum_input_size) {
        throw FormatError(0, "PICT file exceeds supported input limit");
    }
    const auto payload_offset = picture_offset(bytes);
    Reader reader(bytes.subspan(payload_offset), Endian::Big, payload_offset);
    reader.u16(); // picSize is explicitly not reliable for version-2 pictures.
    const auto frame_offset = reader.offset();
    const auto picture_frame = read_rect(reader);
    require_rect(picture_frame, frame_offset, "PICT frame");
    if (reader.u16() != 0x0011 || reader.u16() != 0x02ff || reader.u16() != 0x0c00) {
        throw FormatError(reader.offset() - 2U, "Unsupported PICT version stanza");
    }

    double resolution = 72.0;
    Rect frame = picture_frame;
    const auto header_kind = reader.u32();
    if (header_kind == 0xfffe0000U) {
        const auto horizontal = std::bit_cast<std::int32_t>(reader.u32());
        const auto vertical = std::bit_cast<std::int32_t>(reader.u32());
        const auto native_frame_offset = reader.offset();
        frame = read_rect(reader);
        reader.u32();
        if (horizontal <= 0 || vertical <= 0) {
            throw FormatError(reader.offset() - 20U, "Invalid PICT extended resolution");
        }
        require_rect(frame, native_frame_offset, "PICT native frame");
        resolution = double(horizontal) / 65536.0;
    } else if (header_kind == 0xffffffffU ||
               header_kind == bytes.size() ||
               header_kind == bytes.size() - payload_offset) {
        // OpenPicture writes 0xffffffff here.  Some widespread version-2
        // writers put the complete file or headerless payload size in the
        // same field; both forms are followed by the Fixed bounding rectangle.
        reader.take(16); // Fixed bounding rectangle.
        reader.u32();
    } else {
        throw FormatError(reader.offset() - 4U, "Unsupported PICT version-2 header kind");
    }
    const auto frame_pixels = static_cast<std::uint64_t>(frame.width()) *
                              static_cast<std::uint64_t>(frame.height());
    if (frame_pixels > 268435456ull || frame_pixels * 4U > maximum_input_size) {
        throw FormatError(frame_offset, "PICT frame exceeds allocation limit");
    }
    admit(context,0,std::size_t((frame.width()+255)/256)*((frame.height()+255)/256)*(256*256*4+128));
    Raster result(frame.width(), frame.height(), 32, resolution);
    Region clip;
    clip.bounds = frame;
    Pixel foreground = Pixel::rgba8(0, 0, 0);
    Pixel background = Pixel::rgba8(255, 255, 255);
    bool drew_raster = false;
    bool quicktime_fallback_pending = false;

    for (;;) {
        admit(context,1);
        align_v2_opcode(reader, payload_offset);
        const auto opcode_offset = reader.offset();
        const auto opcode = reader.u16();
        const bool quicktime_fallback_candidate = quicktime_fallback_pending;
        quicktime_fallback_pending = false;
        switch (opcode) {
        case 0x0000: break;
        case 0x0001: clip = read_region(reader,context); break;
        case 0x0002:
        case 0x0009:
        case 0x000a: reader.take(8); break;
        case 0x0003:
        case 0x0005:
        case 0x0008:
        case 0x000d:
        case 0x0015:
        case 0x0016: reader.take(2); break;
        case 0x0004: reader.take(1); break;
        case 0x0006:
        case 0x000b: reader.take(4); break;
        case 0x0007: {
            const auto vertical = reader.u16();
            const auto horizontal = reader.u16();
            if (quicktime_fallback_candidate && vertical == 0x00aeU) {
                reader.take(horizontal);
            }
            break;
        }
        case 0x000e: foreground = classic_color(reader.u32()); break;
        case 0x000f: background = classic_color(reader.u32()); break;
        case 0x0010: reader.take(8); break;
        case 0x001a: foreground = read_rgb_color(reader); break;
        case 0x001b: background = read_rgb_color(reader); break;
        case 0x001c:
        case 0x001e: break;
        case 0x001d:
        case 0x001f: reader.take(6); break;
        case 0x002c: {
            const auto length = reader.u16();
            reader.take(length);
            break;
        }
        case 0x002d: {
            const auto length = reader.u16();
            if (length != 8) throw FormatError(opcode_offset, "Invalid PICT lineJustify state");
            reader.take(length);
            break;
        }
        case 0x0035:
        case 0x0036:
        case 0x0037:
        case 0x0045:
        case 0x0046:
        case 0x0047:
        case 0x0055:
        case 0x0056:
        case 0x0057:
            reader.take(8);
            break;
        case 0x003d:
        case 0x003e:
        case 0x003f:
        case 0x004d:
        case 0x004e:
        case 0x004f:
        case 0x005d:
        case 0x005e:
        case 0x005f:
        case 0x007d:
        case 0x007e:
        case 0x007f:
        case 0x008d:
        case 0x008e:
        case 0x008f:
            break;
        case 0x0065:
        case 0x0066:
        case 0x0067:
            reader.take(12);
            break;
        case 0x006d:
        case 0x006e:
        case 0x006f:
            reader.take(4);
            break;
        case 0x0075:
        case 0x0076:
        case 0x0077:
            skip_polygon(reader);
            break;
        case 0x0085:
        case 0x0086:
        case 0x0087:
            (void)read_region(reader,context);
            break;
        case 0x0090:
        case 0x0091:
        case 0x0098:
        case 0x0099: {
            const auto map = read_indexed_pixmap_or_bitmap(reader,context);
            const auto source_rect = read_rect(reader);
            const auto destination_rect = read_rect(reader);
            const auto mode = reader.u16();
            std::optional<Region> mask;
            if (opcode == 0x0091 || opcode == 0x0099) mask = read_region(reader,context);
            const auto pixels = read_indexed_pixels(reader, map, opcode >= 0x0098,
                                                    foreground, background,context);
            blit(result, frame, map, pixels, source_rect, destination_rect, clip, mask,
                 mode, opcode_offset,context);
            drew_raster = true;
            break;
        }
        case 0x009a:
        case 0x009b: {
            const auto map = read_direct_pixmap(reader);
            const auto source_rect = read_rect(reader);
            const auto destination_rect = read_rect(reader);
            const auto mode = reader.u16();
            std::optional<Region> mask;
            if (opcode == 0x009b) mask = read_region(reader,context);
            const auto pixels = read_direct_pixels(reader, map,context);
            blit(result, frame, map, pixels, source_rect, destination_rect, clip, mask,
                 mode, opcode_offset,context);
            drew_raster = true;
            break;
        }
        case 0x00a0: reader.take(2); break;
        case 0x00a1: {
            reader.u16();
            const auto length = reader.u16();
            reader.take(length);
            break;
        }
        case 0x0024:
        case 0x0025:
        case 0x0026:
        case 0x0027:
        case 0x002f:
        case 0x0092:
        case 0x0093:
        case 0x0094:
        case 0x0095:
        case 0x0096:
        case 0x0097:
        case 0x009c:
        case 0x009d:
        case 0x009e:
        case 0x009f:
        case 0x00a2:
        case 0x00a3:
        case 0x00a4:
        case 0x00a5:
        case 0x00a6:
        case 0x00a7:
        case 0x00a8:
        case 0x00a9:
        case 0x00aa:
        case 0x00ab:
        case 0x00ac:
        case 0x00ad:
        case 0x00ae:
        case 0x00af:
            skip_word_sized_payload(reader);
            break;
        case 0x00ff:
            if (!drew_raster) {
                throw FormatError(opcode_offset,
                                  "PICT contains no supported raster drawing opcode");
            }
            return result;
        case 0x8200:
            decode_compressed_quicktime(reader, result, frame, clip, opcode_offset,context);
            drew_raster = true;
            quicktime_fallback_pending = true;
            break;
        case 0x8201:
            throw FormatError(opcode_offset,
                              "PICT UncompressedQuickTime opcode is not supported");
        default:
            if (opcode >= 0x00b0 && opcode <= 0x00cf) break;
            if (opcode >= 0x00d0 && opcode <= 0x00fe) {
                skip_long_sized_payload(reader);
                break;
            }
            if (opcode >= 0x0100 && opcode <= 0x7fff) {
                reader.take(2U * (opcode >> 8U));
                break;
            }
            if (opcode >= 0x8000 && opcode <= 0x80ff) break;
            if ((opcode >= 0x8100 && opcode <= 0x81ff) || opcode == 0xffff) {
                skip_long_sized_payload(reader);
                break;
            }
            throw FormatError(opcode_offset, "Unsupported PICT drawing opcode");
        }
    }
}

Bytes encode_pict(const Raster& image) {
    if ((!image.can_edit() && !(image.depth == 24 && image.color_order == 4)) ||
        image.width <= 0 || image.height <= 0 || image.width > 4095 ||
        image.height > 4095) {
        throw std::invalid_argument("Unsupported PICT output dimensions or layout");
    }
    if (!std::isfinite(image.resolution) || image.resolution < 1.0 ||
        image.resolution > 32767.0) {
        throw std::invalid_argument("PICT resolution is outside signed 16.16 range");
    }

    Writer writer;
    writer.data.resize(legacy_file_header_size, 0);
    const auto payload_offset = writer.data.size();
    writer.u16(0); // Patched after OpEndPic; zero denotes a payload above 64 KiB.
    const Rect frame{0, 0, image.height, image.width};
    write_rect(writer, frame);
    writer.u16(0x0011);
    writer.u16(0x02ff);
    writer.u16(0x0c00);
    writer.u16(0xfffe);
    writer.u16(0);
    const auto fixed_resolution = static_cast<std::uint32_t>(image.resolution) << 16U;
    writer.u32(fixed_resolution);
    writer.u32(fixed_resolution);
    write_rect(writer, frame);
    writer.u32(0);

    writer.u16(0x0001);
    writer.u16(10);
    write_rect(writer, frame);

    writer.u16(0x009a);
    writer.u32(0xff);
    const auto row_bytes = static_cast<std::size_t>(image.width) * 4U;
    writer.u16(static_cast<std::uint16_t>(0x8000U | row_bytes));
    write_rect(writer, frame);
    writer.u16(0); // pmVersion
    writer.u16(4); // component PackBits
    writer.u32(0); // packSize
    writer.u32(fixed_resolution);
    writer.u32(fixed_resolution);
    writer.u16(16); // RGBDirect
    writer.u16(32);
    writer.u16(4); // alpha, red, green, blue
    writer.u16(8);
    writer.u32(0); // planeBytes
    writer.u32(0); // pmTable
    writer.u32(0); // pmReserved
    write_rect(writer, frame);
    write_rect(writer, frame);
    writer.u16(64); // ditherCopy; equivalent to srcCopy for direct pixels

    Bytes components(row_bytes);
    for (int y = 0; y < image.height; ++y) {
        for (int x = 0; x < image.width; ++x) {
            const auto pixel = image.pixel(x, y);
            const auto position = static_cast<std::size_t>(x);
            components[position] = static_cast<std::uint8_t>(pixel.a >> 8U);
            components[static_cast<std::size_t>(image.width) + position] =
                static_cast<std::uint8_t>(pixel.r >> 8U);
            components[static_cast<std::size_t>(image.width) * 2U + position] =
                static_cast<std::uint8_t>(pixel.g >> 8U);
            components[static_cast<std::size_t>(image.width) * 3U + position] =
                static_cast<std::uint8_t>(pixel.b >> 8U);
        }
        if (row_bytes < 8U) {
            const auto pixel = image.pixel(0, y);
            writer.u8(static_cast<std::uint8_t>(pixel.a >> 8U));
            writer.u8(static_cast<std::uint8_t>(pixel.r >> 8U));
            writer.u8(static_cast<std::uint8_t>(pixel.g >> 8U));
            writer.u8(static_cast<std::uint8_t>(pixel.b >> 8U));
        } else {
            write_packed_row(writer, row_bytes, components);
        }
    }
    if ((writer.data.size() - payload_offset) & 1U) writer.u8(0);
    writer.u16(0x00ff);

    const auto payload_size = writer.data.size() - payload_offset;
    if (payload_size <= std::numeric_limits<std::uint16_t>::max()) {
        writer.data[payload_offset] = static_cast<std::uint8_t>(payload_size >> 8U);
        writer.data[payload_offset + 1U] = static_cast<std::uint8_t>(payload_size);
    }
    return std::move(writer.data);
}

} // namespace retas
