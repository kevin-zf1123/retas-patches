#pragma once
#include "retas/vector_face_archive.hpp"
namespace retas::vector_archive_detail {
std::size_t line_wire_size(const VectorLineArchive&);
std::size_t paint_wire_size(const VectorPaintArchive&);
}
