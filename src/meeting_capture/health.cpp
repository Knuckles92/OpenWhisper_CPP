#include "meeting_capture/health.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

#include "pyport/number.h"

namespace fv::meeting {

Json CaptureHealth::to_json() const {
    Json out = Json::object();
    out["receiving"] = receiving;
    out["stalled"] = stalled;
    out["signal_windows"] = signal_windows;
    out["generation"] = generation;
    return out;
}

CaptureSignalMonitor::CaptureSignalMonitor(const Clock& clock, double signal_rms)
    : clock_(clock), signal_rms_(signal_rms),
      last_block_mono_(std::numeric_limits<double>::quiet_NaN()) {
    reset_stream();
}

double CaptureSignalMonitor::now_or_clock(std::optional<double> now) const {
    if (now) {
        return *now;
    }
    return std::chrono::duration<double>(clock_.steady().time_since_epoch()).count();
}

void CaptureSignalMonitor::reset_stream(std::optional<double> now) {
    started_mono_.store(now_or_clock(now));
    last_block_mono_.store(std::numeric_limits<double>::quiet_NaN());
    generation_.fetch_add(1);
}

void CaptureSignalMonitor::observe(const CaptureBlock& block, bool signal_enabled,
                                   std::optional<double> now) noexcept {
    const double receipt = now_or_clock(now);
    if (block.count == 0) {
        return;
    }
    const std::int64_t generation = generation_.load();
    if (generation != seen_generation_) {
        seen_generation_ = generation;
        sum_squares_ = 0.0;
        samples_ = 0;
        sample_rate_ = 0;
        consecutive_signal_windows_ = 0;
        last_reported_signal_mono_.reset();
    }
    const int rate = std::max(1, block.rate);
    // Squares of int16 values sum exactly in a double, so the order does not matter.
    double squares = 0.0;
    if (signal_enabled) {
        for (std::size_t i = 0; i < block.count; ++i) {
            const double value = block.frames[i];
            squares += value * value;
        }
    }
    last_block_mono_.store(receipt);
    if (!signal_enabled) {
        sum_squares_ = 0.0;
        samples_ = 0;
        consecutive_signal_windows_ = 0;
        return;
    }
    if (sample_rate_ != rate) {
        sum_squares_ = 0.0;
        samples_ = 0;
        consecutive_signal_windows_ = 0;
        sample_rate_ = rate;
    }
    sum_squares_ += squares;
    samples_ += static_cast<std::int64_t>(block.count);
    const auto window = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(py::round_half_even(rate * SIGNAL_WINDOW_S)));
    if (samples_ < window) {
        return;
    }
    const double rms = std::sqrt(sum_squares_ / static_cast<double>(samples_));
    if (rms >= signal_rms_) {
        ++consecutive_signal_windows_;
        if (consecutive_signal_windows_ >= MIN_CONSECUTIVE_SIGNAL_WINDOWS &&
            (!last_reported_signal_mono_ ||
             receipt - *last_reported_signal_mono_ >= SIGNAL_REPORT_INTERVAL_S)) {
            // The dashboard needs only a fresh pulse: capping it keeps persistence and status
            // broadcasts quiet during speech.
            signal_windows_.fetch_add(1);
            last_reported_signal_mono_ = receipt;
        }
    } else {
        consecutive_signal_windows_ = 0;
    }
    sum_squares_ = 0.0;
    samples_ = 0;
}

CaptureHealth CaptureSignalMonitor::snapshot(std::optional<double> now) const {
    const double instant = now_or_clock(now);
    const double last = last_block_mono_.load();
    const double started = started_mono_.load();
    const std::int64_t signals = signal_windows_.load();
    const std::int64_t generation = generation_.load();
    const bool has_block = !std::isnan(last);
    const double age = instant - (has_block ? last : started);
    return CaptureHealth{
        .receiving = has_block && age < BLOCK_STALL_S,
        .stalled = age >= (has_block ? BLOCK_STALL_S : FIRST_BLOCK_GRACE_S),
        .signal_windows = signals,
        .generation = generation,
    };
}

} // namespace fv::meeting
