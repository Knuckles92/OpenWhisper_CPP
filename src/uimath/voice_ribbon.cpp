#include "uimath/voice_ribbon.h"

#include <algorithm>
#include <array>
#include <cmath>

#include "uimath/loudness.h"
#include "uimath/overlay_look.h"

namespace fv {

void VoiceRibbon::reset() {
    loudness_ = 0.0;
    shown_ = 0.0;
    peak_ = 0.0;
    slot_peak_ = 0.0;
    history_.assign(kRibbonSamples, 0.0);
    carry_ = 0.0;
}

void VoiceRibbon::hear(double level) {
    loudness_ = level_to_height(level);
    peak_ = detail::py_max(peak_, loudness_);
}

void VoiceRibbon::advance(double dt) {
    const double target = detail::py_max(peak_, loudness_);
    peak_ = 0.0;
    if (target >= shown_) {
        shown_ = target;
    } else {
        shown_ += (target - shown_) * (1.0 - std::exp(-dt / kRibbonReleaseS));
    }
    slot_peak_ = detail::py_max(slot_peak_, shown_);
    carry_ += dt;
    const double step = kRibbonHistoryS / static_cast<double>(kRibbonSamples);
    while (carry_ >= step) {
        carry_ -= step;
        history_.push_back(slot_peak_);
        history_.pop_front();
        slot_peak_ = shown_;
    }
}

std::vector<double> VoiceRibbon::history() const {
    return {history_.begin(), history_.end()};
}

double VoiceRibbon::height_at(double x, double width) const {
    const double span = detail::py_max(1.0, width - 2 * kRibbonEdge);
    const double f = detail::py_min(1.0, detail::py_max(0.0, (x - kRibbonEdge) / span));
    const double pos = f * static_cast<double>(kRibbonSamples - 1);
    const auto i = static_cast<std::size_t>(pos);
    const std::size_t j = std::min(i + 1, kRibbonSamples - 1);
    return history_[i] + (history_[j] - history_[i]) * (pos - static_cast<double>(i));
}

double VoiceRibbon::crest(double x, double width, double baseline) const {
    return baseline - kRibbonRise * height_at(x, width);
}

std::optional<RibbonShape> VoiceRibbon::shape(double width, double baseline, double hue,
                                              double grow, bool dark) const {
    if (grow <= 0.0) {
        return std::nullopt;
    }
    RibbonShape shape;
    constexpr int kSegments = 80;
    shape.top.reserve(kSegments + 1);
    for (int i = 0; i <= kSegments; ++i) {
        const double x = kRibbonEdge + (width - 2 * kRibbonEdge) * i / kSegments;
        shape.top.push_back(PointD{x, baseline - (kRibbonRise * height_at(x, width) + 1.2) * grow});
    }
    shape.bottom_y = baseline + 1;
    shape.gradient_left = kRibbonEdge;
    shape.gradient_right = width - kRibbonEdge;
    // Faded at both ends; across, the hue drifts like the particles'.
    struct Stop {
        double position;
        int fill_alpha;
        int edge_alpha;
    };
    constexpr std::array<Stop, 4> kStops = {
        {{0.0, 0, 0}, {0.12, 40, 150}, {0.88, 60, 230}, {1.0, 0, 0}}};
    for (const Stop& stop : kStops) {
        const double stop_hue = hue + stop.position * 60;
        shape.fill.push_back(
            {stop.position, hue_color(stop_hue, std::trunc(stop.fill_alpha * grow), true, dark)});
        shape.edge.push_back(
            {stop.position, hue_color(stop_hue, std::trunc(stop.edge_alpha * grow), true, dark)});
    }
    return shape;
}

} // namespace fv
