#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

// meeting/interfaces.py CaptureBlock: one buffer of mono int16 audio from a capture source.
namespace fv::meeting {

// The view is valid only during the call that receives it: the device thread reuses the buffer.
// Affinity: value; read on the thread that delivers it.
struct CaptureBlock {
    std::string_view channel;             // meeting_state::kChannelMic or kChannelLoopback
    const std::int16_t* frames = nullptr; // mono samples
    std::size_t count = 0;                // samples in frames
    int rate = 0;                         // Hz, the device's native rate
    double t_mono = 0.0;                  // monotonic seconds when the first sample was captured

    std::span<const std::int16_t> samples() const { return {frames, count}; }
};

} // namespace fv::meeting
