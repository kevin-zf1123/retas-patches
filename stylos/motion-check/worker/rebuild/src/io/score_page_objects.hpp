#pragma once
#include "retas/score_objects.hpp"
namespace retas::score_page_detail {
// Page-only wire view: retains Text NUL suffixes for RCArchive::ReadString
// projection. The public editable object codec keeps its stricter contract.
ScoreObjects decode_page_objects(std::span<const std::uint8_t>);
}
