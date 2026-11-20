#include "recorder/post_roll.h"

#include <algorithm>
#include <cmath>

#include "pyport/number.h"

namespace fv {

PostRollGate::PostRollGate(int rate, PostRollConfig config) : rate_(rate), config_(config) {}

bool PostRollGate::observe(double level_db, std::int64_t frames) {
    if (!armed_) {
        // int(round(level_db)): Python rounds half to even.
        const auto rounded = static_cast<std::int64_t>(py::round_half_even(level_db));
        const std::int64_t index =
            std::clamp<std::int64_t>(rounded - dsp::LEVEL_HISTOGRAM_MIN_DB, 0,
                                     static_cast<std::int64_t>(histogram_.size()) - 1);
        histogram_[static_cast<std::size_t>(index)] += 1;
        frames_before_stop_ += frames;
        return false;
    }
    frames_after_stop_ += frames;
    if (!threshold_db_ || ended_) {
        return false;
    }
    if (level_db < *threshold_db_) {
        quiet_frames_ += frames;
    } else {
        quiet_frames_ = 0;
    }
    if (quiet_frames_ >= quiet_frames_needed_) {
        ended_ = true;
        return true;
    }
    return false;
}

void PostRollGate::arm() {
    if (armed_) {
        return;
    }
    armed_ = true;
    if (!config_.adaptive) {
        fallback_ = "adaptive post-roll disabled";
        return;
    }
    const double captured_ms = static_cast<double>(frames_before_stop_) * 1000.0 / rate_;
    if (captured_ms < config_.floor_min_ms) {
        fallback_ = "only " + py::format_fixed(captured_ms, 0) +
                    " ms before stop, too short for a noise floor";
        return;
    }
    floor_db_ = percentile(NOISE_FLOOR_PERCENTILE);
    speech_db_ = percentile(SPEECH_LEVEL_PERCENTILE);
    const double snr_db = *speech_db_ - *floor_db_;
    if (snr_db < config_.min_snr_db) {
        fallback_ = "speech only " + py::format_fixed(snr_db, 0) + " dB above the noise floor";
        return;
    }
    threshold_db_ =
        std::max(*floor_db_ + config_.quiet_margin_db, *speech_db_ - config_.speech_headroom_db);
    quiet_frames_needed_ = std::max<std::int64_t>(
        1, static_cast<std::int64_t>(py::round_half_even(rate_ * config_.quiet_ms / 1000.0)));
}

std::string PostRollGate::describe() const {
    if (!threshold_db_) {
        return fallback_.empty() ? "not armed" : fallback_;
    }
    return "floor " + py::format_fixed(*floor_db_, 0) + " dBFS, speech " +
           py::format_fixed(*speech_db_, 0) + " dBFS, quiet below " +
           py::format_fixed(*threshold_db_, 0) + " dBFS";
}

double PostRollGate::percentile(double percent) const {
    // np.searchsorted(np.cumsum(histogram), total * percent / 100, side="left").
    std::int64_t total = 0;
    for (const std::int64_t count : histogram_) {
        total += count;
    }
    const double rank = static_cast<double>(total) * percent / 100.0;
    std::size_t index = histogram_.size();
    std::int64_t running = 0;
    for (std::size_t i = 0; i < histogram_.size(); ++i) {
        running += histogram_[i];
        if (static_cast<double>(running) >= rank) {
            index = i;
            break;
        }
    }
    index = std::min(index, histogram_.size() - 1);
    return static_cast<double>(static_cast<int>(index) + dsp::LEVEL_HISTOGRAM_MIN_DB);
}

} // namespace fv
