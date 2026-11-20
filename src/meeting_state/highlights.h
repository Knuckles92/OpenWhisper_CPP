#pragma once

#include <array>
#include <optional>
#include <string_view>
#include <utility>

// meeting/highlights.py: labels of the detected meeting moments (pulses), by kind, in Python's
// dict order.
namespace fv::meeting_state {

inline constexpr std::array<std::pair<std::string_view, std::string_view>, 5> PULSE_LABELS = {{
    {"decision", "Decision"},
    {"disagreement", "Disagreement"},
    {"commitment", "Dated commitment"},
    {"number", "Number"},
    {"takeaway", "Takeaways"},
}};

// PULSE_LABELS.get(kind).
inline constexpr std::optional<std::string_view> pulse_label(std::string_view kind) {
    for (const auto& [name, label] : PULSE_LABELS) {
        if (name == kind) {
            return label;
        }
    }
    return std::nullopt;
}

} // namespace fv::meeting_state
