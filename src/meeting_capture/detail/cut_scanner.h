#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// spool.py _CutScanner: an incremental quiet-window scanner over a growing pending buffer. Audio
// is folded into fixed 50 ms frames as it arrives and only each frame's energy is kept, so a cut
// decision costs O(new audio) instead of a rescan of the whole buffer on every block.
namespace fv::meeting::detail {

// Affinity: one thread at a time (the spool's writer thread).
class CutScanner {
public:
    CutScanner(int sample_rate, double target_sec, double max_sec, double quiet_rms,
               double quiet_window_s);

    std::int64_t total_samples() const { return total_; }
    // Folds newly buffered samples into the frame-energy prefix sums.
    void push(std::span<const std::int16_t> frames);
    // The sample index to cut at (the end of the first quiet window after the target, else the
    // maximum), or nullopt when no cut is due yet.
    std::optional<std::int64_t> cut_index();

private:
    std::int64_t frame_len_;
    std::int64_t win_frames_;
    std::int64_t target_n_;
    std::int64_t max_n_;
    double threshold_;
    // prefix_[i] is the energy of frames [0, i). Sums of squared int16 values stay below 2^53,
    // so they are exact whatever the order of addition.
    std::vector<double> prefix_{0.0};
    std::vector<std::int16_t> carry_;
    std::int64_t total_ = 0;
    std::int64_t next_frame_;
};

} // namespace fv::meeting::detail
