#pragma once
#include <cstdint>

namespace retas {
struct Point { double x{},y{}; };
struct Rect {
    int x{},y{},width{},height{};
    bool contains(int px,int py) const {
        return px>=x&&py>=y&&std::int64_t(px)<std::int64_t(x)+width&&std::int64_t(py)<std::int64_t(y)+height;
    }
    bool operator==(const Rect&) const = default;
};
}
