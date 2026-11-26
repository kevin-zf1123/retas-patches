#pragma once
#include "retas/document.hpp"

namespace retas {struct VectorPaintArchive;}
namespace retas::vector_tool_output_detail {
enum class PaintProfile { Normal, Fill, Brush };
// Modern Document consumer metadata support, independent of native tool gates.
// Call before source capture/geometry copies; this guard does not allocate.
// Ordinary child origin is ignored. Paint requires supported zero Length
// positions and zero child x/y, and owner/child opacity100/mode1.
void admit_metadata(const Layer&);
// Explicit Fill-only extension: opacity100 Paint/Select owner mode1/4,
//with exact22100001 child mode4. Other callers use the false wrapper.
void admit_metadata(const Layer&,bool allow_paint_mode4);
//Brush alone admits owner/child density0..100 and modes1..5. Its caller
//also certifies the whole native image when the owner requires that terminal.
void admit_metadata(const Layer&,PaintProfile);
// Actual persistent normal RGBA1x/AA2 consumer admission without rendering.
// Ordinary221/244 owners use dense stored Line costs and pass nullptr.
// Paint212/208 owners use actual mixed sparse Line/Face costs when native LC
// lists are nonempty, and the independent Face-only costs otherwise. They pass
// the matching private decoded candidate after layer.specific was encoded.
// Paint projection can move decoded texture leaves into owning recipes;
// do not encode that archive again after this call. Layer bytes are immutable.
// All hidden siblings and Face closure/recipes participate. Owning carriers
// remain live throughout the actual prepare call. storage/work are cumulative
// operation costs, including previous staging and projection reservations.
// The caller retains and publishes its candidate only after success. Failure
// throws before publication; native host gates remain the caller's concern.
void admit_output(const Layer&,VectorPaintArchive* private_encoded_candidate,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token stop={});
void admit_output(const Layer&,VectorPaintArchive* private_encoded_candidate,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token stop,bool allow_paint_mode4);
void admit_output(const Layer&,VectorPaintArchive* private_encoded_candidate,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token stop,PaintProfile);
}
