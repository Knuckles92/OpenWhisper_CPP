#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "audio/backend.h"

// The platform-free half of the WASAPI loopback stream: which format to ask the audio engine
// for, the float32 to int16 conversion, and the hand-over of packets and synthesized silence to
// a CaptureFn in blocks of at most kLoopbackBlockFrames.
namespace fv::audio::detail {

// soundcard_stream.py's BLOCK_FRAMES, the most frames one CaptureFn call receives.
inline constexpr std::size_t kLoopbackBlockFrames = 1024;

// Without a packet for this long the endpoint is taken as silent and zeros are delivered for the
// time that passed, as soundcard's reads return zeros (some drivers stop delivering while the
// endpoint plays nothing). Milliseconds.
inline constexpr int kLoopbackSilenceAfterMs = 100;
// The wait between packet reads when the audio engine refuses an event handle (event-driven
// loopback needs Windows 10 1703). Milliseconds.
inline constexpr int kLoopbackPollMs = 10;

// soundcard_stream.py's float to int16 step on one sample: np.clip(x * 32767.0, -32768.0,
// 32767.0).astype(np.int16), in float32, truncating toward zero. NaN becomes 0.
std::int16_t float_to_int16(float sample);

enum class LoopbackSampleFormat { Float32, Int16 };

// A mix format as the audio engine reports it, without Windows types.
struct MixFormat {
    enum class Kind { Float, Pcm, Other };
    Kind kind = Kind::Other;
    int bits_per_sample = 0; // the container size (wBitsPerSample)
    int valid_bits = 0;      // WAVEFORMATEXTENSIBLE's wValidBitsPerSample, else bits_per_sample
    int rate = 0;
    int channels = 0;
};

struct LoopbackFormatChoice {
    LoopbackSampleFormat format = LoopbackSampleFormat::Float32;
    // false: initialize with the mix format itself. true: ask for float32 at the endpoint's rate
    // and channels and let the audio engine convert (soundcard's AUTOCONVERTPCM and
    // SRC_DEFAULT_QUALITY flags), when the mix format is neither float32 nor 16-bit PCM or no
    // longer matches the endpoint as it was listed.
    bool convert = false;
};

LoopbackFormatChoice choose_loopback_format(const MixFormat& mix, int rate, int channels);

// What one IAudioCaptureClient::GetBuffer handed over, without Windows types.
struct LoopbackPacket {
    const void* data = nullptr;
    std::size_t frames = 0;
    bool silent = false;            // AUDCLNT_BUFFERFLAGS_SILENT
    bool discontinuity = false;     // AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY
    bool timestamp_error = false;   // AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR
    std::uint64_t qpc_position = 0; // pu64QPCPosition: the first frame's QPC in 100 ns units
};

// Converts packets and hands them to the CaptureFn under its rules: the constructor allocates
// the one block buffer, and nothing after it allocates, locks or calls the system.
// Affinity: the loopback thread.
class LoopbackPump {
public:
    LoopbackPump(int rate, int channels, LoopbackSampleFormat format, CaptureFn fn, void* user);

    // The stream started at `now` (QPC seconds): the quiet that idle() measures begins here.
    void start(double now);
    // A packet read at `now`. Its capture time is the QPC position in seconds, NaN when WASAPI
    // flags a timestamp error or gives no position. The first packet's discontinuity is not
    // reported, since WASAPI flags a stream's first packet (soundcard ignores it too).
    void read(const LoopbackPacket& packet, double now);
    // A wake that found no packet. Once kLoopbackSilenceAfterMs have passed since the last packet
    // (or the last zeros), zeros for all the time since then, rounded down to whole frames and
    // stamped from where the quiet began; the quiet then resumes after them.
    void idle(double now);

    // One packet of `frames` frames in the stream's format. `silent` (AUDCLNT_BUFFERFLAGS_SILENT)
    // delivers zeros whatever `data` holds; `discontinuity` marks the first block overflow.
    // `adc_time` is the first frame's capture time and `now` the hand-over time, in seconds;
    // a later block of the packet is stamped `adc_time` plus its offset.
    void packet(const void* data, std::size_t frames, bool silent, bool discontinuity,
                double adc_time, double now);
    // `frames` of zeros whose first frame was at `adc_time`.
    void silence(std::size_t frames, double adc_time, double now);

    // Frames for `seconds` of silence at the stream's rate, rounded down.
    std::size_t frames_for(double seconds) const;
    int rate() const { return rate_; }
    int channels() const { return channels_; }

private:
    void deliver(const std::int16_t* samples, std::size_t frames, BlockStatus status);

    int rate_;
    int channels_;
    LoopbackSampleFormat format_;
    CaptureFn fn_;
    void* user_;
    std::vector<std::int16_t> block_; // kLoopbackBlockFrames * channels
    double quiet_since_ = 0.0;        // QPC seconds: where audio not yet delivered begins
    bool first_packet_ = true;
};

} // namespace fv::audio::detail
