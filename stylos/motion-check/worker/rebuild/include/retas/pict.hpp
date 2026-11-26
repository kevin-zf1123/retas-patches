#pragma once

#include "raster.hpp"
#include <functional>

namespace retas {

// Decodes the portable raster subset of classic PICT files: version-2
// BitsRect/PackBitsRect and DirectBitsRect records, including indexed, 16-bit,
// 24-bit and alpha-bearing 32-bit pixels.  It also accepts the identity-mapped
// QuickTime Animation (`rle `) record emitted by the original PaintMan plug-in.
// The optional 512-byte file header is accepted.  PICT v1, transformed/matted
// or partial QuickTime images, other QuickTime codecs, and vector or
// coordinate-changing drawing opcodes fail explicitly; they are never silently
// omitted.
Raster decode_pict(std::span<const std::uint8_t> bytes);
struct PictReadContext {
    // Invoked before expansion/allocation and at scan boundaries. The caller
    // may throw to enforce a cumulative budget or request cancellation. The
    // decoder freezes an owning callback copy before its first invocation.
    std::function<void(std::uint64_t work,std::size_t allocation)> admit;
};
Raster decode_pict(std::span<const std::uint8_t> bytes,const PictReadContext&);

// Writes a deterministic extended-version-2 PICT with the historical 512-byte
// zero header and a component-PackBits DirectBitsRect record.  Alpha is stored
// as QuickDraw's fourth (high-byte) component.  The original plug-in's 4095 by
// 4095 export limit is enforced.
Bytes encode_pict(const Raster& image);

}
