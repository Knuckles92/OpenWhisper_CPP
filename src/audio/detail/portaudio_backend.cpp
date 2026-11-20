// audio::Backend on PortAudio, the library sounddevice wraps, so device names, host-API names,
// format checks and error texts are the ones OpenWhisper sees. Device queries and stream opens
// run under portaudio_mutex(); stream starts and closes do not, as in recorder.py.
#include <portaudio.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "audio/backend.h"
#include "audio/detail/portaudio_error.h"
#include "audio/detail/portaudio_platform.h"
#include "audio/devices.h"
#include "audio/win/wasapi_loopback.h"

namespace fv::audio {

namespace {

// str() of the PortAudioError sounddevice's _check() raises; for paUnanticipatedHostError it
// carries the host API's own error, read at once before another call overwrites it.
std::string error_text(PaError error, std::string_view message = {}) {
    std::optional<detail::PortAudioHostError> host;
    if (error == paUnanticipatedHostError) {
        if (const PaHostErrorInfo* info = Pa_GetLastHostErrorInfo(); info != nullptr) {
            const PaHostApiIndex index = Pa_HostApiTypeIdToHostApiIndex(info->hostApiType);
            detail::PortAudioHostError details;
            if (index == paHostApiNotFound) {
                details.host_api = "<host API not found>";
            } else if (index < 0) {
                details.host_api = std::format("<error getting host API: {}>", index);
            } else {
                const PaHostApiInfo* api = Pa_GetHostApiInfo(index);
                details.host_api = api != nullptr && api->name != nullptr ? api->name : "";
            }
            details.code = info->errorCode;
            details.text = info->errorText != nullptr ? info->errorText : "";
            host = std::move(details);
        }
    }
    const char* text = Pa_GetErrorText(error);
    return detail::portaudio_error_text(message, text != nullptr ? text : "", error, host);
}

// Pa_Initialize once per process, lazily, so building the backend costs nothing on the GUI
// thread. Terminated only by refresh; never at exit, which could race a closing stream.
bool g_initialized = false;

void ensure_initialized() {
    if (g_initialized) {
        return;
    }
    const PaError error = Pa_Initialize();
    if (error != paNoError) {
        throw std::runtime_error(error_text(error, "Error initializing PortAudio"));
    }
    g_initialized = true;
}

class Lifecycle final : public PortAudioLifecycle {
public:
    void terminate() override {
        if (g_initialized) {
            Pa_Terminate();
            g_initialized = false;
        }
    }
    void initialize() override { ensure_initialized(); }
};

std::string host_api_name(PaHostApiIndex index) {
    const PaHostApiInfo* host = index >= 0 ? Pa_GetHostApiInfo(index) : nullptr;
    return host != nullptr && host->name != nullptr ? std::string(host->name) : std::string();
}

std::vector<DeviceInfo> list_devices(bool input) {
    ensure_initialized();
    const PaDeviceIndex count = Pa_GetDeviceCount();
    if (count < 0) {
        throw std::runtime_error(error_text(count));
    }
    const PaHostApiIndex default_api = Pa_GetDefaultHostApi();
    std::vector<DeviceInfo> devices;
    for (PaDeviceIndex index = 0; index < count; ++index) {
        const PaDeviceInfo* info = Pa_GetDeviceInfo(index);
        if (info == nullptr) {
            continue;
        }
        if ((input ? info->maxInputChannels : info->maxOutputChannels) <= 0) {
            continue;
        }
        DeviceInfo device{.index = index,
                          .name = detail::device_name(*info),
                          .host_api = host_api_name(info->hostApi),
                          .max_input_channels = info->maxInputChannels,
                          .max_output_channels = info->maxOutputChannels,
                          .default_rate = info->defaultSampleRate};
        device.default_api = info->hostApi == default_api;
        devices.push_back(std::move(device));
    }
    const PaHostApiInfo* host = default_api >= 0 ? Pa_GetHostApiInfo(default_api) : nullptr;
    int default_index = -1;
    if (input) {
        default_index = default_input_index(devices, Pa_GetDefaultInputDevice(),
                                            host != nullptr ? host->defaultInputDevice : -1);
    } else {
        default_index = Pa_GetDefaultOutputDevice();
        if (default_index < 0 && host != nullptr) {
            default_index = host->defaultOutputDevice;
        }
    }
    for (DeviceInfo& device : devices) {
        device.is_default = device.index == default_index;
    }
    return devices;
}

// The device a stream config names; sounddevice's "Error querying device <n>" when none.
std::expected<std::pair<PaDeviceIndex, const PaDeviceInfo*>, std::string>
resolve_device(int requested, bool input) {
    const PaDeviceIndex index =
        requested >= 0 ? requested
                       : (input ? Pa_GetDefaultInputDevice() : Pa_GetDefaultOutputDevice());
    const PaDeviceInfo* info = index >= 0 ? Pa_GetDeviceInfo(index) : nullptr;
    if (info == nullptr) {
        return std::unexpected(std::format("Error querying device {}", index));
    }
    return std::pair{index, info};
}

struct InputParameters {
    PaStreamParameters parameters{};
    std::shared_ptr<void> storage;
};

std::expected<InputParameters, std::string> input_parameters(const StreamConfig& config) {
    auto device = resolve_device(config.device_index, true);
    if (!device) {
        return std::unexpected(device.error());
    }
    InputParameters made;
    made.parameters.device = device->first;
    made.parameters.channelCount = config.channels;
    made.parameters.sampleFormat = paInt16;
    // sounddevice's default latency is "high".
    made.parameters.suggestedLatency = device->second->defaultHighInputLatency;
    made.parameters.hostApiSpecificStreamInfo =
        detail::input_stream_info(*device->second, config.wasapi_auto_convert, made.storage);
    return made;
}

// A PortAudio stream; destroying it aborts and closes it.
class PaStreamBase : public Stream {
public:
    bool active() const override { return stream_ != nullptr && Pa_IsStreamActive(stream_) == 1; }

    void abort() override {
        closing_.store(true);
        if (stream_ != nullptr) {
            Pa_AbortStream(stream_);
        }
    }

    PaStream** handle() { return &stream_; }

    // Registers the opened stream and its finished callback.
    void opened() {
        register_stream(this);
        Pa_SetStreamFinishedCallback(stream_, &PaStreamBase::on_finished);
    }

    std::expected<void, std::string> start() {
        const PaError error = Pa_StartStream(stream_);
        if (error != paNoError) {
            close();
            return std::unexpected(error_text(error, "Error starting stream"));
        }
        return {};
    }

protected:
    virtual void finished() = 0;
    std::atomic<bool> closing_{false};

    // Every subclass calls it from its own destructor: closing can run finished(), which the
    // base destructor could no longer reach.
    void close() {
        if (stream_ == nullptr) {
            return;
        }
        closing_.store(true);
        // Abort first: 7 ms against 33 ms for a stop (recorder.py:751).
        Pa_AbortStream(stream_);
        Pa_CloseStream(stream_);
        stream_ = nullptr;
        unregister_stream(this);
    }

private:
    static void on_finished(void* user) { static_cast<PaStreamBase*>(user)->finished(); }

    PaStream* stream_ = nullptr;
};

class CaptureStream final : public PaStreamBase {
public:
    CaptureStream(CaptureFn fn, void* user, std::function<void(std::string)> on_lost)
        : fn_(fn), user_(user), on_lost_(std::move(on_lost)) {}
    ~CaptureStream() override { close(); }
    CaptureStream(const CaptureStream&) = delete;
    CaptureStream& operator=(const CaptureStream&) = delete;

    static int callback(const void* input, void* /*output*/, unsigned long frames,
                        const PaStreamCallbackTimeInfo* time, PaStreamCallbackFlags flags,
                        void* user) {
        auto* self = static_cast<CaptureStream*>(user);
        if (input != nullptr) {
            BlockStatus status;
            status.overflow = (flags & paInputOverflow) != 0;
            status.underflow = (flags & paInputUnderflow) != 0;
            // As PortAudio reports them, 0 included: the reader judges which clocks are usable
            // (sd_stream.py's rule).
            if (time != nullptr) {
                status.adc_time = time->inputBufferAdcTime;
                status.current_time = time->currentTime;
            }
            self->fn_(self->user_, static_cast<const std::int16_t*>(input), frames, status);
        }
        return paContinue;
    }

protected:
    void finished() override {
        // PortAudio's thread: no logging and no PortAudio calls here.
        if (!closing_.load() && on_lost_) {
            on_lost_(std::string());
        }
    }

private:
    CaptureFn fn_;
    void* user_;
    std::function<void(std::string)> on_lost_;
};

class PlaybackStream final : public PaStreamBase {
public:
    PlaybackStream(PlaybackSource& source, int channels) : source_(source), channels_(channels) {}
    ~PlaybackStream() override { close(); }
    PlaybackStream(const PlaybackStream&) = delete;
    PlaybackStream& operator=(const PlaybackStream&) = delete;

    static int callback(const void* /*input*/, void* output, unsigned long frames,
                        const PaStreamCallbackTimeInfo* /*time*/, PaStreamCallbackFlags /*flags*/,
                        void* user) {
        auto* self = static_cast<PlaybackStream*>(user);
        const std::size_t samples = frames * static_cast<std::size_t>(self->channels_);
        std::span<std::int16_t> out(static_cast<std::int16_t*>(output), samples);
        const std::size_t written = std::min<std::size_t>(self->source_.read(out), frames);
        if (written < frames) {
            std::fill(out.begin() + static_cast<std::ptrdiff_t>(written * self->channels_),
                      out.end(), std::int16_t{0});
            return paComplete;
        }
        return paContinue;
    }

protected:
    void finished() override {
        if (!told_.exchange(true)) {
            source_.finished();
        }
    }

private:
    PlaybackSource& source_;
    int channels_;
    std::atomic<bool> told_{false};
};

class PortAudioBackend final : public Backend {
public:
    std::vector<DeviceInfo> inputs() override {
        const std::lock_guard lock(portaudio_mutex());
        return list_devices(true);
    }

    std::vector<DeviceInfo> outputs() override {
        const std::lock_guard lock(portaudio_mutex());
        return list_devices(false);
    }

    // WASAPI loopback is not a PortAudio stream: it takes no PortAudio lock and does not hold off
    // refresh().
    std::vector<LoopbackEndpoint> loopbacks() override { return detail::wasapi_loopbacks(); }
    // One GetDefaultAudioEndpoint, where the base class would list and activate every endpoint.
    std::optional<std::string> default_loopback_id() override {
        return detail::wasapi_default_loopback_id();
    }

    std::expected<void, std::string> check_input(const StreamConfig& config) override {
        const std::lock_guard lock(portaudio_mutex());
        try {
            ensure_initialized();
        } catch (const std::exception& error) {
            return std::unexpected(std::string(error.what()));
        }
        auto parameters = input_parameters(config);
        if (!parameters) {
            return std::unexpected(parameters.error());
        }
        const PaError error = Pa_IsFormatSupported(&parameters->parameters, nullptr, config.rate);
        if (error != paFormatIsSupported) {
            return std::unexpected(error_text(error));
        }
        return {};
    }

    std::expected<std::unique_ptr<Stream>, std::string>
    open_capture(const StreamConfig& config, CaptureFn fn, void* user,
                 std::function<void(std::string)> on_lost) override {
        auto stream = std::make_unique<CaptureStream>(fn, user, std::move(on_lost));
        {
            const std::lock_guard lock(portaudio_mutex());
            try {
                ensure_initialized();
            } catch (const std::exception& error) {
                return std::unexpected(std::string(error.what()));
            }
            auto parameters = input_parameters(config);
            if (!parameters) {
                return std::unexpected(parameters.error());
            }
            const PaError error =
                Pa_OpenStream(stream->handle(), &parameters->parameters, nullptr, config.rate,
                              static_cast<unsigned long>(config.frames_per_block), paNoFlag,
                              &CaptureStream::callback, stream.get());
            if (error != paNoError) {
                return std::unexpected(error_text(error, "Error opening InputStream"));
            }
            stream->opened();
        }
        if (auto started = stream->start(); !started) {
            return std::unexpected(started.error());
        }
        return std::unique_ptr<Stream>(std::move(stream));
    }

    std::expected<std::unique_ptr<Stream>, std::string>
    open_loopback(const LoopbackEndpoint& endpoint, CaptureFn fn, void* user,
                  std::function<void(std::string)> on_lost) override {
        return detail::open_wasapi_loopback(endpoint, fn, user, std::move(on_lost));
    }

    std::expected<std::unique_ptr<Stream>, std::string>
    open_playback(const StreamConfig& config, PlaybackSource& source) override {
        auto stream = std::make_unique<PlaybackStream>(source, config.channels);
        {
            const std::lock_guard lock(portaudio_mutex());
            try {
                ensure_initialized();
            } catch (const std::exception& error) {
                return std::unexpected(std::string(error.what()));
            }
            auto device = resolve_device(config.device_index, false);
            if (!device) {
                return std::unexpected(device.error());
            }
            PaStreamParameters parameters{};
            parameters.device = device->first;
            parameters.channelCount = config.channels;
            parameters.sampleFormat = paInt16;
            parameters.suggestedLatency = device->second->defaultHighOutputLatency;
            const PaError error = Pa_OpenStream(stream->handle(), nullptr, &parameters, config.rate,
                                                paFramesPerBufferUnspecified, paNoFlag,
                                                &PlaybackStream::callback, stream.get());
            if (error != paNoError) {
                return std::unexpected(error_text(error, "Error opening OutputStream"));
            }
            stream->opened();
        }
        if (auto started = stream->start(); !started) {
            return std::unexpected(started.error());
        }
        return std::unique_ptr<Stream>(std::move(stream));
    }

    // A restart that fails twice throws, as refresh_portaudio raises in Python: the Microphones
    // dialog tells "busy" (false) apart from a failed read (an exception).
    bool refresh() override {
        Lifecycle lifecycle;
        return refresh_portaudio(lifecycle);
    }
};

} // namespace

std::shared_ptr<Backend> make_portaudio_backend() {
    // PortAudio is process-wide state, so every caller shares one backend.
    static const std::shared_ptr<Backend> backend = std::make_shared<PortAudioBackend>();
    return backend;
}

} // namespace fv::audio
