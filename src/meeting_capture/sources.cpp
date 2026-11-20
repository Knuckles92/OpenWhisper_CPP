#include "meeting_capture/sources.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <span>
#include <stdexcept>
#include <thread>

#include "base/log.h"
#include "meeting_state/types.h"

namespace fv::meeting {

namespace {

constexpr log::Logger mic_logger("meeting.capture.sd_stream");
constexpr log::Logger loopback_logger("meeting.capture.soundcard_stream");

constexpr int kStatusOverflow = 1;
constexpr int kStatusUnderflow = 2;

std::string status_text(int flags) {
    std::string text;
    if ((flags & kStatusOverflow) != 0) {
        text = "input overflow";
    }
    if ((flags & kStatusUnderflow) != 0) {
        text += text.empty() ? "input underflow" : ", input underflow";
    }
    return text;
}

} // namespace

namespace detail {

// Turns interleaved device frames into mono blocks without allocating: a block wider than the
// preallocated buffer goes out in consecutive pieces, each stamped where its first frame falls.
// Affinity: the device thread of one stream.
class BlockShaper {
public:
    static constexpr std::size_t kFrames = 4096;

    BlockShaper() : mono_(kFrames) {}

    // `channels` interleaved samples per frame, of which the first `mixed` are averaged.
    void deliver(const std::int16_t* frames, std::size_t count, int channels, int mixed, int rate,
                 double t_mono, std::string_view channel, const BlockSink& sink) {
        if (channels <= 1) {
            sink(CaptureBlock{.channel = channel,
                              .frames = frames,
                              .count = count,
                              .rate = rate,
                              .t_mono = t_mono});
            return;
        }
        const auto width = static_cast<std::size_t>(channels);
        for (std::size_t at = 0; at < count; at += kFrames) {
            const std::size_t piece = std::min(kFrames, count - at);
            const std::int16_t* source = frames + at * width;
            for (std::size_t i = 0; i < piece; ++i) {
                std::int32_t sum = 0;
                for (std::size_t c = 0; c < static_cast<std::size_t>(mixed); ++c) {
                    sum += source[i * width + c];
                }
                // numpy's mean then astype(int16): the quotient truncated toward zero.
                mono_[i] = static_cast<std::int16_t>(sum / mixed);
            }
            const double offset = rate > 0 ? static_cast<double>(at) / rate : 0.0;
            sink(CaptureBlock{.channel = channel,
                              .frames = mono_.data(),
                              .count = piece,
                              .rate = rate,
                              .t_mono = t_mono + offset});
        }
    }

    // As deliver, each frame's mono sample the sum of its channels times `weights` (one per
    // channel), saturated to int16 and truncated toward zero.
    void deliver_weighted(const std::int16_t* frames, std::size_t count,
                          std::span<const float> weights, int rate, double t_mono,
                          std::string_view channel, const BlockSink& sink) {
        const std::size_t width = weights.size();
        for (std::size_t at = 0; at < count; at += kFrames) {
            const std::size_t piece = std::min(kFrames, count - at);
            const std::int16_t* source = frames + at * width;
            for (std::size_t i = 0; i < piece; ++i) {
                float sum = 0.0F;
                for (std::size_t c = 0; c < width; ++c) {
                    sum += weights[c] * static_cast<float>(source[i * width + c]);
                }
                mono_[i] = static_cast<std::int16_t>(std::clamp(sum, -32768.0F, 32767.0F));
            }
            const double offset = rate > 0 ? static_cast<double>(at) / rate : 0.0;
            sink(CaptureBlock{.channel = channel,
                              .frames = mono_.data(),
                              .count = piece,
                              .rate = rate,
                              .t_mono = t_mono + offset});
        }
    }

private:
    std::vector<std::int16_t> mono_;
};

} // namespace detail

std::vector<float> loopback_mix_weights(int channels) {
    // ITU-R BS.775's stereo downmix, Lo = L + 0.7071 C + 0.7071 Ls (Ro alike, LFE dropped),
    // then the mean of Lo and Ro. The channel order is Windows' default for the count
    // (KSAUDIO_SPEAKER_QUAD, 5POINT1, 7POINT1_SURROUND: FL FR [FC LFE] then the surrounds).
    constexpr float kFront = 0.5F;
    constexpr float kCentre = 0.70710678F;
    constexpr float kSurround = 0.70710678F / 2.0F;
    switch (channels) {
    case 4:
        return {kFront, kFront, kSurround, kSurround};
    case 6:
        return {kFront, kFront, kCentre, 0.0F, kSurround, kSurround};
    case 8:
        return {kFront, kFront, kCentre, 0.0F, kSurround, kSurround, kSurround, kSurround};
    default:
        return {};
    }
}

double capture_timestamp(double delivered_mono, const audio::BlockStatus& status) {
    const double adc = status.adc_time;
    const double current = status.current_time;
    if (std::isfinite(adc) && std::isfinite(current) && current > 0.0 && 0.0 <= adc &&
        adc <= current) {
        return delivered_mono - (current - adc);
    }
    return delivered_mono;
}

// --- MicSource ---------------------------------------------------------------------------------

MicSource::MicSource(std::shared_ptr<audio::Backend> backend, const MeetingClock& clock,
                     std::string channel, int device_index, int samplerate, int channels)
    : backend_(std::move(backend)), clock_(clock), channel_(std::move(channel)),
      device_index_(device_index), samplerate_(samplerate), channels_(std::max(1, channels)),
      shaper_(std::make_unique<detail::BlockShaper>()) {}

MicSource::~MicSource() {
    active_.store(false);
    close_stream();
}

void MicSource::on_capture(void* user, const std::int16_t* frames, std::size_t count,
                           audio::BlockStatus status) {
    auto* self = static_cast<MicSource*>(user);
    if (status.overflow || status.underflow) {
        int expected = 0;
        self->status_flags_.compare_exchange_strong(expected,
                                                    (status.overflow ? kStatusOverflow : 0) |
                                                        (status.underflow ? kStatusUnderflow : 0));
    }
    const double t_mono = capture_timestamp(self->clock_.monotonic(), status);
    if (!self->active_.load(std::memory_order_acquire) || !self->on_block_) {
        return;
    }
    self->shaper_->deliver(frames, count, self->channels_, self->channels_, self->samplerate_,
                           t_mono, self->channel_, self->on_block_);
}

void MicSource::start(BlockSink on_block) {
    on_block_ = std::move(on_block);
    lost_.store(false);
    lost_logged_.store(false);
    const audio::StreamConfig config{.device_index = device_index_,
                                     .rate = samplerate_,
                                     .channels = channels_,
                                     .frames_per_block = MIC_BLOCK_FRAMES,
                                     // sd.InputStream without extra_settings: WASAPI converts
                                     // nothing, and the native rate needs no conversion.
                                     .wasapi_auto_convert = false};
    auto opened = backend_->open_capture(config, &MicSource::on_capture, this,
                                         [this](const std::string& /*reason*/) {
                                             lost_.store(true);
                                             active_.store(false);
                                         });
    if (!opened) {
        const std::runtime_error error(opened.error());
        mic_logger.error_with(
            error, std::format("Failed to start {} capture on device {}", channel_, device_index_));
        active_.store(false);
        close_stream();
        throw error;
    }
    stream_ = std::move(*opened);
    active_.store(true, std::memory_order_release);
    mic_logger.info("Capture started: channel={} device={} rate={} channels={}", channel_,
                    device_index_, samplerate_, channels_);
}

void MicSource::stop() {
    active_.store(false);
    close_stream();
    mic_logger.info("Capture stopped: channel={}", channel_);
}

void MicSource::close_stream() {
    std::unique_ptr<audio::Stream> stream = std::move(stream_);
    if (!stream) {
        return;
    }
    try {
        stream->abort();
    } catch (const std::exception&) {
        mic_logger.debug("Stream stop failed ({})", channel_);
    }
    stream.reset();
}

void MicSource::log_device_events() const {
    const int flags = status_flags_.load();
    if (flags != 0 && !status_logged_.exchange(true)) {
        mic_logger.warning("Capture stream status ({}): {}", channel_, status_text(flags));
    }
    if (lost_.load() && !lost_logged_.exchange(true)) {
        mic_logger.warning("Capture stream ended unexpectedly: channel={}", channel_);
    }
}

bool MicSource::is_active() const {
    log_device_events();
    if (!active_.load() || !stream_) {
        return false;
    }
    try {
        return stream_->active();
    } catch (const std::exception&) {
        mic_logger.debug("Stream state unreadable ({}); treating it as inactive", channel_);
        return false;
    }
}

// --- LoopbackSource ----------------------------------------------------------------------------

LoopbackSource::LoopbackSource(std::shared_ptr<audio::Backend> backend, const MeetingClock& clock,
                               audio::LoopbackEndpoint endpoint, double first_block_wait_s)
    : backend_(std::move(backend)), clock_(clock), endpoint_(std::move(endpoint)),
      first_block_wait_s_(first_block_wait_s), shaper_(std::make_unique<detail::BlockShaper>()),
      weights_(loopback_mix_weights(endpoint_.channels)) {}

LoopbackSource::~LoopbackSource() {
    running_.store(false);
    close_stream();
}

std::string_view LoopbackSource::channel() const {
    return meeting_state::kChannelLoopback;
}

void LoopbackSource::on_capture(void* user, const std::int16_t* frames, std::size_t count,
                                audio::BlockStatus status) {
    auto* self = static_cast<LoopbackSource*>(user);
    const double t_mono = capture_timestamp(self->clock_.monotonic(), status);
    if (!self->running_.load(std::memory_order_acquire) || !self->on_block_ || count == 0) {
        return;
    }
    const int channels = std::max(1, self->endpoint_.channels);
    if (!self->weights_.empty()) {
        self->shaper_->deliver_weighted(frames, count, self->weights_, self->endpoint_.rate, t_mono,
                                        meeting_state::kChannelLoopback, self->on_block_);
        self->first_block_.store(true, std::memory_order_release);
        return;
    }
    // A layout without a known order: the front pair only, as SoundCard recorded at most two
    // channels; averaging every channel would cut stereo content below the quiet and signal
    // thresholds.
    self->shaper_->deliver(frames, count, channels, std::min(2, channels), self->endpoint_.rate,
                           t_mono, meeting_state::kChannelLoopback, self->on_block_);
    self->first_block_.store(true, std::memory_order_release);
}

void LoopbackSource::start(BlockSink on_block) {
    if (stream_ && running_.load()) {
        loopback_logger.warning("Loopback capture already running");
        return;
    }
    on_block_ = std::move(on_block);
    first_block_.store(false);
    failed_.store(false);
    lost_.store(false);
    lost_logged_.store(false);
    running_.store(true, std::memory_order_release);
    auto opened =
        backend_->open_loopback(endpoint_, &LoopbackSource::on_capture, this,
                                [this](const std::string& /*reason*/) { lost_.store(true); });
    if (!opened) {
        const std::runtime_error error(opened.error());
        loopback_logger.error_with(
            error, std::format("Failed to start loopback capture on {}", endpoint_.name));
        running_.store(false);
        failed_.store(true);
        throw error;
    }
    stream_ = std::move(*opened);
    loopback_logger.info("Loopback capture opened (endpoint={} rate={} channels={})",
                         endpoint_.name, endpoint_.rate, endpoint_.channels);
    // The device thread may not signal (no syscalls there), so the wait polls.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::duration<double>(first_block_wait_s_);
    while (!first_block_.load(std::memory_order_acquire) && !lost_.load()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            loopback_logger.warning("Loopback capture did not deliver audio within {:.1f}s",
                                    first_block_wait_s_);
            failed_.store(true);
            running_.store(false);
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (first_block_.load()) {
        loopback_logger.info("Loopback capture first audio block @ {} Hz", endpoint_.rate);
    }
}

void LoopbackSource::stop() {
    running_.store(false);
    close_stream();
    loopback_logger.info("Loopback capture stopped");
}

void LoopbackSource::close_stream() {
    std::unique_ptr<audio::Stream> stream = std::move(stream_);
    if (!stream) {
        return;
    }
    try {
        stream->abort();
    } catch (const std::exception&) {
        loopback_logger.debug("Loopback stream stop failed");
    }
    stream.reset();
}

bool LoopbackSource::is_active() const {
    if (lost_.load() && !lost_logged_.exchange(true)) {
        loopback_logger.warning("Loopback capture stream ended unexpectedly");
    }
    if (!running_.load() || failed_.load() || lost_.load() || !stream_) {
        return false;
    }
    try {
        return stream_->active();
    } catch (const std::exception&) {
        loopback_logger.debug("Loopback stream state unreadable; treating it as inactive");
        return false;
    }
}

std::optional<bool> LoopbackSource::is_default_device_current() {
    try {
        for (const audio::LoopbackEndpoint& endpoint : backend_->loopbacks()) {
            if (endpoint.is_default) {
                return endpoint.id == endpoint_.id;
            }
        }
        return false;
    } catch (const std::exception&) {
        // Fail closed so the watchdog can recover.
        loopback_logger.debug("Could not probe the default render endpoint");
        return false;
    }
}

} // namespace fv::meeting
