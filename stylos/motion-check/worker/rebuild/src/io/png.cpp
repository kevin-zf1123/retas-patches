#include "retas/png.hpp"
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace retas {
namespace {
constexpr std::uint64_t allocation_limit = 512ull * 1024 * 1024;
unsigned filter_type(PngFilter filter) {
    switch (filter) {
    case PngFilter::None: return 0;
    case PngFilter::Sub: return 1;
    case PngFilter::Up: return 2;
    case PngFilter::Average: return 3;
    case PngFilter::Paeth: return 4;
    }
    throw std::invalid_argument("Invalid PNG filter setting");
}
unsigned paeth(int a, int b, int c) {
    const int p = a + b - c;
    const int pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
    return unsigned(pa <= pb && pa <= pc ? a : (pb <= pc ? b : c));
}
Bytes compress_rows(const Bytes& rows) {
    z_stream stream{};
    // Native png_write_info initializes zlib before the selected row filter is
    // applied. All supported non-palette 8/16-bit outputs start with libpng's
    // all-filter mask and its implicit Z_FILTERED strategy.
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15, 8,
        Z_FILTERED) != Z_OK)
        throw std::runtime_error("PNG compressor initialization failed");
    struct EndStream {
        z_stream& stream;
        ~EndStream() { deflateEnd(&stream); }
    } end{stream};
    const auto bound = deflateBound(&stream, static_cast<uLong>(rows.size()));
    if (bound + 128ull > allocation_limit) throw std::length_error("PNG output exceeds size limit");
    Bytes compressed(bound);
    stream.next_in = const_cast<Bytef*>(rows.data());
    stream.avail_in = static_cast<uInt>(rows.size());
    stream.next_out = compressed.data();
    stream.avail_out = static_cast<uInt>(compressed.size());
    if (deflate(&stream, Z_FINISH) != Z_STREAM_END)
        throw std::runtime_error("PNG compression failed");
    compressed.resize(stream.total_out);
    return compressed;
}
void chunk(Writer& output, std::string_view type, std::span<const std::uint8_t> data) {
    output.u32(static_cast<std::uint32_t>(data.size()));
    const auto start = output.data.size();
    output.raw(std::span(reinterpret_cast<const std::uint8_t*>(type.data()), type.size()));
    output.raw(data);
    output.u32(static_cast<std::uint32_t>(crc32(0, output.data.data() + start,
        static_cast<uInt>(data.size() + type.size()))));
}
}
Bytes encode_png(const Raster& image, PngSettings settings, const PngWriteContext& context) {
    const auto filter = filter_type(settings.filter);
    if (image.width <= 0 || image.height <= 0 || image.width > 100000 || image.height > 100000 ||
        std::uint64_t(image.width) * image.height > 268435456)
        throw std::invalid_argument("Invalid PNG dimensions");
    if (!image.can_edit() && !(image.depth == 24 && image.color_order == 4) &&
        !(image.depth == 8 && image.color_order == 0))
        throw std::invalid_argument("Unsupported PNG pixel layout");
    const bool wide = image.depth == 64 && settings.write_16bit;
    const unsigned channels = image.depth == 8 ? 1 : (image.depth == 24 ? 3 : 4);
    const unsigned pixel_bytes = channels * (wide ? 2 : 1);
    const auto stride = std::size_t(image.width) * pixel_bytes;
    const auto raw_size = std::uint64_t(stride + 1) * image.height;
    if (raw_size > allocation_limit) throw std::length_error("PNG row allocation exceeds size limit");
    const auto bound = compressBound(static_cast<uLong>(raw_size));
    if (bound + 128ull > allocation_limit) throw std::length_error("PNG output exceeds size limit");
    // Preserve the native binary64 operation order before truncation.
    const double physical_resolution = (image.resolution * 100.0) / 2.54;
    if (!std::isfinite(image.resolution) || image.resolution <= 0 ||
        !std::isfinite(physical_resolution) || physical_resolution > UINT32_MAX)
        throw std::invalid_argument("Invalid PNG resolution");
    const auto ppm = static_cast<std::uint32_t>(physical_resolution);
    std::uint64_t backing_size = 0;
    for (const auto& [position, tile] : image.tiles) {
        if (position.x < 0 || position.y < 0 || position.x >= (image.width + 255) / 256 ||
            position.y >= (image.height + 255) / 256 || !tile || tile->size() != image.tile_bytes())
            throw std::invalid_argument("Invalid PNG source tile");
        backing_size += tile->size();
        if (backing_size > allocation_limit) throw std::length_error("PNG source exceeds allocation limit");
    }

    Bytes rows(static_cast<std::size_t>(raw_size));
    const bool report_progress = image.width > 4000 || image.height > 4000;
    for (int y = 0; y < image.height; ++y) {
        if (report_progress && y != 0 && y % 100 == 0) {
            if (context.progress) context.progress(std::size_t(y), std::size_t(image.height));
            if (context.cancelled && context.cancelled()) throw PngWriteCancelled();
        }
        auto destination = rows.data() + std::size_t(y) * (stride + 1);
        *destination++ = std::uint8_t(filter);
        for (int x = 0; x < image.width; ++x) {
            const auto p = image.pixel(x, y);
            const std::uint16_t values[]{p.r, p.g, p.b, p.a};
            if (channels == 1) *destination++ = std::uint8_t(p.a >> 8);
            else for (unsigned channel = 0; channel < channels; ++channel) {
                *destination++ = std::uint8_t(image.depth == 64 && !wide ? values[channel] / 257 : values[channel] >> 8);
                if (wide) *destination++ = std::uint8_t(values[channel]);
            }
        }
    }
    // All host row access and cancellation finish before filtering/compression.
    if (filter != 0) {
        Bytes previous(stride, 0), current(stride);
        for (int y = 0; y < image.height; ++y) {
            auto row = rows.data() + std::size_t(y) * (stride + 1) + 1;
            std::copy_n(row, stride, current.begin());
            for (std::size_t x = 0; x < stride; ++x) {
                const unsigned left = x < pixel_bytes ? 0 : current[x - pixel_bytes];
                const unsigned above = previous[x];
                const unsigned upper_left = x < pixel_bytes ? 0 : previous[x - pixel_bytes];
                unsigned predictor = 0;
                switch (filter) {
                case 1: predictor = left; break;
                case 2: predictor = above; break;
                case 3: predictor = (left + above) / 2; break;
                case 4: predictor = paeth(left, above, upper_left); break;
                }
                row[x] = std::uint8_t(current[x] - predictor);
            }
            previous.swap(current);
        }
    }
    auto compressed = compress_rows(rows);
    Bytes{}.swap(rows);
    Writer output;
    output.data.reserve(compressed.size() + 128);
    output.raw(Bytes{137, 80, 78, 71, 13, 10, 26, 10});
    Writer header;
    header.u32(image.width); header.u32(image.height);
    header.u8(wide ? 16 : 8); header.u8(channels == 1 ? 0 : (channels == 3 ? 2 : 6));
    header.u8(0); header.u8(0); header.u8(0);
    chunk(output, "IHDR", header.data);
    chunk(output, "tEXt", Bytes{'S','o','f','t','w','a','r','e',0,'R','E','T','A','S'});
    Writer physical;
    physical.u32(ppm); physical.u32(ppm); physical.u8(1);
    chunk(output, "pHYs", physical.data);
    chunk(output, "IDAT", compressed);
    chunk(output, "IEND", {});
    return std::move(output.data);
}
}
