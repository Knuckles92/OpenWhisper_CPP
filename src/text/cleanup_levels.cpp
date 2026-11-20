#include "text/cleanup_levels.h"

namespace fv::text::cleanup_levels {

namespace {

std::string_view lookup(const std::array<std::pair<std::string_view, std::string_view>, 4>& table,
                        std::string_view value) {
    for (const auto& [key, text] : table) {
        if (key == value) {
            return text;
        }
    }
    return {};
}

} // namespace

std::string_view note(std::string_view value) {
    return lookup(NOTES, value);
}

std::string_view short_note(std::string_view value) {
    return lookup(SHORT_NOTES, value);
}

std::size_t segment_index(std::string_view value) {
    std::size_t medium = 0;
    for (std::size_t i = 0; i < SEGMENTS.size(); ++i) {
        if (SEGMENTS[i].value == value) {
            return i;
        }
        if (SEGMENTS[i].value == "medium") {
            medium = i;
        }
    }
    return medium;
}

} // namespace fv::text::cleanup_levels
