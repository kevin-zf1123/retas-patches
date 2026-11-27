#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "stylos_gdi_text.hpp"
#include "retas/document.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <windows.h>

namespace retas::text_font_detail {
namespace {
struct Context {
  HDC dc = CreateCompatibleDC(nullptr);
  HGDIOBJ oldfont{}, oldbitmap{};
  HFONT font{};
  HBITMAP bitmap{};
  Context() {
    if (!dc)
      throw std::runtime_error("Cannot create a text measurement DC");
  }
  ~Context() {
    if (oldfont)
      SelectObject(dc, oldfont);
    if (oldbitmap)
      SelectObject(dc, oldbitmap);
    if (font)
      DeleteObject(font);
    if (bitmap)
      DeleteObject(bitmap);
    DeleteDC(dc);
  }
};
struct FontMatch {
  std::string requested;
  const char *fallback;
  LOGFONTA first{}, substitute{}, exact{};
};
int CALLBACK match_font(const LOGFONTA *candidate, const TEXTMETRICA *, DWORD,
                        LPARAM info) {
  auto &match = *reinterpret_cast<FontMatch *>(info);
  if (candidate->lfFaceName[0] == '@')
    return 1;
  if (!match.first.lfFaceName[0] ||
      std::strcmp(candidate->lfFaceName, match.first.lfFaceName) < 0)
    match.first = *candidate;
  if (!match.substitute.lfFaceName[0] &&
      !std::strcmp(candidate->lfFaceName, match.fallback))
    match.substitute = *candidate;
  if (!std::strncmp(candidate->lfFaceName, match.requested.c_str(),
                    match.requested.size()) &&
      (!match.exact.lfFaceName[0] ||
       std::strcmp(candidate->lfFaceName, match.exact.lfFaceName) < 0))
    match.exact = *candidate;
  return 1;
}
struct MonoInfo {
  BITMAPINFOHEADER header{};
  RGBQUAD palette[2]{};
};
template <class Paint>
Mask bitmap_mask(Context &c, int x, int y, int width, int height, double dpi,
                 resolution_detail::Budget &budget, Paint paint) {
  width = std::max(1, width);
  height = std::max(1, height);
  if (width > 20000 || height > 20000 ||
      std::uint64_t(width) * height > 64ULL * 1024 * 1024)
    throw std::length_error("Text bitmap is too large");
  const size_t stride = ((size_t(width) + 31) / 32) * 4;
  budget.allocate(stride * height + std::uint64_t((width + 255) / 256) *
                                        ((height + 255) / 256) * 8192);
  MonoInfo info;
  info.header.biSize = sizeof(info.header);
  info.header.biWidth = width;
  info.header.biHeight = -height;
  info.header.biPlanes = 1;
  info.header.biBitCount = 1;
  info.palette[1] = {255, 255, 255, 0};
  void *bits = nullptr;
  auto bitmap = CreateDIBSection(c.dc, reinterpret_cast<BITMAPINFO *>(&info),
                                 DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!bitmap)
    throw std::runtime_error("Cannot create a Stylos text mask");
  if (c.oldbitmap) {
    SelectObject(c.dc, c.oldbitmap);
    DeleteObject(c.bitmap);
  }
  c.bitmap = bitmap;
  c.oldbitmap = SelectObject(c.dc, bitmap);
  std::memset(bits, 255, stride * height);
  SetBkColor(c.dc, RGB(255, 255, 255));
  SetTextColor(c.dc, RGB(0, 0, 0));
  SetBkMode(c.dc, TRANSPARENT);
  paint(c.dc);
  GdiFlush();
  Mask result{Raster(width, height, 1, dpi), {x, y}};
  const auto *data = static_cast<const BYTE *>(bits);
  for (int row = 0; row < height; ++row) {
    budget.poll();
    for (int col = 0; col < width; ++col)
      if (!(data[stride * row + col / 8] & (0x80 >> (col % 8)))) {
        auto &tile = result.raster.tiles[{col / 256, row / 256}];
        if (!tile)
          tile = std::make_shared<Bytes>(8192, 0);
        (*tile)[(row % 256) * 32 + (col % 256) / 8] |=
            std::uint8_t(0x80 >> (col % 8));
      }
  }
  return result;
}
} // namespace
ExportMasks stylos_gdi_text(const TextParameters &p, const Layer &owner,
                            const Plane &plane, double dpi,
                            resolution_detail::Budget &budget) {
  if (!std::isfinite(dpi) || dpi <= 0 ||
      !std::isfinite(plane.common.resolution) || plane.common.resolution <= 0)
    throw std::invalid_argument("Invalid Stylos text resolution");
  const auto height =
      legacy_rt_double_to_int(double(int(dpi)) * p.font_size / 72.0);
  if (!height || *height < 1 || *height > 8192)
    throw std::invalid_argument("Invalid Stylos text height");
  Context c;
  FontMatch match{std::string(p.font_name.begin(), p.font_name.end()),
                  PRIMARYLANGID(GetSystemDefaultLangID()) == LANG_JAPANESE
                      ? "MS UI Gothic"
                      : "MS Sans Serif"};
  match.requested.resize(std::strlen(match.requested.c_str()));
  EnumFontFamiliesA(c.dc, nullptr, match_font,
                    reinterpret_cast<LPARAM>(&match));
  LOGFONTA font = match.exact.lfFaceName[0]        ? match.exact
                  : match.substitute.lfFaceName[0] ? match.substitute
                                                   : match.first;
  font.lfHeight = -*height;
  font.lfWidth = 0;
  font.lfQuality = PROOF_QUALITY;
  c.font = CreateFontIndirectA(&font);
  if (!c.font)
    throw std::runtime_error("Cannot create Stylos font");
  c.oldfont = SelectObject(c.dc, c.font);
  std::string text(p.text.begin(), p.text.end());
  text.resize(std::strlen(text.c_str()));
  struct Line {
    std::string value;
    int height;
  };
  std::vector<Line> lines;
  int width = 0, total = 0;
  for (size_t start = 0; start <= text.size();) {
    size_t end = text.find('\n', start);
    auto line = text.substr(start, end == text.npos ? end : end - start);
    if (!line.empty() && line.back() == '\r')
      line.pop_back();
    SIZE extent{};
    if (!GetTextExtentPoint32A(c.dc, line.empty() ? " " : line.data(),
                               line.empty() ? 1 : int(line.size()), &extent))
      throw std::runtime_error("Cannot measure Stylos text");
    if (!line.empty())
      width = std::max(width, int(extent.cx));
    if (extent.cy < 0 || extent.cy > 20000 - total)
      throw std::length_error("Stylos text height exceeds limit");
    total += extent.cy;
    lines.push_back({std::move(line), int(extent.cy)});
    if (end == text.npos)
      break;
    start = end + 1;
  }
  const double ratio = dpi / plane.common.resolution;
  auto project = [&](int offset, int point) {
    const auto v = (double(offset) + point) * ratio;
    if (!std::isfinite(v) || std::abs(v) > 1000000)
      throw std::length_error("Stylos text anchor exceeds limit");
    return int(v);
  };
  int ax = project(owner.common.x, p.text_anchor[0]),
      ay = project(owner.common.y, p.text_anchor[1]);
  int lx = project(owner.common.x, p.line_anchor[0]),
      ly = project(owner.common.y, p.line_anchor[1]);
  width = std::max(width, 1);
  total = std::max(total, 1);
  const int left = p.line_enabled && ax < lx ? ax - width : ax,
            join = ay + total - 1;
  ExportMasks result;
  result.text_color = p.text_color;
  result.line_color = p.line_color;
  result.glyph =
      bitmap_mask(c, left, ay, width, total, dpi, budget, [&](HDC dc) {
        int y = 0;
        for (const auto &line : lines) {
          if (!line.value.empty())
            TextOutA(dc, 0, y, line.value.data(), int(line.value.size()));
          y += line.height;
        }
      });
  if (p.line_enabled) {
    const auto x = std::min(left, lx), y = std::min(ay, ly),
               right = std::max(left + width, lx),
               bottom = std::max(ay + total, ly);
    result.leader =
        bitmap_mask(c, x, y, right - x, bottom - y, dpi, budget, [&](HDC dc) {
          auto pen = CreatePen(PS_SOLID, std::max(1, int(ratio)), RGB(0, 0, 0));
          if (!pen)
            throw std::runtime_error("Cannot create leader pen");
          auto old = SelectObject(dc, pen);
          MoveToEx(dc, lx - x, ly - y, nullptr);
          LineTo(dc, ax - x, join - y);
          MoveToEx(dc, left - x, join - y, nullptr);
          LineTo(dc, left + width - x, join - y);
          SelectObject(dc, old);
          DeleteObject(pen);
        });
  }
  return result;
}
} // namespace retas::text_font_detail
#endif
