#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "audio/backend.h"
#include "meeting_capture/capture_block.h"
#include "meeting_capture/meeting_clock.h"

// meeting/interfaces.py CaptureSource and its two Windows implementations: the microphone through
// PortAudio (meeting/capture/sd_stream.py SdCaptureSource) and system audio through WASAPI
// loopback of a render endpoint (the role of SdCaptureSource on a "[Loopback]" device and of
// soundcard_stream.py's SoundcardLoopbackSource). Both deliver mono int16 blocks at the device's
// native rate; resampling is the spool's job.
namespace fv::meeting {

// Frames per PortAudio callback.
inline constexpr int MIC_BLOCK_FRAMES = 1024;
// How long LoopbackSource::start waits for the first block (soundcard_stream._START_TIMEOUT_S):
// with the 1 s watchdog and 3 s retry spacing a restart still fits the 12 s restoration bound.
inline constexpr double LOOPBACK_START_TIMEOUT_S = 4.0;

// What the capture watchdog compares to follow a device: a PortAudio index, an endpoint id, or
// nothing.
using DeviceId = std::variant<std::monostate, int, std::string>;

// Receives each block on the device thread; must be fast, must not allocate, lock or throw.
using BlockSink = std::function<void(const CaptureBlock&)>;

// Affinity: start, stop and the queries on the capture's start, end and watchdog threads, one at
// a time; blocks arrive on the source's device thread.
class CaptureSource {
public:
    virtual ~CaptureSource() = default;
    virtual std::string_view channel() const = 0;
    // Opens the device and starts delivering blocks to `on_block`. Throws when the device cannot
    // be opened: the caller must treat that as "this channel is dead", never as a silent room.
    virtual void start(BlockSink on_block) = 0;
    // Stops delivery and releases the device; no block arrives after it returns.
    virtual void stop() = 0;
    virtual bool is_active() const = 0;
    virtual DeviceId device_id() const = 0;
    // True when is_active() only says the stream is open, so the health monitor must also see
    // blocks arrive before the source counts as live (PortAudio can keep a stream "active"
    // without ever calling back).
    virtual bool requires_audio_blocks() const { return false; }
    // Whether the source still records the device the system now uses by default; nullopt when it
    // cannot tell.
    virtual std::optional<bool> is_default_device_current() { return std::nullopt; }
};

// The monotonic time of a block's first frame (sd_stream.py:122-142). The device's two times are
// used together, only when both are finite, current > 0 and 0 <= adc <= current: Windows MME can
// report currentTime 0 yet add an ADC offset to alternate batched buffers, and mixing that offset
// with another clock backdates them by the system uptime, after which the spool drops them as
// overlap. Otherwise the delivery time stands.
double capture_timestamp(double delivered_mono, const audio::BlockStatus& status);

namespace detail {
class BlockShaper;
}

// A PortAudio input stream at the device's native rate and channels, downmixed to mono by the
// mean of each frame's channels (through int32, truncated toward zero).
// Affinity: see CaptureSource.
class MicSource final : public CaptureSource {
public:
    MicSource(std::shared_ptr<audio::Backend> backend, const MeetingClock& clock,
              std::string channel, int device_index, int samplerate, int channels);
    ~MicSource() override;

    std::string_view channel() const override { return channel_; }
    void start(BlockSink on_block) override;
    void stop() override;
    bool is_active() const override;
    DeviceId device_id() const override { return device_index_; }
    bool requires_audio_blocks() const override { return true; }

private:
    static void on_capture(void* user, const std::int16_t* frames, std::size_t count,
                           audio::BlockStatus status);
    void close_stream();
    void log_device_events() const;

    std::shared_ptr<audio::Backend> backend_;
    const MeetingClock& clock_;
    std::string channel_;
    int device_index_;
    int samplerate_;
    int channels_;
    BlockSink on_block_;
    std::unique_ptr<detail::BlockShaper> shaper_;
    std::unique_ptr<audio::Stream> stream_;
    std::atomic<bool> active_{false};
    // Set on the device thread, logged later by is_active(): no logging there.
    mutable std::atomic<int> status_flags_{0};
    mutable std::atomic<bool> status_logged_{false};
    mutable std::atomic<bool> lost_{false};
    mutable std::atomic<bool> lost_logged_{false};
};

// The per-channel weights that make a surround loopback frame mono (4, 6 or 8 channels in
// Windows' default order): the front pair averaged, the centre and the surrounds mixed in at
// -3 dB as a stereo downmix has them, the LFE dropped. Empty for any other count.
std::vector<float> loopback_mix_weights(int channels);

// WASAPI loopback of one render endpoint (audio::Backend::open_loopback) at the endpoint's mix
// rate, made mono: a quad, 5.1 or 7.1 endpoint through loopback_mix_weights, any other by the
// mean of its first two channels (SoundCard recorded at most two), through int32 and truncated.
// start() returns once the first block arrived or `first_block_wait_s` passed; a source that
// delivered nothing by then is inactive. Affinity: see CaptureSource.
class LoopbackSource final : public CaptureSource {
public:
    LoopbackSource(std::shared_ptr<audio::Backend> backend, const MeetingClock& clock,
                   audio::LoopbackEndpoint endpoint,
                   double first_block_wait_s = LOOPBACK_START_TIMEOUT_S);
    ~LoopbackSource() override;

    std::string_view channel() const override;
    void start(BlockSink on_block) override;
    void stop() override;
    bool is_active() const override;
    DeviceId device_id() const override { return endpoint_.id; }
    bool requires_audio_blocks() const override { return true; }
    // Whether the backend's default render endpoint is still this one; false when the endpoints
    // cannot be read, so the watchdog can recover.
    std::optional<bool> is_default_device_current() override;

    const audio::LoopbackEndpoint& endpoint() const { return endpoint_; }

private:
    static void on_capture(void* user, const std::int16_t* frames, std::size_t count,
                           audio::BlockStatus status);
    void close_stream();

    std::shared_ptr<audio::Backend> backend_;
    const MeetingClock& clock_;
    audio::LoopbackEndpoint endpoint_;
    double first_block_wait_s_;
    BlockSink on_block_;
    std::unique_ptr<detail::BlockShaper> shaper_;
    std::vector<float> weights_; // loopback_mix_weights(endpoint_.channels)
    std::unique_ptr<audio::Stream> stream_;
    std::atomic<bool> running_{false};
    std::atomic<bool> first_block_{false};
    std::atomic<bool> failed_{false};
    mutable std::atomic<bool> lost_{false};
    mutable std::atomic<bool> lost_logged_{false};
};

} // namespace fv::meeting
