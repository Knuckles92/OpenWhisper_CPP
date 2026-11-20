#include "audio/detail/loopback_pump.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace fv::audio::detail {

std::int16_t float_to_int16(float sample) {
    // float32 arithmetic throughout, as numpy keeps a float32 array float32 when it is scaled by
    // a Python float; astype(np.int16) then truncates toward zero.
    const float scaled = sample * 32767.0F;
    if (std::isnan(scaled)) {
        return 0;
    }
    return static_cast<std::int16_t>(std::clamp(scaled, -32768.0F, 32767.0F));
}

LoopbackFormatChoice choose_loopback_format(const MixFormat& mix, int rate, int channels) {
    if (mix.rate == rate && mix.channels == channels) {
        if (mix.kind == MixFormat::Kind::Float && mix.bits_per_sample == 32 &&
            mix.valid_bits == 32) {
            return {.format = LoopbackSampleFormat::Float32, .convert = false};
        }
        if (mix.kind == MixFormat::Kind::Pcm && mix.bits_per_sample == 16 && mix.valid_bits == 16) {
            return {.format = LoopbackSampleFormat::Int16, .convert = false};
        }
    }
    return {.format = LoopbackSampleFormat::Float32, .convert = true};
}

LoopbackPump::LoopbackPump(int rate, int channels, LoopbackSampleFormat format, CaptureFn fn,
                           void* user)
    : rate_(rate), channels_(channels), format_(format), fn_(fn), user_(user),
      block_(kLoopbackBlockFrames * static_cast<std::size_t>(channels), std::int16_t{0}) {}

std::size_t LoopbackPump::frames_for(double seconds) const {
    if (!(seconds > 0.0)) {
        return 0;
    }
    return static_cast<std::size_t>(std::floor(seconds * rate_));
}

void LoopbackPump::deliver(const std::int16_t* samples, std::size_t frames, BlockStatus status) {
    if (fn_ != nullptr) {
        fn_(user_, samples, frames, status);
    }
}

void LoopbackPump::packet(const void* data, std::size_t frames, bool silent, bool discontinuity,
                          double adc_time, double now) {
    const auto* bytes = static_cast<const unsigned char*>(data);
    const auto channels = static_cast<std::size_t>(channels_);
    const bool zeros = silent || bytes == nullptr;
    std::size_t done = 0;
    while (done < frames) {
        const std::size_t count = std::min(kLoopbackBlockFrames, frames - done);
        const std::size_t samples = count * channels;
        BlockStatus status;
        status.overflow = discontinuity && done == 0;
        status.adc_time = adc_time + static_cast<double>(done) / rate_;
        status.current_time = now;
        if (zeros) {
            std::fill_n(block_.begin(), samples, std::int16_t{0});
        } else if (format_ == LoopbackSampleFormat::Int16) {
            std::memcpy(block_.data(), bytes + done * channels * sizeof(std::int16_t),
                        samples * sizeof(std::int16_t));
        } else {
            const unsigned char* at = bytes + done * channels * sizeof(float);
            for (std::size_t i = 0; i < samples; ++i) {
                float sample = 0.0F;
                std::memcpy(&sample, at + i * sizeof(float), sizeof(float));
                block_[i] = float_to_int16(sample);
            }
        }
        deliver(block_.data(), count, status);
        done += count;
    }
}

void LoopbackPump::silence(std::size_t frames, double adc_time, double now) {
    packet(nullptr, frames, true, false, adc_time, now);
}

void LoopbackPump::start(double now) {
    quiet_since_ = now;
    first_packet_ = true;
}

void LoopbackPump::read(const LoopbackPacket& read_packet, double now) {
    // pu64QPCPosition is the performance counter converted to 100 ns units, so it shares the
    // clock of QueryPerformanceCounter / QueryPerformanceFrequency.
    const bool stamped = read_packet.qpc_position != 0 && !read_packet.timestamp_error;
    const double adc = stamped ? static_cast<double>(read_packet.qpc_position) * 1e-7
                               : std::numeric_limits<double>::quiet_NaN();
    const bool discontinuity = read_packet.discontinuity && !first_packet_;
    first_packet_ = false;
    packet(read_packet.data, read_packet.frames, read_packet.silent, discontinuity, adc, now);
    quiet_since_ = now;
}

void LoopbackPump::idle(double now) {
    const double quiet = now - quiet_since_;
    if (!(quiet >= kLoopbackSilenceAfterMs / 1000.0)) {
        return;
    }
    const std::size_t frames = frames_for(quiet);
    silence(frames, quiet_since_, now);
    quiet_since_ += static_cast<double>(frames) / rate_;
}

} // namespace fv::audio::detail
