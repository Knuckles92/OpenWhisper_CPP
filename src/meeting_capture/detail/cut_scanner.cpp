#include "meeting_capture/detail/cut_scanner.h"

#include <algorithm>

#include "pyport/number.h"

namespace fv::meeting::detail {

namespace {

// Hop between candidate quiet windows (spool._CUT_SCAN_HOP_S).
constexpr double kCutScanHopS = 0.05;

// Python's int(x) of a float: truncation toward zero.
std::int64_t py_trunc(double value) {
    return static_cast<std::int64_t>(value);
}

// Python's floor division for a positive divisor.
std::int64_t floor_div(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b != 0) && (a < 0)) {
        --q;
    }
    return q;
}

} // namespace

CutScanner::CutScanner(int sample_rate, double target_sec, double max_sec, double quiet_rms,
                       double quiet_window_s)
    : frame_len_(std::max<std::int64_t>(
          1, static_cast<std::int64_t>(py::round_half_even(kCutScanHopS * sample_rate)))),
      win_frames_(std::max<std::int64_t>(
          1, static_cast<std::int64_t>(py::round_half_even(quiet_window_s / kCutScanHopS)))),
      target_n_(py_trunc(target_sec * sample_rate)), max_n_(py_trunc(max_sec * sample_rate)),
      threshold_(quiet_rms * quiet_rms),
      // ceil(target_n / frame_len) as -(-target_n // frame_len).
      next_frame_(-floor_div(-target_n_, frame_len_)) {}

void CutScanner::push(std::span<const std::int16_t> frames) {
    if (frames.empty()) {
        return;
    }
    total_ += static_cast<std::int64_t>(frames.size());
    std::vector<std::int16_t> data;
    std::span<const std::int16_t> view = frames;
    if (!carry_.empty()) {
        data.reserve(carry_.size() + frames.size());
        data.insert(data.end(), carry_.begin(), carry_.end());
        data.insert(data.end(), frames.begin(), frames.end());
        view = data;
    }
    const auto length = static_cast<std::size_t>(frame_len_);
    const std::size_t full = view.size() / length;
    double running = prefix_.back();
    for (std::size_t f = 0; f < full; ++f) {
        double energy = 0.0;
        const std::int16_t* at = view.data() + f * length;
        for (std::size_t i = 0; i < length; ++i) {
            const double value = at[i];
            energy += value * value;
        }
        running += energy;
        prefix_.push_back(running);
    }
    carry_.assign(view.begin() + static_cast<std::ptrdiff_t>(full * length), view.end());
}

std::optional<std::int64_t> CutScanner::cut_index() {
    if (total_ < target_n_) {
        return std::nullopt;
    }
    const std::int64_t win = win_frames_;
    const auto n_frames = static_cast<std::int64_t>(prefix_.size()) - 1;
    const std::int64_t limit = std::min(total_, max_n_);
    const std::int64_t last_end_frame = std::min(n_frames, floor_div(limit, frame_len_));
    const double denom = static_cast<double>(win * frame_len_);
    std::int64_t frame = next_frame_;
    while (frame + win <= last_end_frame) {
        const double mean_sq = (prefix_[static_cast<std::size_t>(frame + win)] -
                                prefix_[static_cast<std::size_t>(frame)]) /
                               denom;
        if (mean_sq < threshold_) {
            return (frame + win) * frame_len_;
        }
        ++frame;
    }
    next_frame_ = frame;
    if (total_ >= max_n_) {
        return max_n_;
    }
    return std::nullopt;
}

} // namespace fv::meeting::detail
