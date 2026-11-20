#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>

#include "dsp/level.h"

// Adaptive post-roll (services/recorder.py PostRollGate, config.py POST_ROLL_*): capture keeps
// running after the stop press so a last word the press beat is not clipped, and ends once the
// tail has gone quiet, or at the cap.
namespace fv {

// The histogram covers whole-dB bins from dsp::LEVEL_HISTOGRAM_MIN_DB to this, so a percentile
// of an hours-long recording costs constant memory and one pass over 121 bins.
inline constexpr int LEVEL_HISTOGRAM_MAX_DB = 0;
// A dictation's noise floor is the level its quietest tenth of blocks stay under; its speech
// level is the one its loudest tenth reach.
inline constexpr double NOISE_FLOOR_PERCENTILE = 10;
inline constexpr double SPEECH_LEVEL_PERCENTILE = 90;

// config.py's POST_ROLL_* switches that decide how a stop's threshold is set. Replaying the
// owner's 20 saved dictations cut the audio kept after the stop from 1231 ms to a median of
// 511 ms with no word lost; the reasoning behind each value is in config.py.
struct PostRollConfig {
    // False restores the fixed cap (POST_ROLL_ADAPTIVE).
    bool adaptive = true;
    // Quiet blocks adding up to this many milliseconds in a row end the post-roll.
    int quiet_ms = 300;
    // The threshold is the floor plus this many dB...
    double quiet_margin_db = 12.0;
    // ...but never lower than this many dB under the speech level (noise-suppressing mics gate
    // pauses to near digital silence, where floor + 12 dB counts every rustle as speech).
    double speech_headroom_db = 30.0;
    // Below this speech-to-floor gap the floor is not trusted and the post-roll runs to the cap.
    double min_snr_db = 20.0;
    // With less audio than this before the stop the floor is not trusted either.
    int floor_min_ms = 500;
};

// Every block captured before the stop goes into a level histogram. arm() (at the stop press)
// freezes a quiet threshold from it; after that, observe() returns true once blocks adding up to
// quiet_ms in a row stay below the threshold. A recording whose floor cannot be trusted never
// ends early: it runs to the cap.
// Affinity: not thread-safe; the recorder touches it only under its capture lock.
class PostRollGate {
public:
    explicit PostRollGate(int rate, PostRollConfig config = {});

    // Accounts one block; true exactly once, when the tail has gone quiet. `frames` per channel.
    bool observe(double level_db, std::int64_t frames);
    // Freezes the quiet threshold from what was captured before the stop; a second call does
    // nothing.
    void arm();
    // One log-friendly phrase for how this stop's threshold was set.
    std::string describe() const;

    int rate() const { return rate_; }
    const PostRollConfig& config() const { return config_; }
    std::int64_t frames_before_stop() const { return frames_before_stop_; }
    std::int64_t frames_after_stop() const { return frames_after_stop_; }
    bool armed() const { return armed_; }
    bool ended() const { return ended_; }
    std::optional<double> floor_db() const { return floor_db_; }
    std::optional<double> speech_db() const { return speech_db_; }
    std::optional<double> threshold_db() const { return threshold_db_; }
    // Why this stop cannot end early; empty while the gate is usable.
    const std::string& fallback() const { return fallback_; }

private:
    double percentile(double percent) const;

    int rate_;
    PostRollConfig config_;
    std::array<std::int64_t, LEVEL_HISTOGRAM_MAX_DB - dsp::LEVEL_HISTOGRAM_MIN_DB + 1> histogram_{};
    std::int64_t frames_before_stop_ = 0;
    std::int64_t frames_after_stop_ = 0;
    std::int64_t quiet_frames_ = 0;
    std::int64_t quiet_frames_needed_ = 0;
    bool armed_ = false;
    bool ended_ = false;
    std::optional<double> floor_db_;
    std::optional<double> speech_db_;
    std::optional<double> threshold_db_;
    std::string fallback_;
};

} // namespace fv
