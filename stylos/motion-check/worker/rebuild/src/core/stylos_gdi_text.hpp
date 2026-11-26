#pragma once
#ifdef _WIN32
#include "text_font_backend_internal.hpp"
#include "retas/text_tool.hpp"

namespace retas::text_font_detail {
ExportMasks stylos_gdi_text(const TextParameters&,const Layer&,const Plane&,double,
                            resolution_detail::Budget&);
}
#endif
