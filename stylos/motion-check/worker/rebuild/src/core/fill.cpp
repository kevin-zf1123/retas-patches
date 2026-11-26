#include "retas/fill.hpp"
#include "retas/numeric.hpp"
#include "retas/selection_wand.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "selection_raster.hpp"
#include "native_offscreen.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace retas {
namespace {

struct Cancelled final {};

Pixel fill_pixel(const Raster& surface,int x,int y){
    if(surface.can_edit()&&(!surface.contains(x,y)||!surface.tiles.contains({x/256,y/256})))return {};
    return raster_stroke_surface_pixel(surface,x,y);
}

struct Bounds {
    int left{};
    int top{};
    int right{};
    int bottom{};

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool empty() const { return left >= right || top >= bottom; }
    bool contains(int x, int y) const {
        return x >= left && y >= top && x < right && y < bottom;
    }
};

std::pair<std::size_t, std::size_t> checked_mask_layout(int width, int height) {
    constexpr int max_dimension = 100000;
    constexpr std::uint64_t max_pixels = 268435456ull;
    constexpr std::size_t max_words = std::size_t(max_pixels / 32);
    if (width <= 0 || height <= 0 || width > max_dimension ||
        height > max_dimension ||
        std::uint64_t(width) * std::uint64_t(height) > max_pixels) {
        throw std::invalid_argument("Fill mask exceeds supported limits");
    }
    const auto stride = (std::size_t(width) + 31) / 32;
    if (stride == 0 || std::size_t(height) > max_words / stride)
        throw std::invalid_argument("Fill mask exceeds supported limits");
    return {stride, stride * std::size_t(height)};
}

class BitMask {
    int width_{};
    int height_{};
    std::size_t stride_{};
    std::vector<std::uint32_t> words_;

public:
    BitMask() = default;
    BitMask(int width, int height) {
        const auto [stride, word_count] = checked_mask_layout(width, height);
        width_ = width;
        height_ = height;
        stride_ = stride;
        words_.assign(word_count, 0);
    }

    int width() const { return width_; }
    int height() const { return height_; }
    bool contains(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }
    bool test(int x, int y) const {
        if (!contains(x, y)) return false;
        const auto word = std::size_t(y) * stride_ + std::size_t(x >> 5);
        return (words_[word] & (std::uint32_t(1) << (x & 31))) != 0;
    }
    void set(int x, int y, bool value = true) {
        if (!contains(x, y)) return;
        const auto word = std::size_t(y) * stride_ + std::size_t(x >> 5);
        const auto bit = std::uint32_t(1) << (x & 31);
        if (value) words_[word] |= bit;
        else words_[word] &= ~bit;
    }
    void clear() { std::fill(words_.begin(), words_.end(), 0); }
};

struct DirtyBounds {
    int left{std::numeric_limits<int>::max()};
    int top{std::numeric_limits<int>::max()};
    int right{std::numeric_limits<int>::min()};
    int bottom{std::numeric_limits<int>::min()};

    void add(int x, int y) {
        left = std::min(left, x);
        top = std::min(top, y);
        right = std::max(right, x + 1);
        bottom = std::max(bottom, y + 1);
    }
    bool empty() const { return left >= right || top >= bottom; }
    Rect rect() const {
        return empty() ? Rect{} : Rect{left, top, right - left, bottom - top};
    }
};

struct LineView {
    const Raster* raster{};
    std::array<double, 6> target_to_line{1.0, 0.0, 0.0,
                                         0.0, 1.0, 0.0};
    bool white_is_empty{};
    bool hard_barrier{};
    std::optional<std::uint32_t> match_seed32{};
};

Bounds clipped_bounds(const Raster& raster, const std::optional<RasterSelection>& selection) {
    if (!selection) return {0, 0, raster.width, raster.height};
    const auto& bounds=selection->bounds();
    if (bounds.width <= 0 || bounds.height <= 0) return {};
    const auto right64 = std::int64_t(bounds.x) + bounds.width;
    const auto bottom64 = std::int64_t(bounds.y) + bounds.height;
    return {
        std::max(0, bounds.x),
        std::max(0, bounds.y),
        static_cast<int>(std::min<std::int64_t>(raster.width, right64)),
        static_cast<int>(std::min<std::int64_t>(raster.height, bottom64)),
    };
}

bool white_rgb(Pixel pixel) {
    return pixel.r == 65535 && pixel.g == 65535 && pixel.b == 65535;
}

bool white_transparent_family32(Pixel pixel, int depth, bool white_is_empty) {
    return white_is_empty && depth == 32 &&
           (pixel.a == 0 || white_rgb(pixel));
}

bool line_sample_empty(Pixel pixel, int depth, bool white_is_empty) {
    (void)depth;
    return pixel.a == 0 || (white_is_empty && white_rgb(pixel));
}

bool equal_with_margin(Pixel first, Pixel second, int margin,
                       int depth, bool white_is_empty) {
    if(depth==1)return first.a==second.a;
    if(depth==8)return std::abs(int(first.a>>8)-int(second.a>>8))<=margin;
    if(depth==2||depth==16||depth==24)return false;
    if (white_transparent_family32(first, depth, white_is_empty) &&
        white_transparent_family32(second, depth, white_is_empty)) return true;
    const auto distance = [margin](std::uint16_t a, std::uint16_t b) {
        return std::abs(int(a) - int(b)) <= margin * 257;
    };
    return distance(first.r, second.r) && distance(first.g, second.g) &&
           distance(first.b, second.b) && distance(first.a, second.a);
}

bool list_equal(Pixel first, Pixel second, int depth, bool white_is_empty) {
    if (white_transparent_family32(first, depth, white_is_empty) &&
        white_transparent_family32(second, depth, white_is_empty)) return true;
    if (depth == 32) {
        return (first.r >> 8) == (second.r >> 8) &&
               (first.g >> 8) == (second.g >> 8) &&
               (first.b >> 8) == (second.b >> 8) &&
               (first.a >> 8) == (second.a >> 8);
    }
    return first == second;
}

bool exact_opaque_black(Pixel pixel, int depth) {
    if (depth == 32) {
        return (pixel.r >> 8) == 0 && (pixel.g >> 8) == 0 &&
               (pixel.b >> 8) == 0 && (pixel.a >> 8) == 255;
    }
    return pixel == Pixel{0, 0, 0, 65535};
}

Pixel canonical_write(Pixel pixel, int depth) {
    if(depth==1)return {0,0,0,std::uint16_t((pixel.a>>8)&&!((pixel.r>>8)==255&&(pixel.g>>8)==255&&(pixel.b>>8)==255)?65535:0)};
    if(depth==8)return {0,0,0,std::uint16_t((255-(pixel.r>>8))*257)};
    if (depth != 32) return pixel;
    const auto channel = [](std::uint16_t value) {
        return static_cast<std::uint16_t>((value / 257u) * 257u);
    };
    return {channel(pixel.r), channel(pixel.g), channel(pixel.b),
            channel(pixel.a)};
}

void store_bucket_color(Raster& output,int x,int y,Pixel value){
    if(output.can_edit())output.set_pixel(x,y,value);
    else if(output.depth==1||output.depth==8)
        selection_detail::store_raw_pixel(output,x,y,output.depth==1?bool(value.a):value.a>>8);
}

std::vector<Pixel> selected_colors(const IncludeFillSettings& settings) {
    std::vector<Pixel> result;
    result.reserve(6);
    for (std::size_t i = 0; i < settings.enabled.size(); ++i) {
        if (!settings.enabled[i]) continue;
        result.push_back(settings.color(i));
    }
    return result;
}

struct LineEvidence {
    bool present{};
    bool black{};
    bool selected{};
    bool hard_blocked{};
};

class PixelClassifier {
    const Raster& target_;
    std::span<const LineView> lines_;
    Bounds bounds_;
    const BucketFillSettings& settings_;
    Pixel seed_color_;
    std::vector<Pixel> selected_;
    bool white_is_empty_{};
    bool scalar_mode_{};
    bool black_guard_{};
    const RasterSelection* selection_{};
    std::stop_token stop_;
    std::size_t samples_{};
    BitMask known_;
    BitMask work_;
    BitMask support_;

    bool is_selected(Pixel pixel, int depth, bool white_is_empty) const {
        return std::ranges::any_of(selected_, [&](Pixel candidate) {
            return list_equal(pixel, candidate, depth, white_is_empty);
        });
    }

    LineEvidence line_evidence(int x, int y) {
        LineEvidence result;
        for (const auto& view : lines_) {
            const double line_x = view.target_to_line[0] * x +
                                  view.target_to_line[1] * y + view.target_to_line[2];
            const double line_y = view.target_to_line[3] * x +
                                  view.target_to_line[4] * y + view.target_to_line[5];
            const bool inside=std::isfinite(line_x)&&std::isfinite(line_y)&&line_x>=0&&line_y>=0&&line_x<view.raster->width&&line_y<view.raster->height;
            if(!inside&&!view.match_seed32)continue;
            auto pixel=inside?fill_pixel(*view.raster,static_cast<int>(std::floor(line_x)),static_cast<int>(std::floor(line_y))):Pixel{};
            if(view.match_seed32){
                // Native LightLine uses Get32BitColorFast equality with its
                // seed sample, including latent RGB at alpha0. Ordinary
                // one-bit Get32 maps clear bits to ccrTransmit32Bit=0.
                if(view.raster->depth==1&&pixel.a==0)pixel=Pixel{0,0,0,0};
                const auto raw=(std::uint32_t(pixel.r>>8))|(std::uint32_t(pixel.g>>8)<<8)|(std::uint32_t(pixel.b>>8)<<16)|(std::uint32_t(pixel.a>>8)<<24);
                if(raw!=*view.match_seed32)result.hard_blocked=true;
                continue;
            }
            if (line_sample_empty(pixel, view.raster->depth,
                                  view.white_is_empty)) continue;
            result.present = true;
            result.hard_blocked=result.hard_blocked||view.hard_barrier;
            result.black = result.black || exact_opaque_black(pixel, view.raster->depth);
            result.selected = result.selected ||
                              is_selected(pixel, view.raster->depth, view.white_is_empty);
        }
        return result;
    }

    void classify(int local_x, int local_y) {
        if (known_.test(local_x, local_y)) return;
        if (((++samples_) & 4095u) == 0 && stop_.stop_requested()) throw Cancelled{};
        const int x = bounds_.left + local_x;
        const int y = bounds_.top + local_y;
        if(selection_&&!selection_->contains(x,y)){known_.set(local_x,local_y);return;}
        const Pixel target = fill_pixel(target_,x,y);
        const auto line = line_evidence(x, y);
        if(line.hard_blocked){known_.set(local_x,local_y);return;}
        const bool target_selected = is_selected(target, target_.depth, white_is_empty_);
        const bool target_matches = equal_with_margin(
            target, seed_color_, settings_.color_margin,
            target_.depth, white_is_empty_);
        const bool target_black = black_guard_ && !scalar_mode_ &&
                                  target_.depth == 32 &&
                                  exact_opaque_black(target, target_.depth);
        bool work = false;
        bool support = false;
        if(target_.depth<32){
            work=target_matches&&!line.present;
        }else if (scalar_mode_) {
            work = std::abs(int(target.a) - int(seed_color_.a)) <=
                       settings_.color_margin * 257 && !line.present &&
                   !target_black;
        } else switch (settings_.include.mode) {
        case IncludeFillMode::None:
            work = target_matches && !line.present && !target_black;
            break;
        case IncludeFillMode::SpecifiedColors:
            work = target_matches && !line.present && !target_black;
            support = !target_black && !line.black &&
                      (line.present ? line.selected : target_selected);
            break;
        case IncludeFillMode::ExceptSpecifiedColors:
            // On the normal raster bucket route mode 2 is a full-colour list
            // miss predicate, independent of the sampled seed/tolerance.
            work = !target_black && !line.black &&
                   !(line.present ? line.selected : target_selected);
            break;
        }
        known_.set(local_x, local_y);
        work_.set(local_x, local_y, work);
        support_.set(local_x, local_y, support);
    }

public:
    PixelClassifier(const Raster& target,
                    std::span<const LineView> lines,
                    Bounds bounds,
                    const BucketFillSettings& settings,
                    Pixel seed_color,
                    bool white_is_empty,
                    bool scalar_mode,
                    bool black_guard,
                    const RasterSelection* selection,
                    std::stop_token stop)
        : target_(target), lines_(lines), bounds_(bounds), settings_(settings),
          seed_color_(seed_color), selected_(selected_colors(settings.include)),
          white_is_empty_(white_is_empty), scalar_mode_(scalar_mode),
          black_guard_(black_guard), selection_(selection), stop_(stop),
          known_(bounds.width(), bounds.height()),
          work_(bounds.width(), bounds.height()),
          support_(bounds.width(), bounds.height()) {}

    bool work(int local_x, int local_y) {
        if (!known_.contains(local_x, local_y)) return false;
        classify(local_x, local_y);
        return work_.test(local_x, local_y);
    }
    bool support(int local_x, int local_y) {
        if (!known_.contains(local_x, local_y)) return false;
        classify(local_x, local_y);
        return support_.test(local_x, local_y);
    }
};

struct MaskFloodResult {
    bool ok{};
    bool leaked{};
    std::size_t count{};
    Bounds dirty{};
};

Bounds intersect_bounds(Bounds first, Bounds second) {
    Bounds result{std::max(first.left, second.left),
                  std::max(first.top, second.top),
                  std::min(first.right, second.right),
                  std::min(first.bottom, second.bottom)};
    return result.empty() ? Bounds{} : result;
}

Bounds expand_bounds(Bounds value, int amount) {
    const auto saturated = [](std::int64_t coordinate) {
        return static_cast<int>(std::clamp<std::int64_t>(
            coordinate, std::numeric_limits<int>::min(),
            std::numeric_limits<int>::max()));
    };
    return {saturated(std::int64_t(value.left) - amount),
            saturated(std::int64_t(value.top) - amount),
            saturated(std::int64_t(value.right) + amount),
            saturated(std::int64_t(value.bottom) + amount)};
}

bool contains_bounds(Bounds outer, Bounds inner) {
    return inner.left >= outer.left && inner.top >= outer.top &&
           inner.right <= outer.right && inner.bottom <= outer.bottom;
}

Bounds union_bounds(Bounds first, Bounds second) {
    if (first.empty()) return second;
    if (second.empty()) return first;
    return {std::min(first.left, second.left),
            std::min(first.top, second.top),
            std::max(first.right, second.right),
            std::max(first.bottom, second.bottom)};
}

bool has_set_four_neighbor(const BitMask& mask, int x, int y) {
    return mask.test(x - 1, y) || mask.test(x + 1, y) ||
           mask.test(x, y - 1) || mask.test(x, y + 1);
}

bool has_clear_in_bounds_four_neighbor(const BitMask& mask, int x, int y) {
    return (x > 0 && !mask.test(x - 1, y)) ||
           (x + 1 < mask.width() && !mask.test(x + 1, y)) ||
           (y > 0 && !mask.test(x, y - 1)) ||
           (y + 1 < mask.height() && !mask.test(x, y + 1));
}

template<class Condition>
MaskFloodResult mask_flood(BitMask& destination,
                           int seed_x,
                           int seed_y,
                           Condition condition,
                           bool stop_at_horizontal_edge,
                           std::stop_token stop) {
    MaskFloodResult output;
    if (!destination.contains(seed_x, seed_y) || destination.test(seed_x, seed_y))
        return output;

    const bool seed_class = condition(seed_x, seed_y);
    struct SeedScanLine { int base_y; int left; int right; int dy; };
    std::vector<SeedScanLine> stack{
        {seed_y, seed_x, seed_x, +1},
        {seed_y + 1, seed_x, seed_x, -1},
    };
    int dirty_left = seed_x;
    int dirty_top = seed_y;
    int dirty_right = seed_x;
    int dirty_bottom = seed_y;
    std::size_t iterations = 0;

    auto eligible = [&](int x, int y) {
        return destination.contains(x, y) && !destination.test(x, y) &&
               condition(x, y) == seed_class;
    };
    auto write = [&](int x, int y) {
        destination.set(x, y);
        ++output.count;
    };

    while (!stack.empty()) {
        if (((++iterations) & 2047u) == 0 && stop.stop_requested())
            throw Cancelled{};
        const auto line = stack.back();
        stack.pop_back();
        const int y = line.base_y + line.dy;
        if (y < 0 || y >= destination.height()) continue;

        // sub_5F7A70 widens x from the popped descriptor before proving a
        // write, hence its caller-visible horizontal bounds are conservative.
        dirty_left = std::min(dirty_left, line.left);
        dirty_right = std::max(dirty_right, line.right);
        bool row_wrote = false;
        int x = std::max(0, line.left);
        const int parent_right = std::min(line.right, destination.width() - 1);
        while (x <= parent_right) {
            while (x <= parent_right && !eligible(x, y)) ++x;
            if (x > parent_right) break;

            int left = x;
            while (left >= 0 && eligible(left, y)) {
                write(left, y);
                row_wrote = true;
                --left;
            }
            ++left;
            if (left < line.left && y - line.dy >= 0 &&
                y - line.dy < destination.height()) {
                stack.push_back({y, left, line.left - 1, -line.dy});
            }

            int right = x + 1;
            while (right < destination.width() && eligible(right, y)) {
                write(right, y);
                row_wrote = true;
                ++right;
            }
            if (y + line.dy >= 0 && y + line.dy < destination.height())
                stack.push_back({y, left, right - 1, line.dy});
            if (right - 1 > line.right && y - line.dy >= 0 &&
                y - line.dy < destination.height()) {
                stack.push_back({y, line.right + 1, right - 1, -line.dy});
            }
            x = right + 1;
        }
        if (row_wrote) {
            dirty_top = std::min(dirty_top, y);
            dirty_bottom = std::max(dirty_bottom, y);
        }
        if (stop_at_horizontal_edge &&
            (dirty_left <= 0 || dirty_right >= destination.width() - 1)) {
            output.leaked = true;
            break;
        }
    }

    output.ok = output.count != 0;
    if (output.ok) {
        output.dirty = {
            std::max(0, dirty_left), std::max(0, dirty_top),
            std::min(destination.width(), dirty_right + 1),
            std::min(destination.height(), dirty_bottom + 1),
        };
    }
    return output;
}

Bounds close_gap_seed_window(int width, int height,
                             int seed_x, int seed_y, int radius) {
    const int reach = (radius >> 1) + 2;
    return intersect_bounds({0, 0, width, height},
                            {seed_x - reach, seed_y - reach,
                             seed_x + reach, seed_y + reach});
}

BitMask build_close_gap_condition(const BitMask& work,
                                  Bounds seed_window,
                                  int radius,
                                  std::stop_token stop) {
    BitMask condition(work.width(), work.height());
    const int half = radius >> 1;
    const int parity = radius & 1;
    std::size_t iterations = 0;
    for (int y = 0; y < work.height(); ++y) {
        for (int x = 0; x < work.width(); ++x) {
            if (((++iterations) & 4095u) == 0 && stop.stop_requested()) throw Cancelled{};
            if (work.test(x, y)) continue;
            condition.set(x, y);
            const bool cardinal = has_set_four_neighbor(work, x, y);
            if (!cardinal) continue;
            const int left = std::max(0, x - half);
            const int right = std::min(work.width() - 1, x + half + parity);
            const int top = std::max(0, y - half);
            const int bottom = std::min(work.height() - 1, y + half + parity);
            for (int px = left; px <= right; ++px) {
                condition.set(px, top);
                condition.set(px, bottom);
            }
            for (int py = top; py <= bottom; ++py) {
                condition.set(left, py);
                condition.set(right, py);
            }
        }
    }

    for (int y = seed_window.top; y < seed_window.bottom; ++y) {
        for (int x = seed_window.left; x < seed_window.right; ++x) {
            if (work.test(x, y)) condition.set(x, y, false);
        }
    }
    return condition;
}

void seed_window_cleanup(BitMask& result,
                         const BitMask& work,
                         Bounds seed_window,
                         Bounds dirty,
                         int radius,
                         std::stop_token stop) {
    if (contains_bounds(expand_bounds(seed_window, 1), dirty)) return;
    const int half = radius >> 1;
    const int parity = radius & 1;
    std::size_t iterations = 0;
    for (int y = seed_window.top; y < seed_window.bottom; ++y) {
        for (int x = seed_window.left; x < seed_window.right; ++x) {
            bool complete = true;
            const Bounds neighborhood = intersect_bounds(
                {0, 0, work.width(), work.height()},
                {x - half, y - half, x + half + parity, y + half + parity});
            for (int ny = neighborhood.top; complete && ny < neighborhood.bottom; ++ny) {
                for (int nx = neighborhood.left; nx < neighborhood.right; ++nx) {
                    if (((++iterations) & 4095u) == 0 && stop.stop_requested())
                        throw Cancelled{};
                    if (!work.test(nx, ny)) {
                        complete = false;
                        break;
                    }
                }
            }
            if (!complete) result.set(x, y, false);
        }
    }
}

void grow_and_peel_close_gap(BitMask& result,
                             const BitMask& work,
                             Bounds& dirty,
                             int radius,
                             std::stop_token stop) {
    const Bounds image{0, 0, result.width(), result.height()};
    Bounds grow = intersect_bounds(image, expand_bounds(dirty, radius + 1));
    if (grow.empty()) return;
    Bounds copy{grow.left & ~31, grow.top,
                std::min(result.width(), (grow.right & ~31) + 32), grow.bottom};
    BitMask previous(result.width(), result.height());
    for (int y = copy.top; y < copy.bottom; ++y)
        for (int x = copy.left; x < copy.right; ++x)
            previous.set(x, y, result.test(x, y));
    BitMask next = previous;

    std::size_t iterations = 0;
    for (int pass = 0; pass < radius + 1; ++pass) {
        for (int y = copy.top; y < copy.bottom; ++y) {
            for (int x = copy.left; x < copy.right; ++x) {
                if (((++iterations) & 4095u) == 0 && stop.stop_requested()) throw Cancelled{};
                if (next.test(x, y) || !work.test(x, y)) continue;
                if (has_set_four_neighbor(previous, x, y)) {
                    next.set(x, y);
                    dirty = union_bounds(dirty, {x, y, x + 1, y + 1});
                }
            }
        }
        previous = next;
    }
    for (int pass = 0; pass < (radius + 1) / 4; ++pass) {
        for (int y = copy.top; y < copy.bottom; ++y) {
            for (int x = copy.left; x < copy.right; ++x) {
                if (((++iterations) & 4095u) == 0 && stop.stop_requested()) throw Cancelled{};
                if (previous.test(x, y) || !work.test(x, y)) continue;
                if (x > copy.left) next.set(x - 1, y, false);
                if (x + 1 < copy.right) next.set(x + 1, y, false);
                if (y > copy.top) next.set(x, y - 1, false);
                if (y + 1 < copy.bottom) next.set(x, y + 1, false);
            }
        }
        previous = next;
    }
    for (int y = copy.top; y < copy.bottom; ++y)
        for (int x = copy.left; x < copy.right; ++x)
            result.set(x, y, previous.test(x, y));
}

struct GridPoint {
    int x{};
    int y{};
    auto operator<=>(const GridPoint&) const = default;
};

struct GridSegment {
    GridPoint first;
    GridPoint second;
};

constexpr std::array<GridPoint, 8> boundary_deltas{{
    {1, 0}, {1, -1}, {0, -1}, {-1, -1},
    {-1, 0}, {-1, 1}, {0, 1}, {1, 1},
}};

int wrap_direction(int value) {
    value %= 8;
    return value < 0 ? value + 8 : value;
}

bool boundary_step(const BitMask& result,
                   const BitMask& work,
                   GridPoint& point,
                   int& direction,
                   bool forward) {
    int probe = wrap_direction(direction + (forward ? 3 : -3));
    for (int attempt = 0; attempt < 8; ++attempt) {
        direction = probe;
        const auto delta = boundary_deltas[probe];
        const GridPoint candidate{point.x + delta.x, point.y + delta.y};
        bool accepted = work.contains(candidate.x, candidate.y) &&
                        !result.test(candidate.x, candidate.y) &&
                        work.test(candidate.x, candidate.y);
        if (accepted && (probe & 1)) {
            const int side_direction = wrap_direction(probe + (forward ? -1 : 1));
            const auto side_delta = boundary_deltas[side_direction];
            const GridPoint side{point.x + side_delta.x, point.y + side_delta.y};
            accepted = work.contains(side.x, side.y) &&
                       !result.test(side.x, side.y) && work.test(side.x, side.y);
        }
        if (accepted) {
            point = candidate;
            return true;
        }
        probe = wrap_direction(probe + (forward ? -1 : 1));
    }
    direction = probe;
    return false;
}

struct BoundaryStart {
    int direction{};
    bool forward_terminal{};
    bool reverse_terminal{};
};

std::optional<BoundaryStart> boundary_start(const BitMask& result,
                                            const BitMask& work,
                                            GridPoint point) {
    const int x = point.x;
    const int y = point.y;
    if (result.test(x - 1, y)) {
        return BoundaryStart{
            0,
            y == 0 || !work.test(x, y - 1),
            y + 1 == work.height() || !work.test(x, y + 1),
        };
    }
    if (result.test(x + 1, y)) {
        return BoundaryStart{
            4,
            y + 1 == work.height() || !work.test(x, y + 1),
            y == 0 || !work.test(x, y - 1),
        };
    }
    if (result.test(x, y - 1)) {
        return BoundaryStart{
            6,
            x + 1 == work.width() || !work.test(x + 1, y),
            x == 0 || !work.test(x - 1, y),
        };
    }
    if (result.test(x, y + 1)) {
        return BoundaryStart{
            2,
            x == 0 || !work.test(x - 1, y),
            x + 1 == work.width() || !work.test(x + 1, y),
        };
    }
    return {};
}

int squared_distance(GridPoint first, GridPoint second) {
    const auto dx = std::int64_t(first.x) - second.x;
    const auto dy = std::int64_t(first.y) - second.y;
    const auto value = dx * dx + dy * dy;
    return value > std::numeric_limits<int>::max()
        ? std::numeric_limits<int>::max() : static_cast<int>(value);
}

std::pair<std::size_t, int> nearest_point(const std::vector<GridPoint>& points,
                                          GridPoint target) {
    std::size_t index = 0;
    int distance = std::numeric_limits<int>::max();
    for (std::size_t candidate = 0; candidate < points.size(); ++candidate) {
        const int current = squared_distance(points[candidate], target);
        if (current <= distance) {
            index = candidate;
            distance = current;
        }
    }
    return {index, distance};
}

bool trace_close_gap_segments(const BitMask& result,
                              const BitMask& work,
                              Bounds trace,
                              int radius,
                              std::vector<GridSegment>& segments,
                              std::stop_token stop) {
    BitMask visited(result.width(), result.height());
    bool rebuild = false;
    const auto radius_plus_one = std::int64_t(radius) + 1;
    const int gate = static_cast<int>(std::min<std::int64_t>(
        std::numeric_limits<int>::max(),
        2 * radius_plus_one * radius_plus_one));
    std::size_t iterations = 0;

    for (int y = trace.top; y < trace.bottom; ++y) {
        for (int x = trace.left; x < trace.right; ++x) {
            if (((++iterations) & 2047u) == 0 && stop.stop_requested())
                throw Cancelled{};
            if (result.test(x, y) || !work.test(x, y) ||
                !has_set_four_neighbor(result, x, y) || visited.test(x, y)) continue;

            const GridPoint candidate{x, y};
            visited.set(x, y);
            const auto start = boundary_start(result, work, candidate);
            if (!start) continue;

            GridPoint forward_point = candidate;
            int forward_direction = start->direction;
            bool forward_looped = false;
            if (!start->forward_terminal) {
                for (;;) {
                    if (((++iterations) & 2047u) == 0 && stop.stop_requested())
                        throw Cancelled{};
                    boundary_step(result, work, forward_point,
                                  forward_direction, true);
                    visited.set(forward_point.x, forward_point.y);
                    if (forward_point == candidate) {
                        forward_looped = true;
                        break;
                    }
                    if (has_clear_in_bounds_four_neighbor(
                            work, forward_point.x, forward_point.y)) break;
                }
            }
            if (forward_looped) continue;

            GridPoint reverse_point = candidate;
            int reverse_direction = start->direction;
            if (!start->reverse_terminal) {
                for (;;) {
                    boundary_step(result, work, reverse_point,
                                  reverse_direction, false);
                    visited.set(reverse_point.x, reverse_point.y);
                    if (has_clear_in_bounds_four_neighbor(
                            work, reverse_point.x, reverse_point.y)) break;
                    if (((++iterations) & 2047u) == 0 && stop.stop_requested())
                        throw Cancelled{};
                }
            }

            const GridPoint initial_forward = forward_point;
            const GridPoint initial_reverse = reverse_point;
            std::vector<GridPoint> forward_endpoints;
            std::vector<GridPoint> reverse_endpoints;
            std::size_t best_forward = 0;
            std::size_t best_reverse = 0;
            int best_distance = gate;
            bool closed_loop = false;

            for (;;) {
                const bool forward_exposed = has_clear_in_bounds_four_neighbor(
                    work, forward_point.x, forward_point.y);
                const bool reverse_exposed = has_clear_in_bounds_four_neighbor(
                    work, reverse_point.x, reverse_point.y);
                if (forward_exposed) forward_endpoints.push_back(forward_point);
                if (reverse_exposed) reverse_endpoints.push_back(reverse_point);

                const auto [nearest_forward, forward_distance] =
                    nearest_point(forward_endpoints, reverse_point);
                const auto [nearest_reverse, reverse_distance] =
                    nearest_point(reverse_endpoints, forward_point);
                if (forward_distance >= gate || reverse_distance >= gate) break;

                if (reverse_exposed && forward_distance <= best_distance) {
                    best_distance = forward_distance;
                    best_forward = nearest_forward;
                    best_reverse = reverse_endpoints.size() - 1;
                }
                if (forward_exposed && reverse_distance <= best_distance) {
                    best_distance = reverse_distance;
                    best_forward = forward_endpoints.size() - 1;
                    best_reverse = nearest_reverse;
                }

                GridPoint next_forward = forward_point;
                GridPoint next_reverse = reverse_point;
                if (!boundary_step(result, work, next_forward,
                                   forward_direction, true) ||
                    !boundary_step(result, work, next_reverse,
                                   reverse_direction, false)) break;
                forward_point = next_forward;
                reverse_point = next_reverse;
                if (reverse_point == initial_forward &&
                    forward_point == initial_reverse) {
                    closed_loop = true;
                    break;
                }
                if (((++iterations) & 2047u) == 0 && stop.stop_requested())
                    throw Cancelled{};
            }

            rebuild = true;
            if (!closed_loop) {
                if (!forward_endpoints.empty() && !reverse_endpoints.empty()) {
                    segments.push_back({forward_endpoints[best_forward],
                                        reverse_endpoints[best_reverse]});
                } else {
                    segments.push_back({initial_forward, initial_reverse});
                }
            }
        }
    }
    return rebuild;
}

void draw_mask_line(BitMask& mask, GridPoint first, GridPoint second, bool value) {
    int x = first.x;
    int y = first.y;
    const int dx = std::abs(second.x - first.x);
    const int sx = first.x < second.x ? 1 : -1;
    const int dy = -std::abs(second.y - first.y);
    const int sy = first.y < second.y ? 1 : -1;
    int error = dx + dy;
    for (;;) {
        mask.set(x, y, value);
        if (x == second.x && y == second.y) break;
        const int doubled = 2 * error;
        if (doubled >= dy) {
            error += dy;
            x += sx;
        }
        if (doubled <= dx) {
            error += dx;
            y += sy;
        }
    }
}

bool close_gap_closure(BitMask& result,
                       BitMask& work,
                       int seed_x,
                       int seed_y,
                       int radius,
                       bool stop_at_horizontal_edge,
                       bool& leaked,
                       std::stop_token stop,
                       Bounds* reported_bounds = nullptr) {
    if (radius <= 0) return false;
    const Bounds image{0, 0, result.width(), result.height()};
    const Bounds seed_window = close_gap_seed_window(
        result.width(), result.height(), seed_x, seed_y, radius);
    const BitMask auxiliary = build_close_gap_condition(
        work, seed_window, radius, stop);
    auto initial = mask_flood(result, seed_x, seed_y,
                              [&](int x, int y) { return auxiliary.test(x, y); },
                              stop_at_horizontal_edge, stop);
    if (!initial.ok) return false;
    leaked = initial.leaked;
    if (reported_bounds) *reported_bounds = initial.dirty;
    if (leaked) return true;

    Bounds dirty = initial.dirty;
    seed_window_cleanup(result, work, seed_window, dirty, radius, stop);
    grow_and_peel_close_gap(result, work, dirty, radius, stop);

    const int pad = radius * 3;
    const Bounds trace = intersect_bounds(image, expand_bounds(dirty, pad));
    std::vector<GridSegment> segments;
    const bool rebuild = trace_close_gap_segments(
        result, work, trace, radius, segments, stop);
    BitMask scratch(result.width(), result.height());
    if (rebuild) {
        result.clear();
        scratch = work;
        for (const auto& segment : segments)
            draw_mask_line(scratch, segment.first, segment.second, false);
        auto rebuilt = mask_flood(result, seed_x, seed_y,
                                  [&](int x, int y) { return scratch.test(x, y); },
                                  false, stop);
        if (rebuilt.ok) dirty = rebuilt.dirty;
        scratch.clear();
    } else {
        dirty = trace;
    }

    const Bounds subtract{
        std::max(0, dirty.left & ~31), std::max(0, dirty.top),
        std::min(work.width(), (dirty.right & ~31) + 32),
        std::min(work.height(), dirty.bottom),
    };
    for (int y = subtract.top; y < subtract.bottom; ++y)
        for (int x = subtract.left; x < subtract.right; ++x)
            if (result.test(x, y)) work.set(x, y, false);

    const Bounds leftover_scan = intersect_bounds(
        image, expand_bounds(dirty, pad));
    const auto limit64 = std::int64_t(pad) * pad;
    const std::size_t component_limit = static_cast<std::size_t>(
        std::min<std::int64_t>(limit64, std::numeric_limits<int>::max()));
    for (int y = leftover_scan.top; y < leftover_scan.bottom; ++y) {
        for (int x = leftover_scan.left; x < leftover_scan.right; ++x) {
            if (!work.test(x, y) || !has_set_four_neighbor(result, x, y)) continue;
            auto component = mask_flood(
                scratch, x, y,
                [&](int px, int py) { return work.test(px, py); }, false, stop);
            if (!component.ok) continue;
            const bool accept = component.count <= component_limit;
            for (int cy = component.dirty.top; cy < component.dirty.bottom; ++cy) {
                for (int cx = component.dirty.left; cx < component.dirty.right; ++cx) {
                    if (!scratch.test(cx, cy)) continue;
                    if (accept) result.set(cx, cy);
                    work.set(cx, cy, false);
                    scratch.set(cx, cy, false);
                }
            }
            if (accept) dirty = union_bounds(dirty, component.dirty);
        }
    }
    if (reported_bounds) *reported_bounds = dirty;
    return true;
}

bool line_family(std::uint32_t family) {
    switch (family) {
    case 0x11110001:
    case 0x11110002:
    case 0x11410001:
    case 0x11210001:
    case 0x11220001:
    case 0x11240001:
        return true;
    default:
        return false;
    }
}

bool paint_plane_family(std::uint32_t family) {
    return family == 0x12100001 || family == 0x12100002 || family == 0x12400001;
}

bool valid_reference(const FillReference& reference) {
    return reference.raster &&
           std::ranges::all_of(reference.target_to_reference,
                               [](double value) { return std::isfinite(value); });
}

std::optional<Pixel> sample_reference(const FillReference& reference, Point point) {
    if (!valid_reference(reference)) return {};
    const auto& transform = reference.target_to_reference;
    const double x = transform[0] * point.x + transform[1] * point.y + transform[2];
    const double y = transform[3] * point.x + transform[4] * point.y + transform[5];
    if (!std::isfinite(x) || !std::isfinite(y) || x < 0 || y < 0 ||
        x >= reference.raster->width || y >= reference.raster->height) return {};
    return reference.raster->pixel(static_cast<int>(std::floor(x)),
                                   static_cast<int>(std::floor(y)));
}

std::optional<FillResult> append_light_references(
    const BucketFillSettings& settings,
    Point seed,
    Pixel& color,
    const FillReferences& references,
    std::vector<LineView>& lines) {
    if (settings.use_light_line && !settings.fill_separated) {
        if (references.light_line && !valid_reference(*references.light_line)) {
            FillResult result;
            result.handled = false;
            result.error = "The supplied light-line reference is malformed";
            return result;
        }
        if (references.light_line) {
            lines.push_back({references.light_line->raster,
                             references.light_line->target_to_reference,
                             references.light_line->white_is_empty});
        }
    }
    if (settings.use_light_color) {
        if (references.light_color && !valid_reference(*references.light_color)) {
            FillResult result;
            result.handled = false;
            result.error = "The supplied light-color reference is malformed";
            return result;
        }
        if (references.light_color) {
            if (const auto sampled = sample_reference(*references.light_color, seed))
                color = *sampled;
        }
    }
    return {};
}

FillResult fill_impl(Raster& target,
                     Point seed,
                     Pixel color,
                     const BucketFillSettings& settings,
                     std::span<const LineView> lines,
                     int target_mode,
                     bool black_guard,
                     const std::optional<RasterSelection>& selection,
                     std::stop_token stop,
                     Rect* consecutive_scan = nullptr,
                     std::optional<Bounds> consecutive_bounds = {}) {
    FillResult result;
    if (settings.color_margin < 0 || settings.color_margin > 255) {
        result.handled = false;
        result.error = "Color margin must be between 0 and 255";
        return result;
    }
    if (!raster_stroke_surface_supported(target)) {
        result.handled = false;
        result.error = "The active raster depth needs a plane-specific fill adapter";
        return result;
    }
    if (!std::isfinite(seed.x) || !std::isfinite(seed.y) ||
        seed.x < std::numeric_limits<int>::min() || seed.x > std::numeric_limits<int>::max() ||
        seed.y < std::numeric_limits<int>::min() || seed.y > std::numeric_limits<int>::max()) {
        return result;
    }
    const int seed_x = static_cast<int>(std::floor(seed.x));
    const int seed_y = static_cast<int>(std::floor(seed.y));
    const auto bounds = consecutive_bounds?*consecutive_bounds:clipped_bounds(target, selection);
    if (bounds.empty() || !bounds.contains(seed_x, seed_y)
        || (selection&&!selection->contains(seed_x,seed_y))) return result;
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    try {
        const Raster source = target;
        const Pixel wanted = fill_pixel(source,seed_x, seed_y);
        if(settings.fill_separated&&(source.depth==2||source.depth==16||source.depth==24)){
            Raster output=source;
            if(!offscreen_detail::materialize(output,{seed_x,seed_y,1,1},stop))throw Cancelled{};
            result.storage_changed=source.tiles.size()!=output.tiles.size();
            if(stop.stop_requested())throw Cancelled{};
            if(result.storage_changed)target=std::move(output);
            return result;
        }
        const bool white_is_empty = target_mode == 5;
        const bool scalar_mode = target_mode == 4&&target.depth>=32;
        const auto luma = [](Pixel pixel) {
            return static_cast<std::uint16_t>(
                (std::uint64_t(pixel.g) * 0x258B +
                 std::uint64_t(pixel.b) * 0x0754 +
                 std::uint64_t(pixel.r) * 0x1321) >> 14);
        };
        const int local_seed_x = seed_x - bounds.left;
        const int local_seed_y = seed_y - bounds.top;

        if (settings.fill_separated) {
            if (black_guard && !scalar_mode && source.depth == 32 &&
                exact_opaque_black(wanted, source.depth)) return result;
            BitMask selected(bounds.width(), bounds.height());
            const bool packed_scan = source.depth == 1 || source.depth == 8;
            const bool visit_missing = !packed_scan ||
                selection_detail::raw_pixel(source,seed_x,seed_y) == 0;
            std::size_t inspected = 0;
            for (int y = bounds.top; y < bounds.bottom; ++y) {
                for (int x = bounds.left; x < bounds.right; ++x) {
                    if (((++inspected) & 4095u) == 0 && stop.stop_requested()) throw Cancelled{};
                    if(selection&&!selection->contains(x,y))continue;
                    if(!visit_missing&&!source.tiles.contains({x/256,y/256}))continue;
                    const auto current = fill_pixel(source,x, y);
                    const bool target_black = black_guard && !scalar_mode &&
                        source.depth == 32 &&
                        exact_opaque_black(fill_pixel(source,x, y), source.depth);
                    const bool matches = source.depth==1?true:scalar_mode
                        ? std::abs(int(current.a) - int(wanted.a)) <=
                              settings.color_margin * 257
                        : equal_with_margin(current, wanted,
                                            settings.color_margin,
                                            source.depth, white_is_empty);
                    if (!target_black && matches) {
                        selected.set(x - bounds.left, y - bounds.top);
                    }
                }
            }
            if (stop.stop_requested()) throw Cancelled{};
            DirtyBounds dirty;
            Raster output = source;
            if(packed_scan&&visit_missing&&!offscreen_detail::materialize(output,{bounds.left,bounds.top,bounds.width(),bounds.height()},stop))throw Cancelled{};
            std::size_t write_visits = 0;
            for (int y = 0; y < selected.height(); ++y) {
                for (int x = 0; x < selected.width(); ++x) {
                    if (((++write_visits) & 4095u) == 0 && stop.stop_requested())
                        throw Cancelled{};
                    if (!selected.test(x, y)) continue;
                    const int target_x = bounds.left + x;
                    const int target_y = bounds.top + y;
                    Pixel written = color;
                    if (scalar_mode) {
                        written = fill_pixel(source,target_x, target_y);
                        written.a = luma(color);
                    }
                    written = canonical_write(written, source.depth);
                    if (fill_pixel(source,target_x, target_y) == written) continue;
                    store_bucket_color(output,target_x,target_y,written);
                    dirty.add(target_x, target_y);
                    ++result.changed_pixels;
                }
            }
            result.changed = result.changed_pixels != 0;
            result.storage_changed=source.depth<32&&(result.changed||source.tiles.size()!=output.tiles.size());
            result.dirty = dirty.rect();
            if (stop.stop_requested()) throw Cancelled{};
            if (result.changed||result.storage_changed) target = std::move(output);
            return result;
        }

        const auto mode = static_cast<std::int32_t>(settings.include.mode);
        if (target.depth>=32&&!scalar_mode && (mode < 0 || mode > 2)) {
            result.handled = false;
            result.error = "Unsupported IncludeFill value";
            return result;
        }
        if (settings.close_gap_on &&
            (settings.close_gap_distance_px < 0 ||
             settings.close_gap_distance_px > 200)) {
            result.handled = false;
            result.error = "Close-gap distance must be between 0 and 200 pixels";
            return result;
        }

        // sub_5F4CB0 rejects exact 32-bit black before dispatching to the
        // scalar/full-colour comparison branch.  Per-pixel scalar matching
        // remains coverage-only after this seed gate.
        if (black_guard && source.depth == 32 &&
            exact_opaque_black(wanted, source.depth)) return result;

        PixelClassifier classifier(source, lines, bounds, settings, wanted,
                                   white_is_empty, scalar_mode, black_guard,
                                   selection?&*selection:nullptr, stop);
        BitMask fill_mask(bounds.width(), bounds.height());
        Bounds reported{};
        bool leaked = false;
        if (settings.close_gap_on && settings.close_gap_distance_px != 0) {
            if (!classifier.work(local_seed_x, local_seed_y)) return result;
            BitMask work(bounds.width(), bounds.height());
            for (int y = 0; y < work.height(); ++y) {
                for (int x = 0; x < work.width(); ++x)
                    work.set(x, y, classifier.work(x, y));
            }
            close_gap_closure(fill_mask, work, local_seed_x, local_seed_y,
                              settings.close_gap_distance_px,
                              settings.stop_if_leaked != 0, leaked, stop,&reported);
        } else {
            if (!classifier.work(local_seed_x, local_seed_y)) return result;
            const auto flooded = mask_flood(
                fill_mask, local_seed_x, local_seed_y,
                [&](int x, int y) { return classifier.work(x, y); },
                settings.stop_if_leaked != 0, stop);
            leaked = flooded.leaked;
            reported = flooded.dirty;
        }

        if (target.depth>=32&&!scalar_mode && settings.include.mode == IncludeFillMode::SpecifiedColors &&
            !leaked) {
            const BitMask initial = fill_mask;
            if (initial.test(local_seed_x, local_seed_y) ||
                classifier.support(local_seed_x, local_seed_y)) {
                fill_mask.clear();
                const auto supported = mask_flood(fill_mask, local_seed_x, local_seed_y,
                           [&](int x, int y) {
                               return initial.test(x, y) || classifier.support(x, y);
                           }, false, stop);
                reported = supported.dirty;
            }
        }
        if (stop.stop_requested()) throw Cancelled{};

        DirtyBounds dirty;
        Raster output = source;
        if(source.depth<32&&!reported.empty()&&!offscreen_detail::materialize(output,{bounds.left+reported.left,bounds.top+reported.top,reported.width(),reported.height()},stop))throw Cancelled{};
        if(consecutive_scan&&!reported.empty()){
            // sub_5F57D0 enumerates the reported rectangle before testing mask
            // bits. Missing blocks in its holes still acquire white/A0 data.
            for(int ty=(bounds.top+reported.top)/Raster::tile_side;
                ty<=(bounds.top+reported.bottom-1)/Raster::tile_side;++ty)
                for(int tx=(bounds.left+reported.left)/Raster::tile_side;
                    tx<=(bounds.left+reported.right-1)/Raster::tile_side;++tx){
                    if(stop.stop_requested())throw Cancelled{};
                    if(!output.tiles.contains({tx,ty}))
                        output.set_pixel(tx*Raster::tile_side,ty*Raster::tile_side,Pixel{});
                }
        }
        std::size_t write_visits = 0;
        for (int y = consecutive_scan?reported.top:0; y < (consecutive_scan?reported.bottom:fill_mask.height()); ++y) {
            for (int x = consecutive_scan?reported.left:0; x < (consecutive_scan?reported.right:fill_mask.width()); ++x) {
                if (((++write_visits) & 4095u) == 0 && stop.stop_requested())
                    throw Cancelled{};
                if (!fill_mask.test(x, y)) continue;
                const int target_x = bounds.left + x;
                const int target_y = bounds.top + y;
                Pixel written = color;
                if (scalar_mode) {
                    written = fill_pixel(source,target_x, target_y);
                    written.a = luma(color);
                }
                written = consecutive_scan&&source.depth==32
                    ? Pixel::rgba8(written.r>>8,written.g>>8,written.b>>8,written.a>>8)
                    : canonical_write(written, source.depth);
                const bool different=fill_pixel(source,target_x, target_y)!=written;
                if(!different&&!consecutive_scan)continue;
                store_bucket_color(output,target_x,target_y,written);
                if(different){dirty.add(target_x, target_y);++result.changed_pixels;}
            }
        }
        result.leaked = leaked;
        result.changed = result.changed_pixels != 0;
        result.storage_changed=source.depth<32&&(result.changed||source.tiles.size()!=output.tiles.size());
        result.dirty = dirty.rect();
        if (stop.stop_requested()) throw Cancelled{};
        if (consecutive_scan&&!reported.empty()){
            *consecutive_scan={bounds.left+reported.left,bounds.top+reported.top,reported.width(),reported.height()};
            target=std::move(output);
        }else if (result.changed||result.storage_changed) target = std::move(output);
        return result;
    } catch (const Cancelled&) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.leaked = false;
        result.cancelled = true;
        return result;
    } catch (const std::bad_alloc&) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = "Insufficient memory for fill masks";
        return result;
    } catch (const std::invalid_argument& error) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = error.what();
        return result;
    }
}

bool any_line_evidence(std::span<const LineView> lines, int x, int y) {
    for (const auto& view : lines) {
        const double line_x = view.target_to_line[0] * x +
                              view.target_to_line[1] * y + view.target_to_line[2];
        const double line_y = view.target_to_line[3] * x +
                              view.target_to_line[4] * y + view.target_to_line[5];
        if (!std::isfinite(line_x) || !std::isfinite(line_y) ||
            line_x < 0 || line_y < 0 || line_x >= view.raster->width ||
            line_y >= view.raster->height) continue;
        const auto pixel = fill_pixel(*view.raster,static_cast<int>(std::floor(line_x)),
                                               static_cast<int>(std::floor(line_y)));
        if (!line_sample_empty(pixel, view.raster->depth, view.white_is_empty))
            return true;
    }
    return false;
}

Bounds rect_bounds(Rect rect) {
    if (rect.width <= 0 || rect.height <= 0) return {};
    const auto right = std::int64_t(rect.x) + rect.width;
    const auto bottom = std::int64_t(rect.y) + rect.height;
    return {rect.x, rect.y,
            static_cast<int>(std::clamp<std::int64_t>(
                right, std::numeric_limits<int>::min(),
                std::numeric_limits<int>::max())),
            static_cast<int>(std::clamp<std::int64_t>(
                bottom, std::numeric_limits<int>::min(),
                std::numeric_limits<int>::max()))};
}

BitMask import_binary_mask(const BinaryMask& source,const std::optional<RasterSelection>& selection) {
    BitMask result(source.width(), source.height());
    for (int y = 0; y < source.height(); ++y)
        for (int x = 0; x < source.width(); ++x)
            result.set(x, y, source.test(x, y)&&(!selection||selection->contains(x,y)));
    return result;
}

Bounds set_bit_bounds(const BitMask& mask) {
    Bounds result{};
    bool found = false;
    for (int y = 0; y < mask.height(); ++y) {
        for (int x = 0; x < mask.width(); ++x) {
            if (!mask.test(x, y)) continue;
            result = found ? union_bounds(result, {x, y, x + 1, y + 1})
                           : Bounds{x, y, x + 1, y + 1};
            found = true;
        }
    }
    return found ? result : Bounds{};
}

bool selected_color_match(Pixel value,
                          std::span<const Pixel> colors,
                          int depth,
                          int target_mode) {
    return std::ranges::any_of(colors, [&](Pixel candidate) {
        return list_equal(value, candidate, depth, target_mode == 5);
    });
}

FillResult close_fill_impl(Raster& target,
                           const BinaryMask& request,
                           Rect request_bounds,
                           Pixel color,
                           const CloseFillSettings& settings,
                           int target_mode,
                           std::span<const LineView> lines,
                           bool black_guard,
                           const std::optional<RasterSelection>& selection,
                           std::stop_token stop) {
    FillResult result;
    if (!raster_stroke_surface_supported(target)) {
        result.handled = false;
        result.error = "Close Fill requires an editable 32-bit or 64-bit raster";
        return result;
    }
    if (request.width() != target.width || request.height() != target.height) {
        result.handled = false;
        result.error = "Close Fill request mask must match the target raster";
        return result;
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }

    try {
        const Raster source = target;
        const Bounds selected = clipped_bounds(target, {});
        const Bounds scan = intersect_bounds(
            selected, expand_bounds(rect_bounds(request_bounds), 1));
        if (scan.empty()) return result;
        const bool scalar_mode = target_mode == 4&&target.depth>=32;
        // The host booleanizes the persisted integer before invoking the
        // worker.  Values other than zero therefore all select transparent
        // components; the lower worker's inverse selector is unreachable.
        const bool only_transparent = settings.only_transparent != 0;
        const bool support_policy = target.depth>=32&&!scalar_mode &&
            settings.include.mode == IncludeFillMode::SpecifiedColors;
        const auto luma = [](Pixel pixel) {
            return static_cast<std::uint16_t>(
                (std::uint64_t(pixel.g) * 0x258B +
                 std::uint64_t(pixel.b) * 0x0754 +
                 std::uint64_t(pixel.r) * 0x1321) >> 14);
        };
        const Bounds active = support_policy ? selected : scan;
        const auto support_colors = selected_colors(settings.include);
        // CAreaMaskOp cleans the request first. The solver keeps the original
        // work bounds; its IncludeFill support pass can regenerate cleared bits.
        BitMask reference = import_binary_mask(request,selection);
        BitMask accepted(target.width, target.height);
        std::size_t inspected = 0;
        for (int y = scan.top; y < scan.bottom; ++y) {
            for (int x = scan.left; x < scan.right; ++x) {
                if (((++inspected) & 4095u) == 0 && stop.stop_requested())
                    throw Cancelled{};
                if (!reference.test(x, y)) continue;
                const Pixel value = fill_pixel(source,x, y);
                if (any_line_evidence(lines, x, y)) continue;
                if (only_transparent &&
                    !selection_detail::transparent(source,x,y,target_mode==5)) continue;
                if (black_guard && source.depth == 32 &&
                    exact_opaque_black(value, source.depth)) continue;
                accepted.set(x, y);
            }
        }

        BitMask scratch(target.width, target.height);
        for (int y = scan.top; y < scan.bottom; ++y) {
            for (int x = scan.left; x < scan.right; ++x) {
                if (!accepted.test(x, y) || !reference.test(x, y) ||
                    !has_clear_in_bounds_four_neighbor(reference, x, y)) continue;
                //5F4CB0 has no seed sampler for these depths. The boundary
                //subtraction does not flood the false comparator's complement.
                if(source.depth==2||source.depth==16||source.depth==24)continue;
                const Pixel seed = fill_pixel(source,x, y);
                if (black_guard && source.depth == 32 &&
                    exact_opaque_black(seed, source.depth)) continue;
                // sub_5F4CB0 calls IsTransparent(0) for seed polarity even
                // when bulk pruning used the plane-mode selector.
                const bool transparent_seed = seed.a == 0;
                if (only_transparent && !transparent_seed) continue;
                const auto component = mask_flood(
                    scratch, x, y,
                    [&](int px, int py) {
                        if (!accepted.test(px, py)) return false;
                        if (only_transparent) return true;
                        if (scalar_mode)
                            return fill_pixel(source,px, py).a == seed.a;
                        return equal_with_margin(fill_pixel(source,px, py), seed, 0,
                                                 source.depth, target_mode == 5);
                    }, false, stop);
                if (!component.ok) continue;
                const Bounds subtract{
                    std::max(0, component.dirty.left & ~31), component.dirty.top,
                    std::min(target.width, (component.dirty.right & ~31) + 32),
                    component.dirty.bottom,
                };
                for (int cy = subtract.top; cy < subtract.bottom; ++cy) {
                    for (int cx = subtract.left; cx < subtract.right; ++cx) {
                        if (scratch.test(cx, cy)) accepted.set(cx, cy, false);
                        scratch.set(cx, cy, false);
                    }
                }
            }
        }

        Bounds solved{};
        if (support_policy) {
            bool found = false;
            BitMask support_scratch(target.width, target.height);
            for (int y = scan.top; y < scan.bottom; ++y) {
                for (int x = scan.left; x < scan.right; ++x) {
                    if (!accepted.test(x, y) || support_scratch.test(x, y)) continue;
                    const auto component = mask_flood(
                        support_scratch, x, y,
                        [&](int px, int py) {
                            if (!active.contains(px, py)) return false;
                            return accepted.test(px, py) ||
                                   selected_color_match(fill_pixel(source,px, py),
                                                        support_colors,
                                                        source.depth, target_mode);
                        }, false, stop);
                    if (!component.ok) continue;
                    solved = found ? union_bounds(solved, component.dirty)
                                   : component.dirty;
                    found = true;
                }
            }
            if (!found) return result;
            const Bounds copy{
                std::max(0, solved.left & ~31), solved.top,
                std::min(target.width, (solved.right & ~31) + 32), solved.bottom,
            };
            for (int y = copy.top; y < copy.bottom; ++y)
                for (int x = copy.left; x < copy.right; ++x)
                    accepted.set(x, y, support_scratch.test(x, y));
        } else {
            solved = set_bit_bounds(accepted);
            if (solved.empty()) return result;
        }

        if (stop.stop_requested()) throw Cancelled{};
        Raster output = source;
        if(source.depth<32&&!offscreen_detail::materialize(output,{solved.left,solved.top,solved.width(),solved.height()},stop))throw Cancelled{};
        std::size_t write_visits = 0;
        for (int y = solved.top; y < solved.bottom; ++y) {
            for (int x = solved.left; x < solved.right; ++x) {
                if (((++write_visits) & 4095u) == 0 && stop.stop_requested())
                    throw Cancelled{};
                if (!accepted.test(x, y)) continue;
                if(source.depth==2||source.depth==16||source.depth==24)continue;
                Pixel written = color;
                if (scalar_mode) {
                    written = fill_pixel(source,x, y);
                    written.a = luma(color);
                }
                written = canonical_write(written, source.depth);
                if (fill_pixel(source,x, y) == written) continue;
                store_bucket_color(output,x,y,written);
                ++result.changed_pixels;
            }
        }
        result.changed = result.changed_pixels != 0;
        result.storage_changed=source.depth<32&&(result.changed||source.tiles.size()!=output.tiles.size());
        //5FB740 calls5F57D0/604250 for every nonempty solved mask,
        // including same-color writes and native no-write depth leaves.
        result.performed = true;
        result.dirty = {solved.left,solved.top,solved.width(),solved.height()};
        if (stop.stop_requested()) throw Cancelled{};
        if (result.changed||result.storage_changed) target = std::move(output);
        return result;
    } catch (const Cancelled&) {
        result.changed = false;
        result.storage_changed = false;
        result.performed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.cancelled = true;
        return result;
    } catch (const std::bad_alloc&) {
        result.changed = false;
        result.storage_changed = false;
        result.performed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = "Insufficient memory for Close Fill masks";
        return result;
    } catch (const std::invalid_argument& error) {
        result.changed = false;
        result.storage_changed = false;
        result.performed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = error.what();
        return result;
    }
}

bool spread_topology_accept(const BitMask& occupancy, int x, int y,
                            std::int32_t choice) {
    const auto q = [&](int dx, int dy) { return occupancy.test(x + dx, y + dy); };
    const bool b=q(-1,-2), c=q(0,-2), d=q(1,-2);
    const bool f=q(-2,-1), g=q(-1,-1), h=q(0,-1), i=q(1,-1), j=q(2,-1);
    const bool k=q(-2,0), l=q(-1,0), n=q(1,0), o=q(2,0);
    const bool p=q(-2,1), qv=q(-1,1), r=q(0,1), s=q(1,1), t=q(2,1);
    const bool v=q(-1,2), w=q(0,2), xv=q(1,2);

    const int orthogonal = int(h) + int(l) + int(n) + int(r);
    if (orthogonal >= 3) return true;
    if (choice == 1) return false;

    const bool medium =
        (h&&r) || (l&&n) || (g&&i&&r) || (g&&n&&qv) || (g&&n&&r) ||
        (h&&l&&s) || (h&&n&&qv) || (h&&qv&&s) || (i&&l&&r) ||
        (i&&l&&s) || (g&&i&&qv&&s) ||
        (b&&c&&f&&i&&k&&n&&qv&&r) ||
        (c&&d&&g&&j&&l&&o&&r&&s) ||
        (g&&h&&k&&n&&p&&s&&v&&w) ||
        (h&&i&&l&&o&&qv&&t&&w&&xv);
    if (medium) return true;
    if (choice == 2) return false;

    return (b&&c&&d&&qv&&r&&s) ||
           (b&&c&&f&&s&&v&&w) ||
           (b&&c&&i&&p&&v&&w) ||
           (c&&g&&k&&o&&s&&w) ||
           (c&&i&&k&&o&&qv&&w) ||
           (f&&i&&k&&n&&p&&s) ||
           (g&&h&&i&&v&&w&&xv) ||
           (g&&j&&l&&o&&qv&&t) ||
           (g&&k&&o&&p&&t&&xv) ||
           (i&&k&&o&&p&&t&&v) ||
           (b&&c&&k&&o&&qv&&t&&w) ||
           (c&&d&&k&&o&&p&&s&&w) ||
           (c&&f&&i&&k&&o&&w&&xv) ||
           (c&&g&&j&&k&&o&&v&&w);
}

FillResult spread_fill_impl(Raster& target,
                            const BinaryMask& stroke,
                            Rect work_bounds,
                            Pixel sampled_color,
                            const SpreadFillSettings& settings,
                            int target_mode,
                            std::span<const LineView> companions,
                            const std::optional<RasterSelection>& selection,
                            std::stop_token stop) {
    FillResult result;
    if (!raster_stroke_surface_supported(target)) {
        result.handled = false;
        result.error = "Spread Fill requires an editable 32-bit or 64-bit raster";
        return result;
    }
    if (target_mode == 4) {
        result.handled = false;
        result.error = "Spread Fill does not target scalar planes";
        return result;
    }
    if (stroke.width() != target.width || stroke.height() != target.height) {
        result.handled = false;
        result.error = "Spread Fill stroke mask must match the target raster";
        return result;
    }
    if (stop.stop_requested()) {
        result.cancelled = true;
        return result;
    }
    // Raster SPFL does not create a session when the press sample has zero
    // coverage. Keep that begin-time gate at this public prepared-mask
    // boundary so callers cannot synthesize a host-unreachable write.
    if (sampled_color.a == 0) return result;

    try {
        const Raster source = target;
        const Bounds work = intersect_bounds(clipped_bounds(target, {}),
                                             rect_bounds(work_bounds));
        if (work.empty()) return result;
        BitMask occupancy(target.width, target.height);
        std::size_t inspected = 0;
        for (int y = work.top; y < work.bottom; ++y) {
            for (int x = work.left; x < work.right; ++x) {
                if (((++inspected) & 4095u) == 0 && stop.stop_requested())
                    throw Cancelled{};
                const bool occupied = !selection_detail::transparent(source,x,y,target_mode==5) ||
                                      any_line_evidence(companions, x, y);
                occupancy.set(x, y, occupied);
            }
        }

        BitMask accepted(target.width, target.height);
        for (int y = work.top; y < work.bottom; ++y) {
            for (int x = work.left; x < work.right; ++x) {
                if (!stroke.test(x, y) || (selection&&!selection->contains(x,y)) || occupancy.test(x, y)) continue;
                if (spread_topology_accept(occupancy, x, y,
                                           settings.effect_choice)) {
                    accepted.set(x, y);
                }
            }
        }
        if (stop.stop_requested()) throw Cancelled{};

        DirtyBounds dirty;
        Raster output = source;
        std::size_t write_visits = 0;
        for (int y = work.top; y < work.bottom; ++y) {
            for (int x = work.left; x < work.right; ++x) {
                if (((++write_visits) & 4095u) == 0 && stop.stop_requested())
                    throw Cancelled{};
                if (!accepted.test(x, y)) continue;
                const auto word=offscreen_detail::color_word(sampled_color,source.depth);
                if(selection_detail::raw_pixel(source,x,y)==word)continue;
                offscreen_detail::store_color(output,x,y,sampled_color);
                dirty.add(x, y);
                ++result.changed_pixels;
            }
        }
        result.changed = result.changed_pixels != 0;
        result.dirty = dirty.rect();
        if (stop.stop_requested()) throw Cancelled{};
        if (result.changed) target = std::move(output);
        return result;
    } catch (const Cancelled&) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.cancelled = true;
        return result;
    } catch (const std::bad_alloc&) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = "Insufficient memory for Spread Fill masks";
        return result;
    } catch (const std::invalid_argument& error) {
        result.changed = false;
        result.storage_changed = false;
        result.changed_pixels = 0;
        result.dirty = {};
        result.handled = false;
        result.error = error.what();
        return result;
    }
}

} // namespace

BinaryMask::BinaryMask(int width, int height) {
    const auto [stride, word_count] = checked_mask_layout(width, height);
    width_ = width;
    height_ = height;
    stride_ = stride;
    words_.assign(word_count, 0);
    blocks_.assign(std::size_t((width+255)/256)*std::size_t((height+255)/256),0);
}
BinaryMask& BinaryMask::operator=(const BinaryMask& other) {
    if(this!=&other){BinaryMask replacement(other);*this=std::move(replacement);}
    return *this;
}

BinaryMask::BinaryMask(const BinaryMask& other)
    :width_(other.width_),height_(other.height_),stride_(other.stride_),
     words_(other.words_),blocks_(other.blocks_) {}

BinaryMask::BinaryMask(BinaryMask&& other) noexcept
    :width_(other.width_),height_(other.height_),stride_(other.stride_),
     words_(std::move(other.words_)),blocks_(std::move(other.blocks_)) {
    other.width_=other.height_=0;other.stride_=0;++other.mutation_generation_;
}

BinaryMask& BinaryMask::operator=(BinaryMask&& other) noexcept {
    if(this!=&other){
        width_=other.width_;height_=other.height_;stride_=other.stride_;
        words_=std::move(other.words_);blocks_=std::move(other.blocks_);
        ++mutation_generation_;
        other.width_=other.height_=0;other.stride_=0;++other.mutation_generation_;
    }
    return *this;
}

bool BinaryMask::contains(int x, int y) const {
    return x >= 0 && y >= 0 && x < width_ && y < height_;
}

bool BinaryMask::test(int x, int y) const {
    if (!contains(x, y)) return false;
    const auto word = std::size_t(y) * stride_ + std::size_t(x >> 5);
    return (words_[word] & (std::uint32_t(1) << (x & 31))) != 0;
}
Rect BinaryMask::extent(Rect region) const noexcept {
    if(region.width<=0||region.height<=0||width_<=0||height_<=0)return {};
    const int left=int(std::clamp<std::int64_t>(region.x,0,width_));
    const int top=int(std::clamp<std::int64_t>(region.y,0,height_));
    const int right=int(std::clamp<std::int64_t>(std::int64_t(region.x)+region.width,left,width_));
    const int bottom=int(std::clamp<std::int64_t>(std::int64_t(region.y)+region.height,top,height_));
    if(left>=right||top>=bottom)return {};
    int x0=right,y0=bottom,x1=left,y1=top;
    const int first=left/32,last=(right-1)/32;
    for(int y=top;y<bottom;++y)for(int word=first;word<=last;++word){
        auto bits=words_[std::size_t(y)*stride_+std::size_t(word)];
        if(word==first)bits&=~std::uint32_t(0)<<(left%32);
        if(word==last&&right%32)bits&=(std::uint32_t(1)<<(right%32))-1;
        if(!bits)continue;
        x0=std::min(x0,word*32+std::countr_zero(bits));x1=std::max(x1,word*32+32-std::countl_zero(bits));
        y0=std::min(y0,y);y1=y+1;
    }
    return x0>=x1?Rect{}:Rect{x0,y0,x1-x0,y1-y0};
}

void BinaryMask::set(int x, int y, bool value) {
    if (!contains(x, y)) return;
    const auto word = std::size_t(y) * stride_ + std::size_t(x >> 5);
    const auto bit = std::uint32_t(1) << (x & 31);
    const auto before=words_[word];
    if (value) words_[word] |= bit;else words_[word] &= ~bit;
    if(words_[word]!=before)++mutation_generation_;
    if(value)materialize_block(x/256,y/256);
}

bool BinaryMask::block_present(int x,int y) const noexcept {
    const int columns=(width_+255)/256,rows=(height_+255)/256;
    return x>=0&&y>=0&&x<columns&&y<rows&&blocks_[std::size_t(y)*columns+x]!=0;
}
void BinaryMask::materialize_block(int x,int y) noexcept {
    const int columns=(width_+255)/256,rows=(height_+255)/256;
    if(x>=0&&y>=0&&x<columns&&y<rows){auto& block=blocks_[std::size_t(y)*columns+x];if(!block){block=1;++mutation_generation_;}}
}
void BinaryMask::release_block(int x,int y) noexcept {
    if(!block_present(x,y))return;
    const int columns=(width_+255)/256,left=x*256,top=y*256;
    const int right=std::min(left+256,width_),bottom=std::min(top+256,height_);
    for(int row=top;row<bottom;++row)
        std::fill(words_.begin()+std::size_t(row)*stride_+left/32,
                  words_.begin()+std::size_t(row)*stride_+(right+31)/32,0);
    blocks_[std::size_t(y)*columns+x]=0;
    ++mutation_generation_;
}
void BinaryMask::optimize_blocks(Rect region) noexcept {
    if(region.width<=0||region.height<=0)return;
    const int left=int(std::clamp<std::int64_t>(region.x,0,width_)),top=int(std::clamp<std::int64_t>(region.y,0,height_));
    const int right=int(std::clamp<std::int64_t>(std::int64_t(region.x)+region.width,left,width_));
    const int bottom=int(std::clamp<std::int64_t>(std::int64_t(region.y)+region.height,top,height_));
    if(left>=right||top>=bottom)return;
    for(int y=top/256;y<=(bottom-1)/256;++y)for(int x=left/256;x<=(right-1)/256;++x)
        if(block_present(x,y)&&extent({x*256,y*256,256,256}).width==0)release_block(x,y);
}

void BinaryMask::clear() {
    if(std::ranges::any_of(words_,[](auto word){return word!=0;})||std::ranges::any_of(blocks_,[](auto block){return block!=0;}))++mutation_generation_;
    std::fill(words_.begin(), words_.end(), 0);
    std::fill(blocks_.begin(), blocks_.end(),0);
}

BucketFillSettings& FillSettings::active_bucket() {
    return bucket_banks[std::clamp<std::int32_t>(current_set, 0, 1)];
}

const BucketFillSettings& FillSettings::active_bucket() const {
    return bucket_banks[std::clamp<std::int32_t>(current_set, 0, 1)];
}

std::array<Pixel, 3> fixed_include_colors() {
    // Original low/high pairs:
    // {0000ffff, ffff0000}, {ffff0000, ffff0000}, {00000000, ffffffff}.
    return {Pixel{0, 0, 65535, 65535},
            Pixel{0, 65535, 0, 65535},
            Pixel{65535, 0, 0, 65535}};
}

namespace {

int consecutive_length_pixels(const Length& length,double dpi){
    using numeric_detail::binary64;
    if(length.unit==4)return 0;
    if(!std::isfinite(length.value))throw std::invalid_argument("BFIL page boundary is not finite");
    double value{};
    switch(length.unit){
    case 0:if(!std::isfinite(length.reference)||length.reference<=0)throw std::invalid_argument("BFIL page reference resolution is invalid");value=binary64(binary64(length.value/length.reference)*dpi);break;
    case 1:value=binary64(binary64(dpi*length.value)*0.3937007874015748);break;
    case 2:value=binary64(binary64(dpi*length.value)*0.03937007874015748);break;
    case 3:value=binary64(dpi*length.value);break;
    case 5:value=binary64(binary64(dpi*length.value)*0.01388888888888889);break;
    case 6:value=binary64(binary64(dpi*length.value)*0.00984251968503937);break;
    default:throw std::invalid_argument("BFIL page length unit is unsupported");
    }
    const auto result=legacy_rt_double_to_int(value);if(!result)throw std::invalid_argument("BFIL page boundary exceeds integer domain");return *result;
}
Bounds consecutive_work_bounds(const DocumentData& document,const Layer& owner,const Raster& raster,Point seed){
    const auto dpi=document.base.dpi;
    if(!std::isfinite(dpi)||dpi<=0||owner.common.resolution!=dpi)throw std::invalid_argument("BFIL owner and document DPI must match");
    const auto left=std::int64_t(consecutive_length_pixels(document.base.lengths[6],dpi))-owner.common.x;
    const auto top=std::int64_t(consecutive_length_pixels(document.base.lengths[7],dpi))-owner.common.y;
    const auto right=std::int64_t(document.base.width())-consecutive_length_pixels(document.base.lengths[8],dpi)-owner.common.x;
    const auto bottom=std::int64_t(document.base.height())-consecutive_length_pixels(document.base.lengths[9],dpi)-owner.common.y;
    // Native5FAB20 uses the document inner frame iff it contains this seed.
    // It otherwise falls back to the physical offscreen, without selection.
    if(seed.x<left||seed.x>=right||seed.y<top||seed.y>=bottom)return {0,0,raster.width,raster.height};
    if(left<0||top<0||right>raster.width||bottom>raster.height)
        throw std::invalid_argument("BFIL inner frame samples outside the physical raster; this adapter does not clamp it");
    return {int(left),int(top),int(right),int(bottom)};
}

bool lower_bucket_gap_distance(const Layer& owner,
                               const Plane& target,
                               bool metric_units,
                               BucketFillSettings& settings,
                               std::string& error) {
    // Live Bucket resolves neither distance channel on its independent
    // FillSeparated path or while close-gap is disabled.
    if (!settings.close_gap_on || settings.fill_separated) return true;
    const double target_resolution = target.common.resolution;
    if (!std::isfinite(target_resolution) || target_resolution <= 0) {
        error = "The active plane has an invalid close-gap resolution";
        return false;
    }
    double pixels{};
    if (metric_units) {
        if (!std::isfinite(settings.close_gap_distance_mm)) {
            error = "The metric close-gap distance is not finite";
            return false;
        }
        // sub_56AA40: rtDouble2Int(mm * dpi * (1 / 25.4)).
        pixels = numeric_detail::binary64(settings.close_gap_distance_mm * target_resolution);
        pixels = numeric_detail::binary64(pixels * (1.0 / 25.4));
    } else {
        const double owner_resolution = owner.common.resolution;
        if (!std::isfinite(owner_resolution) || owner_resolution <= 0) {
            error = "The owner layer has an invalid close-gap resolution";
            return false;
        }
        // The pixel channel is stored in owner-layer units and scaled into
        // the current plane's resolution before the worker sees it.
        pixels = numeric_detail::binary64(double(settings.close_gap_distance_px) / owner_resolution);
        pixels = numeric_detail::binary64(pixels * target_resolution);
    }
    if (!std::isfinite(pixels)) {
        error = "The resolved close-gap distance is not finite";
        return false;
    }
    // Only the metric branch calls rtDouble2Int. Pixel-unit lowering uses
    // __ftol2 directly, which truncates toward zero (sub_589970).
    const auto metric_rounded = metric_units ? legacy_rt_double_to_int(pixels)
                                            : std::optional<std::int32_t>{};
    if (metric_units && !metric_rounded) {
        error = "The resolved close-gap distance exceeds supported limits";
        return false;
    }
    const double rounded = metric_units ? double(*metric_rounded) : std::trunc(pixels);
    if (rounded < std::numeric_limits<std::int32_t>::min() ||
        rounded > std::numeric_limits<std::int32_t>::max()) {
        error = "The resolved close-gap distance exceeds supported limits";
        return false;
    }
    settings.close_gap_distance_px = static_cast<std::int32_t>(rounded);
    return true;
}

} // namespace

FillResult fill_raster(Raster& target,
                       Point seed,
                       Pixel color,
                       const BucketFillSettings& settings,
                       const Raster* line,
                       const std::optional<RasterSelection>& selection,
                       std::stop_token stop,
                       const FillReferences& references) {
    std::vector<LineView> views;
    views.reserve(2);
    if (line && !settings.fill_separated) {
        views.push_back({line, {1.0, 0.0, 0.0, 0.0, 1.0, 0.0}, false});
    }
    if (auto rejected = append_light_references(settings, seed, color, references, views))
        return *rejected;
    return fill_impl(target, seed, color, settings, views, 1, false,
                     selection, stop);
}

FillResult fill_document(DocumentData& document,
                         Point seed,
                         Pixel color,
                         const FillSettings& settings,
                         const std::optional<RasterSelection>& selection,
                         std::stop_token stop,
                         const FillReferences& references) {
    FillResult result;
    if (document.current_layer < 0 ||
        document.current_layer >= static_cast<int>(document.layers.size())) {
        result.handled = false;
        result.error = "There is no active layer";
        return result;
    }
    auto& layer = document.layers[document.current_layer];
    if (layer.opaque || layer.current_plane < 0 ||
        layer.current_plane >= static_cast<int>(layer.planes.size())) {
        result.handled = false;
        result.error = "There is no editable active plane";
        return result;
    }
    auto& target_plane = layer.planes[layer.current_plane];
    if(!target_plane.opaque&&!target_plane.raster&&(target_plane.family==0x22100001||target_plane.family==0x22100002)){
        if(selection){result.handled=false;result.error="Vector Bucket requires the original RegionSelection entry";return result;}
        return fill_vector_document(document,seed,color,settings,{},stop,references);
    }
    if (target_plane.opaque || !target_plane.raster) {
        result.handled = false;
        result.error = "Vector and opaque planes require their dedicated fill worker";
        return result;
    }

    BucketFillSettings effective_settings = settings.active_bucket();
    // Live host sub_5F1490 disables leak-stop whenever a selection is supplied.
    // The lower fill_raster entry still consumes its explicit worker settings.
    if(selection)effective_settings.stop_if_leaked=0;
    if (!lower_bucket_gap_distance(layer, target_plane,
                                   settings.use_mm_for_tools != 0,
                                   effective_settings, result.error)) {
        result.handled = false;
        return result;
    }

    std::vector<LineView> lines;
    if (!effective_settings.fill_separated && paint_plane_family(target_plane.family)) {
        lines.reserve(layer.planes.size());
        for (std::size_t index = 0; index < layer.planes.size(); ++index) {
            if (static_cast<int>(index) == layer.current_plane) continue;
            const auto& plane = layer.planes[index];
            if (plane.opaque || !plane.raster || !line_family(plane.family)) continue;
            // Raster siblings share their owner's local origin. Common plane
            // positions are retained metadata, not offsets into pixel storage.
            lines.push_back({&*plane.raster,
                             {1.0, 0.0, 0.0, 0.0, 1.0, 0.0},
                             plane.common.opacity_mode == 5});
        }
    }
    Pixel effective_color = color;
    if (auto rejected = append_light_references(effective_settings, seed,
                                                 effective_color, references, lines))
        return *rejected;
    const bool black_guard = layer.family == 0x11001000 &&
                             target_plane.family == 0x12100001;
    result = fill_impl(*target_plane.raster, seed, effective_color,
                       effective_settings, lines,
                       target_plane.common.opacity_mode, black_guard,
                       selection, stop);
    if (result.changed||result.storage_changed) target_plane.pixels_changed = true;
    return result;
}

ConsecutiveBucketResult fill_consecutive_document(DocumentData& document,
    Point seed,Pixel color,const FillSettings& settings,std::stop_token stop,const FillReferences& references) {
    ConsecutiveBucketResult output;
    auto& result=output.pixels;
    try {
        if(stop.stop_requested()){result.cancelled=true;return output;}
        if(!std::isfinite(seed.x)||!std::isfinite(seed.y)||seed.x!=std::trunc(seed.x)||seed.y!=std::trunc(seed.y)||
           seed.x<std::numeric_limits<int>::min()||seed.x>std::numeric_limits<int>::max()||seed.y<std::numeric_limits<int>::min()||seed.y>std::numeric_limits<int>::max())
            throw std::invalid_argument("BFIL lower worker requires normalized integral seeds");
        if(document.current_layer<0||std::size_t(document.current_layer)>=document.layers.size())
            throw std::invalid_argument("BFIL has no bound owner layer");
        auto& layer=document.layers[std::size_t(document.current_layer)];
        if(layer.opaque||(layer.family&0x60000000)||layer.current_plane<0||std::size_t(layer.current_plane)>=layer.planes.size())
            throw std::invalid_argument("BFIL requires a raster owner and bound plane");
        auto& target=layer.planes[std::size_t(layer.current_plane)];
        if(target.opaque||(target.family&0xf0000000)!=0x10000000||!target.raster||target.common.opacity_mode!=1||settings.current_set<0||settings.current_set>1)
            throw std::invalid_argument("BFIL requires a normal raster target and valid native bank");
        std::uint64_t source_bytes{};
        const auto validate=[&](const Raster& raster,double dpi,bool read_only=false){
            const bool readable=raster.can_edit()||(read_only&&raster.depth==1&&raster.color_order==0);
            if(!readable||raster.width<=0||raster.height<=0||raster.width>100000||raster.height>100000||!std::isfinite(dpi)||dpi<=0||raster.resolution!=dpi)
                throw std::invalid_argument("BFIL requires bounded same-DPI32/64 rasters");
            for(const auto& [coordinate,tile]:raster.tiles){
                if(coordinate.x<0||coordinate.y<0||coordinate.x>(raster.width-1)/256||coordinate.y>(raster.height-1)/256||!tile||tile->size()!=raster.tile_bytes())
                    throw std::invalid_argument("BFIL malformed raster tile storage");
                if(tile->size()>512ull*1024*1024-source_bytes)throw std::length_error("BFIL source storage exceeds512MiB");
                source_bytes+=tile->size();
            }
        };
        validate(*target.raster,layer.common.resolution);
        if(target.common.resolution!=layer.common.resolution||target.common.width!=target.raster->width||target.common.height!=target.raster->height||target.common.depth!=target.raster->depth)
            throw std::invalid_argument("BFIL target metadata does not match its physical raster");
        const auto bounds=consecutive_work_bounds(document,layer,*target.raster,seed);
        if(!bounds.contains(int(seed.x),int(seed.y)))return output;
        auto effective=settings.active_bucket();effective.stop_if_leaked=0;effective.fill_separated=0;effective.use_light_color=0;
        if(effective.include.mode!=IncludeFillMode::None){const auto fixed=fixed_include_colors();
            for(std::size_t i=0;i<fixed.size();++i)if(effective.include.enabled[i]&&effective.include.primary_colors[i]!=fixed[i])
                throw std::invalid_argument("BFIL fixed include slots require native BGR color literals");}
        if(!lower_bucket_gap_distance(layer,target,settings.use_mm_for_tools!=0,effective,result.error)){result.handled=false;return output;}
        if(effective.color_margin<0||effective.color_margin>255||
           (effective.close_gap_on&&(effective.close_gap_distance_px<0||effective.close_gap_distance_px>200)))
            throw std::invalid_argument("BFIL margin or gap exceeds bounded range");
        std::vector<LineView> lines;
        // Native5F48D0 chooses one exact first sibling, then tests its eye.
        // An invisible first match does not fall through to a later sibling.
        const std::uint32_t companion_family=(target.family==0x12100001||target.family==0x12400001)
            ?(layer.family==0x11000800?0x11110002:layer.family==0x10040000?0x11410001:0):0;
        if(companion_family)for(const auto& plane:layer.planes){
            if(plane.family!=companion_family)continue;if(!plane.common.flags[1])break;
            if(plane.opaque||!plane.raster)throw std::invalid_argument("BFIL line companion requires its dedicated adapter");
            validate(*plane.raster,layer.common.resolution,true);
            if(plane.common.resolution!=layer.common.resolution||plane.raster->width!=target.raster->width||plane.raster->height!=target.raster->height)
                throw std::invalid_argument("BFIL companion requires same-DPI physical size");
            // Native5FAB20 sets w+AC from TARGET mode5, then calls the chosen
            // companion's IsTransparentFast(w+AC) before IncludeFill. This
            // bounded target profile is mode1, so only alpha0 is transparent.
            if(plane.raster->pixel(int(seed.x),int(seed.y)).a!=0)return output;
            // Materializer5F4EB0 always calls companion transparency(1)
            // as a HARD predicate before all IncludeFill list processing.
            lines.push_back({&*plane.raster,{1,0,0,0,1,0},true,true});
            break;
        }
        if(target.raster->pixel(int(seed.x),int(seed.y)).a!=0)effective.use_light_line=0;
        if(effective.use_light_line&&references.light_line){
            if(!valid_reference(*references.light_line))throw std::invalid_argument("BFIL malformed prepared LightLine reference");
            validate(*references.light_line->raster,layer.common.resolution,true);
        }
        if(effective.use_light_line&&references.light_line){
            const auto& ref=*references.light_line;const auto& t=ref.target_to_reference;
            const double x=t[0]*seed.x+t[1]*seed.y+t[2],y=t[3]*seed.x+t[4]*seed.y+t[5];
            const bool inside=std::isfinite(x)&&std::isfinite(y)&&x>=0&&y>=0&&x<ref.raster->width&&y<ref.raster->height;
            auto p=inside?ref.raster->pixel(int(std::floor(x)),int(std::floor(y))):Pixel{};
            if(ref.raster->depth==1&&p.a==0)p=Pixel{0,0,0,0};
            const auto raw=(std::uint32_t(p.r>>8))|(std::uint32_t(p.g>>8)<<8)|(std::uint32_t(p.b>>8)<<16)|(std::uint32_t(p.a>>8)<<24);
            lines.push_back({ref.raster,ref.target_to_reference,false,true,raw});
        }
        const auto pixels=std::uint64_t(target.raster->width)*target.raster->height;
        const auto gap=effective.close_gap_on?std::uint64_t(effective.close_gap_distance_px):0;
        const auto units=128+16*lines.size()+(gap?64*pixels+16*gap*gap+16*gap:0);
        if(units>128ull*1024*1024||pixels>(128ull*1024*1024)/units||pixels>(512ull*1024*1024)/128)
            throw std::length_error("BFIL solver work/scratch exceeds modern cap");
        const auto blocks=((std::uint64_t(target.raster->width)+255)/256)*((std::uint64_t(target.raster->height)+255)/256);
        if(blocks*(target.raster->tile_bytes()/4)>128ull*1024*1024-pixels*units)
            throw std::length_error("BFIL physical copy/compare work exceeds128Mi units");
        if(blocks*target.raster->tile_bytes()>512ull*1024*1024-source_bytes)
            throw std::length_error("BFIL retained-plus-COW storage exceeds512MiB");
        const bool black_guard=layer.family==0x11001000&&target.family==0x12100001;
        const Raster before=*target.raster;
        result=fill_impl(*target.raster,seed,color,effective,lines,1,black_guard,{},stop,&output.scan,bounds);
        output.performed=output.scan.width>0&&output.scan.height>0&&!result.cancelled&&result.handled;
        if(output.performed){
            output.storage_changed=before.tiles.size()!=target.raster->tiles.size();
            if(!output.storage_changed){auto old=before.tiles.begin();auto now=target.raster->tiles.begin();
                for(;old!=before.tiles.end();++old,++now)if(old->first!=now->first||(old->second!=now->second&&*old->second!=*now->second)){output.storage_changed=true;break;}}
        }
        if(output.performed)target.pixels_changed=true;
    }catch(const std::exception& error){output={};output.pixels.handled=false;output.pixels.error=error.what();}
    return output;
}

FillResult close_fill_raster(Raster& target,
                             const BinaryMask& request,
                             Rect request_bounds,
                             Pixel color,
                             const CloseFillSettings& settings,
                             int target_mode,
                             const Raster* companion,
                             bool structural_black_guard,
                             const std::optional<RasterSelection>& selection,
                             std::stop_token stop) {
    std::vector<LineView> lines;
    if (companion) {
        lines.push_back({companion,
                         {1.0, 0.0, 0.0, 0.0, 1.0, 0.0}, true});
    }
    return close_fill_impl(target, request, request_bounds, color, settings,
                           target_mode, lines, structural_black_guard,
                           selection, stop);
}

FillResult close_fill_document(DocumentData& document,
                               const BinaryMask& request,
                               Rect request_bounds,
                               Pixel color,
                               const CloseFillSettings& settings,
                               const std::optional<RasterSelection>& selection,
                               std::stop_token stop) {
    FillResult result;
    if (document.current_layer < 0 ||
        document.current_layer >= static_cast<int>(document.layers.size())) {
        result.handled = false;
        result.error = "There is no active layer";
        return result;
    }
    auto& layer = document.layers[document.current_layer];
    if (layer.opaque || layer.current_plane < 0 ||
        layer.current_plane >= static_cast<int>(layer.planes.size())) {
        result.handled = false;
        result.error = "There is no editable active plane";
        return result;
    }
    auto& target_plane = layer.planes[layer.current_plane];
    if (target_plane.opaque || !target_plane.raster) {
        result.handled = false;
        result.error = "Vector Close Fill requires its dedicated worker";
        if(!target_plane.opaque&&!target_plane.raster&&(target_plane.family==0x22100001||target_plane.family==0x22100002))
            return close_fill_vector_document(document,request,request_bounds,color,settings,selection,stop);
        return result;
    }

    std::vector<LineView> lines;
    const bool companion_target = target_plane.family == 0x12100001 ||
                                  target_plane.family == 0x12400001;
    const bool guard = layer.family == 0x11001000 &&
                       target_plane.family == 0x12100001;
    if (companion_target &&
        (layer.family == 0x11000800 || layer.family == 0x10040000)) {
        for (std::size_t index = 0; index < layer.planes.size(); ++index) {
            if (static_cast<int>(index) == layer.current_plane) continue;
            const auto& plane = layer.planes[index];
            if (plane.opaque || !plane.raster || plane.family != 0x11110002) continue;
            lines.push_back({&*plane.raster,
                             {1.0, 0.0, 0.0, 0.0, 1.0, 0.0},
                             true});
            break;
        }
    }
    result = close_fill_impl(*target_plane.raster, request, request_bounds,
                             color, settings, target_plane.common.opacity_mode,
                             lines, guard, selection, stop);
    if (result.changed||result.storage_changed) target_plane.pixels_changed = true;
    return result;
}

FillResult spread_fill_raster(Raster& target,
                              const BinaryMask& scratch,
                              Rect work_bounds,
                              Pixel sampled_color,
                              const SpreadFillSettings& settings,
                              int target_mode,
                              const Raster* companion,
                              const std::optional<RasterSelection>& selection,
                              std::stop_token stop) {
    std::vector<LineView> companions;
    if (companion) {
        companions.push_back({companion,
                              {1.0, 0.0, 0.0, 0.0, 1.0, 0.0}, true});
    }
    return spread_fill_impl(target, scratch, work_bounds, sampled_color,
                            settings, target_mode, companions, selection, stop);
}

FillResult spread_fill_document(DocumentData& document,
                                const BinaryMask& scratch,
                                Rect work_bounds,
                                Pixel sampled_color,
                                const SpreadFillSettings& settings,
                                const std::optional<RasterSelection>& selection,
                                std::stop_token stop) {
    FillResult result;
    if (document.current_layer < 0 ||
        document.current_layer >= static_cast<int>(document.layers.size())) {
        result.handled = false;
        result.error = "There is no active layer";
        return result;
    }
    auto& layer = document.layers[document.current_layer];
    if (layer.opaque || layer.current_plane < 0 ||
        layer.current_plane >= static_cast<int>(layer.planes.size())) {
        result.handled = false;
        result.error = "There is no editable active plane";
        return result;
    }
    auto& target_plane = layer.planes[layer.current_plane];
    if (target_plane.opaque || !target_plane.raster) {
        result.handled = false;
        result.error = "Vector Spread Fill requires its dedicated worker";
        return result;
    }
    if ((layer.family & 0xf0000000u) == 0x40000000u) {
        result.handled = false;
        result.error = "Spread Fill is unavailable on this layer family";
        return result;
    }
    const auto target_family = target_plane.family & 0xf0000000u;
    if (target_family == 0x20000000u) {
        result.handled = false;
        result.error = (target_plane.family & 0x0f000000u) == 0x02000000u
            ? "Vector Spread Fill requires its dedicated worker"
            : "Spread Fill is unavailable on this vector plane family";
        return result;
    }
    if (target_family != 0x10000000u) {
        result.handled = false;
        result.error = "Spread Fill is unavailable on this plane family";
        return result;
    }

    std::vector<LineView> companions;
    if (layer.family == 0x11000800) {
        for (std::size_t index = 0; index < layer.planes.size(); ++index) {
            if (static_cast<int>(index) == layer.current_plane) continue;
            const auto& plane = layer.planes[index];
            if (plane.opaque || !plane.raster || plane.family != 0x11110002) continue;
            companions.push_back({&*plane.raster,
                                  {1.0, 0.0, 0.0, 0.0, 1.0, 0.0},
                                  true});
            break;
        }
    }
    result = spread_fill_impl(*target_plane.raster, scratch, work_bounds,
                              sampled_color, settings,
                              target_plane.common.opacity_mode,
                              companions, selection, stop);
    if (result.changed||result.storage_changed) target_plane.pixels_changed = true;
    return result;
}

SelectionWandResult selection_wand_detail::solve_surface(const Raster& source,const Raster* companion,
    Rect work,Point seed,int target_mode,int gap_px,int color_margin,std::stop_token stop) {
    SelectionWandResult result;
    try {
        if(stop.stop_requested())throw Cancelled{};
        if(!raster_stroke_surface_supported(source)||source.width<=0||source.height<=0||gap_px<0||gap_px>200||color_margin<0||color_margin>254)
            throw std::invalid_argument("Unsupported Wand surface or parameters");
        const auto right=std::int64_t(work.x)+work.width,bottom=std::int64_t(work.y)+work.height;
        if(work.width<=0||work.height<=0)return result;
        if(work.x<0||work.y<0||right>source.width||bottom>source.height)
            throw std::invalid_argument("Wand source work bounds exceed its raster");
        if(std::uint64_t(source.width)*source.height>128ull*1024*1024)
            throw std::invalid_argument("Wand surface exceeds supported resource limits");
        if(!std::isfinite(seed.x)||!std::isfinite(seed.y)||seed.x<work.x||seed.y<work.y||seed.x>=right||seed.y>=bottom)return result;
        const int sx=int(seed.x),sy=int(seed.y);
        // sub_5FB270 checks the optional reference's Transmit predicate before
        // sampling the primary. It installs neither Bucket lists nor black guard.
        if(companion&&!selection_detail::transparent(*companion,sx,sy,target_mode==5))return result;
        const auto sample=raster_stroke_surface_pixel(source,sx,sy);
        std::size_t visits{};
        const auto condition=[&](int x,int y){
            if(((++visits)&2047u)==0&&stop.stop_requested())throw Cancelled{};
            const int px=x+work.x,py=y+work.y;
            if(companion&&!selection_detail::transparent(*companion,px,py,true))return false;
            const auto current=raster_stroke_surface_pixel(source,px,py);
            //5F4970 deliberately has no equality consumer for2/16/24.
            if(source.depth==2||source.depth==16||source.depth==24)return false;
            if(source.depth==1)return current.a==sample.a;
            if(source.depth==8)return std::abs(int(current.a>>8)-int(sample.a>>8))<=color_margin*257;
            return target_mode==4?std::abs(int(current.a)-int(sample.a))<=color_margin*257
                :equal_with_margin(current,sample,color_margin,source.depth,target_mode==5);
        };
        BitMask selected(work.width,work.height);Bounds reported{};bool solved{};
        if(gap_px){
            BitMask candidates(work.width,work.height);
            for(int y=0;y<work.height;++y)for(int x=0;x<work.width;++x)candidates.set(x,y,condition(x,y));
            bool leaked{};solved=close_gap_closure(selected,candidates,sx-work.x,sy-work.y,gap_px,false,leaked,stop,&reported);
        }else{
            // No candidate-true seed admission: the native primitive follows
            // the seed's actual Boolean class. This differs from Bucket setup.
            const auto flooded=mask_flood(selected,sx-work.x,sy-work.y,condition,false,stop);
            solved=flooded.ok;reported=flooded.dirty;
        }
        if(!solved)return result;
        auto output=std::make_shared<BinaryMask>(source.width,source.height);
        const Rect bounds{work.x+reported.left,work.y+reported.top,reported.width(),reported.height()};
        if(bounds.width>0&&bounds.height>0){
            for(int ty=bounds.y/256;ty<=(bounds.y+bounds.height-1)/256;++ty)
                for(int tx=bounds.x/256;tx<=(bounds.x+bounds.width-1)/256;++tx)output->materialize_block(tx,ty);
            for(int y=reported.top;y<reported.bottom;++y)for(int x=reported.left;x<reported.right;++x){
                if(((++visits)&2047u)==0&&stop.stop_requested())throw Cancelled{};
                if(selected.test(x,y))output->set(x+work.x,y+work.y);
            }
        }
        if(stop.stop_requested())throw Cancelled{};
        result.solved=true;result.bounds=bounds;result.mask=std::move(output);
    }catch(const Cancelled&){result.cancelled=true;}
    catch(const std::exception& e){result.handled=false;result.error=e.what();}
    return result;
}

} // namespace retas
