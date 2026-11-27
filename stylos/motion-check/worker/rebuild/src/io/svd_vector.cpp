#include "svd_document.hpp"
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace retas::svd_detail {
namespace {
Bytes inflate_block(Reader &reader, std::uint64_t remaining) {
    const auto length = reader.u32();
    if (!length || length > std::min<std::uint64_t>(remaining, 128u * 1024 * 1024))
        throw FormatError(reader.offset(), "Invalid WCEL vector block size");
    auto compressed = reader.section();
    auto input = compressed.take(compressed.remaining());
    Bytes bytes(length);
    uLongf produced = length;
    uLong used = input.size();
    if (uncompress2(bytes.data(), &produced, input.data(), &used) != Z_OK || produced != length ||
        used != input.size())
        throw FormatError(reader.offset(), "Invalid compressed WCEL vector block");
    return bytes;
}
Pixel argb(Reader &reader) {
    const auto alpha = reader.u16();
    return {reader.u16(), reader.u16(), reader.u16(), alpha};
}
Edge read_edge(Reader &records, const Edge *forward, bool compressed, int units) {
    auto record = records.section();
    Edge edge;
    edge.flags = record.u32();
    if (compressed && (edge.flags & 1u)) {
        if (!forward)
            throw FormatError(record.offset(), "WCEL twin has no forward edge");
        edge.ink = forward->ink;
        edge.start = forward->end;
        edge.end = forward->start;
        edge.start_scale = forward->end_scale;
        edge.end_scale = forward->start_scale;
        edge.radius = forward->radius;
    } else {
        edge.ink = argb(record);
        edge.start.y = std::trunc(double(record.i32()) * 4800 / units);
        edge.end.y = std::trunc(double(record.i32()) * 4800 / units);
        edge.start.x = record.f64() * 4800 / units;
        edge.end.x = record.f64() * 4800 / units;
        edge.start_scale = record.f32();
        edge.end_scale = record.f32();
        record.take(8);
        const auto radius = record.f32() * 4800.f / float(units);
        if (!(std::abs(edge.start.x) <= 16000000 && std::abs(edge.end.x) <= 16000000 &&
              std::abs(edge.start.y) <= 16000000 && std::abs(edge.end.y) <= 16000000) ||
            !std::isfinite(edge.start_scale) || !std::isfinite(edge.end_scale) || !std::isfinite(radius))
            throw FormatError(record.offset(), "Nonfinite WCEL stroke geometry");
        edge.radius = units == 4800 ? radius : std::max(2.f, radius);
    }
    edge.id = record.u32();
    edge.face_order = record.u32();
    edge.face = argb(record);
    edge.effect = record.i32();
    return edge;
}
} // namespace

VectorDrawing read_drawing(std::span<const std::uint8_t> bytes, std::uint64_t workspace) {
    Reader database(bytes);
    const auto major = database.u16(), minor = database.u16();
    if (major != 1 && major != 2)
        throw FormatError(0, "Unsupported WCEL vector database version");
    const bool compressed = major != 1 || minor != 0;
    auto metadata = database.section();
    const auto count = metadata.u32();
    VectorDrawing drawing;
    if (major == 2 || minor >= 2)
        drawing.effects = metadata.u32();
    const auto units = major == 2 ? metadata.i32() : 9600;
    if (units <= 0 || count > 200000 || (count & 1) || drawing.effects > 100000)
        throw FormatError(metadata.offset(), "Invalid WCEL vector database metadata");
    // Each directed edge can contribute to a face, a hole and a stroke.
    // Include the maximum 16-step spline expansion, scan edges, duplicate
    // contour storage and node containers before any collection reserves.
    constexpr std::uint64_t bytes_per_edge = 6144;
    const auto shape_storage = count * bytes_per_edge;
    if (shape_storage > workspace)
        throw std::length_error("WCEL vector workspace exceeds memory limit");
    workspace -= shape_storage;
    if (count) {
        Bytes decoded;
        if (compressed)
            decoded = inflate_block(database, workspace);
        Reader records = compressed ? Reader(decoded) : database;
        records.require_count(int(count), 28, 200000);
        drawing.edges.reserve(count);
        for (unsigned i = 0; i < count; ++i) {
            const Edge *forward = i % 2 ? &drawing.edges.back() : nullptr;
            drawing.edges.push_back(read_edge(records, forward, compressed, units));
        }
        if (!compressed)
            database = records;
    }
    if (drawing.effects) {
        auto bytes = inflate_block(database, workspace);
        Reader effects(bytes);
        effects.require_count(int(drawing.effects), 12);
        for (unsigned i = 0; i < drawing.effects; ++i) {
            effects.i32();
            (void)effects.section();
            (void)effects.section();
        }
    }
    return drawing;
}
} // namespace retas::svd_detail
