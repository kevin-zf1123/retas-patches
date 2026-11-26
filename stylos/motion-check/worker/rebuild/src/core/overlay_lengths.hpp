#pragma once
#include "retas/document.hpp"

// Shared native RCLength arithmetic used by secondary Grid/Guide objects.
namespace retas::overlay_detail {
double millimeters(const Length&);
double inches(const Length&);
int pixels(const Length&,double dpi);
int integer(std::int64_t);
struct AdjustedGrid {Length spacing;int subdivisions;};
AdjustedGrid adjusted_grid(const GridState&,double zoom);
}
