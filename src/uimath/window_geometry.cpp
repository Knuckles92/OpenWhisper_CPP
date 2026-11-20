#include "uimath/window_geometry.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <limits>
#include <stdexcept>
#include <string>

namespace fv {

// ui_qt/main_window.py:1866-1893.
Rect clamp_geometry(Rect candidate, Rect available, Size min, Size max) {
    const int max_width = std::min(max.width, available.width);
    const int max_height = available.height;
    // max() over min(): a minimum the screen cannot hold still wins, as Python's order makes it.
    const int width = std::max(min.width, std::min(candidate.width, max_width));
    const int height = std::max(min.height, std::min(candidate.height, max_height));
    const int max_x = available.x + available.width - width;
    const int max_y = available.y + available.height - height;
    const int x = std::min(std::max(candidate.x, available.x), std::max(available.x, max_x));
    const int y = std::min(std::max(candidate.y, available.y), std::max(available.y, max_y));
    return Rect{x, y, width, height};
}

int geometry_int(const Json& geometry, std::string_view key) {
    const auto it = geometry.find(std::string(key));
    if (it == geometry.end()) {
        throw std::invalid_argument(std::format("The saved geometry has no {}.", key));
    }
    if (it->is_boolean()) {
        return it->get<bool>() ? 1 : 0;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<std::uint64_t>();
        if (value > static_cast<std::uint64_t>(std::numeric_limits<int>::max())) {
            throw std::invalid_argument(std::format("The saved {} does not fit a window.", key));
        }
        return static_cast<int>(value);
    }
    if (it->is_number_integer()) {
        const auto value = it->get<std::int64_t>();
        if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
            throw std::invalid_argument(std::format("The saved {} does not fit a window.", key));
        }
        return static_cast<int>(value);
    }
    throw std::invalid_argument(std::format("The saved {} is not a whole number.", key));
}

// ui_qt/main_window.py:2386-2406.
int restored_width(const Json& geometry, int min_width, int max_width, std::string_view format) {
    if (!geometry.is_object()) {
        throw std::invalid_argument("The saved geometry is not an object.");
    }
    // 64 bits: the legacy subtraction below must not overflow for a width near INT_MIN.
    std::int64_t raw_width = geometry_int(geometry, "width");
    bool migrated_expanded_width = false;
    constexpr int legacy_expanded_width = window_size::kDefaultWidth +
                                          window_size::kHistorySidebarWidth -
                                          window_size::kHistoryEdgeTabWidth;
    const auto saved_format = geometry.find("format");
    const bool current_format = saved_format != geometry.end() && saved_format->is_string() &&
                                saved_format->get<std::string>() == format;
    if (!current_format && raw_width >= legacy_expanded_width) {
        raw_width -= window_size::kHistorySidebarWidth;
        migrated_expanded_width = true;
    }
    const std::int64_t clamped =
        std::max<std::int64_t>(min_width, std::min<std::int64_t>(raw_width, max_width));
    int width = static_cast<int>(clamped);
    if (width <= window_size::kDefaultWidth || migrated_expanded_width) {
        width = window_size::kDefaultWidth;
    }
    return width;
}

} // namespace fv
