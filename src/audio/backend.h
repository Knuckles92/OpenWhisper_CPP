#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fv::audio {

struct DeviceInfo {
    int index;
    std::string name;     // PortAudio's names, UTF-8
    std::string host_api; // "MME", "Windows WASAPI", ... (PortAudio names)
    int max_input_channels, max_output_channels;
    double default_rate;
    // PortAudio's default device of the list's direction: Pa_GetDefaultInputDevice for
    // inputs(), else the default host API's default input (sounddevice's default.device[0]).
    bool is_default = false;
    // On PortAudio's default host API (MME on Windows, Core Audio, ALSA).
    bool default_api = false;
};

struct DeviceKey { // the saved audio_input_priority shape
    std::string name, hostapi;
    bool operator==(const DeviceKey&) const = default;
};

// A render endpoint captured through WASAPI loopback (meetings), from IMMDeviceEnumerator.
// open_loopback delivers interleaved int16 at `rate` and `channels`: the endpoint's mix format as
// read at enumeration, converted by the audio engine should the mix format change before the open.
struct LoopbackEndpoint {
    std::string id;   // the endpoint id string, stable across restarts
    std::string name; // the friendly name, UTF-8
    bool is_default = false;
    int rate = 48000; // Hz
    int channels = 2;
};

// device_index below 0 opens PortAudio's default device of the stream's direction (sounddevice's
// device=None). wasapi_auto_convert applies only to a WASAPI device (audio_devices.extra_settings).
struct StreamConfig {
    int device_index;
    int rate = 44100;
    int channels = 1;
    int frames_per_block = 1024;
    bool wasapi_auto_convert = true;
};

// The two times are seconds on one clock, NaN when unknown: `adc_time` is when the block's first
// frame was captured and `current_time` when the block was handed over. PortAudio fills them from
// PaStreamCallbackTimeInfo (inputBufferAdcTime, currentTime; MME may give 0). Loopback gives the
// packet's QPC position and QueryPerformanceCounter now. overflow marks a block after lost
// frames (a WASAPI data discontinuity on loopback).
struct BlockStatus {
    bool overflow = false, underflow = false;
    double adc_time = std::numeric_limits<double>::quiet_NaN();
    double current_time = std::numeric_limits<double>::quiet_NaN();
};

// Runs on the device thread: no allocation, no locks, no logging, no syscalls. `count` is in
// frames; `frames` holds count * channels interleaved samples.
using CaptureFn = void (*)(void* user, const std::int16_t* frames, std::size_t count, BlockStatus);

// What a playback stream plays. Called on the device thread under the CaptureFn rules.
class PlaybackSource {
public:
    virtual ~PlaybackSource() = default;
    // Fills `out` (interleaved int16 at the stream's rate and channels) and returns the frames
    // written; fewer than asked ends playback once they have played.
    virtual std::size_t read(std::span<std::int16_t> out) = 0;
    // Called once when the stream stops, played out or aborted (PortAudio's finished callback),
    // on the device thread or inside abort(). The stream cannot be closed from in here.
    virtual void finished() {}
};

// Affinity: thread-safe.
class Stream {
public:
    virtual ~Stream() = default;
    virtual bool active() const = 0;
    virtual void abort() = 0;
};

// Affinity: thread-safe; device queries and opens block, so never on the GUI thread.
// inputs(), outputs() and loopbacks() throw std::runtime_error when the audio system cannot be
// read, and refresh() when it does not restart (false means only "refused: a PortAudio stream is
// open"; a loopback stream does not count). open_capture's and open_loopback's on_lost run once,
// on the device thread, when the stream ends by itself; never after the owner's abort() or
// destruction. Destroying a Stream closes it.
class Backend {
public:
    virtual ~Backend() = default;
    virtual std::vector<DeviceInfo> inputs() = 0; // never on the GUI thread
    virtual std::vector<DeviceInfo> outputs() = 0;
    virtual std::vector<LoopbackEndpoint> loopbacks() = 0; // render endpoints (meetings)
    // The id of the default render endpoint, nullopt when there is none or it cannot be read;
    // never throws. The meeting watchdog asks every second, so a backend answers it more
    // cheaply than loopbacks() where it can; this default lists the endpoints.
    virtual std::optional<std::string> default_loopback_id();
    virtual std::expected<void, std::string> check_input(const StreamConfig&) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
    open_capture(const StreamConfig&, CaptureFn, void* user,
                 std::function<void(std::string)> on_lost) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string>
    open_loopback(const LoopbackEndpoint&, CaptureFn, void* user,
                  std::function<void(std::string)> on_lost) = 0;
    virtual std::expected<std::unique_ptr<Stream>, std::string> open_playback(const StreamConfig&,
                                                                              PlaybackSource&) = 0;
    virtual bool refresh() = 0; // refused while a stream is open (A05-07)
};

std::shared_ptr<Backend> make_portaudio_backend(); // loopback via WASAPI in audio/win/
std::vector<DeviceInfo> ranked_candidates(std::span<const DeviceKey>,
                                          std::span<const DeviceInfo>); // the a05 ladder

} // namespace fv::audio
