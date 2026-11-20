// WASAPI loopback capture of render endpoints. The only file that talks to the Core Audio APIs:
// every COM call runs on a thread this file starts, in the multithreaded apartment, so a caller's
// apartment (Qt's STA on the GUI thread, say) never matters.
#include "audio/win/wasapi_loopback.h"

#include <windows.h>

// WIN32_LEAN_AND_MEAN leaves COM and the multimedia types out of windows.h.
#include <mmsystem.h>
#include <ole2.h>

#include <Audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wil/com.h>
#include <wil/resource.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <format>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>

#include "audio/detail/loopback_pump.h"
#include "audio/detail/portaudio_platform.h"

namespace fv::audio::detail {

namespace {

// soundcard_stream.py's _START_TIMEOUT_S: the open of an endpoint is given as long as
// SoundcardLoopbackSource.start() gives its recorder thread.
constexpr std::chrono::milliseconds kOpenTimeout{4000};

// The audio engine's buffer, in 100 ns units: headroom for a late loopback thread. It adds no
// latency, since every packet is read as soon as the engine signals it.
constexpr REFERENCE_TIME kBufferDuration = 2'000'000; // 200 ms

// PKEY_Device_FriendlyName, spelled out so that no GUID definitions have to be linked.
constexpr PROPERTYKEY kFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT are this GUID with the format tag as Data1.
constexpr GUID kSubtypeBase = {
    0x00000000, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

std::string narrow(std::wstring_view wide) {
    if (wide.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()),
                                         nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view text) {
    if (text.empty()) {
        return {};
    }
    const int size =
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

// "<text> (0x88890004)". The AUDCLNT_E_* codes have no system message, so they go by name.
std::string hresult_text(HRESULT hr) {
    std::string text;
    switch (hr) {
    case AUDCLNT_E_DEVICE_INVALIDATED:
        text = "The audio endpoint was removed or disabled (AUDCLNT_E_DEVICE_INVALIDATED)";
        break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        text = "The Windows audio service is not running (AUDCLNT_E_SERVICE_NOT_RUNNING)";
        break;
    case AUDCLNT_E_DEVICE_IN_USE:
        text = "The audio endpoint is in use (AUDCLNT_E_DEVICE_IN_USE)";
        break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        text = "The audio engine does not support the format (AUDCLNT_E_UNSUPPORTED_FORMAT)";
        break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        text = "The audio endpoint could not be created (AUDCLNT_E_ENDPOINT_CREATE_FAILED)";
        break;
    case AUDCLNT_E_RESOURCES_INVALIDATED:
        text = "The audio resources were invalidated (AUDCLNT_E_RESOURCES_INVALIDATED)";
        break;
    default: {
        wil::unique_hlocal_string message;
        const DWORD length =
            FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                               FORMAT_MESSAGE_IGNORE_INSERTS,
                           nullptr, static_cast<DWORD>(hr), 0,
                           reinterpret_cast<LPWSTR>(message.put()), 0, nullptr);
        if (length > 0 && message) {
            text = narrow(std::wstring_view(message.get(), length));
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r' ||
                                     text.back() == ' ' || text.back() == '.')) {
                text.pop_back();
            }
        }
        if (text.empty()) {
            text = "Unknown error";
        }
        break;
    }
    }
    return std::format("{} (0x{:08X})", text, static_cast<std::uint32_t>(hr));
}

double qpc_seconds() {
    static const double frequency = [] {
        LARGE_INTEGER value{};
        QueryPerformanceFrequency(&value);
        return static_cast<double>(value.QuadPart);
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) / frequency;
}

MixFormat describe(const WAVEFORMATEX& format) {
    MixFormat mix;
    mix.rate = static_cast<int>(format.nSamplesPerSec);
    mix.channels = format.nChannels;
    mix.bits_per_sample = format.wBitsPerSample;
    mix.valid_bits = format.wBitsPerSample;
    if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
        mix.kind = MixFormat::Kind::Float;
    } else if (format.wFormatTag == WAVE_FORMAT_PCM) {
        mix.kind = MixFormat::Kind::Pcm;
    } else if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
               format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        mix.valid_bits = extensible.Samples.wValidBitsPerSample;
        GUID base = extensible.SubFormat;
        const unsigned long tag = base.Data1;
        base.Data1 = 0;
        if (base == kSubtypeBase && tag == static_cast<unsigned long>(WAVE_FORMAT_IEEE_FLOAT)) {
            mix.kind = MixFormat::Kind::Float;
        } else if (base == kSubtypeBase && tag == static_cast<unsigned long>(WAVE_FORMAT_PCM)) {
            mix.kind = MixFormat::Kind::Pcm;
        }
    }
    return mix;
}

// float32 at the endpoint's rate and channels, for the audio engine to convert the mix into.
WAVEFORMATEXTENSIBLE float_format(int rate, int channels, const WAVEFORMATEX& mix) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec = static_cast<DWORD>(rate);
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = static_cast<WORD>(channels * 4);
    format.Format.nAvgBytesPerSec = static_cast<DWORD>(rate) * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.SubFormat = kSubtypeBase;
    format.SubFormat.Data1 = static_cast<unsigned long>(WAVE_FORMAT_IEEE_FLOAT);
    if (mix.nChannels == channels && mix.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        format.dwChannelMask = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(mix).dwChannelMask;
    } else if (channels == 1) {
        format.dwChannelMask = SPEAKER_FRONT_CENTER;
    } else if (channels == 2) {
        format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    }
    return format;
}

class ComApartment {
public:
    ComApartment() : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComApartment() {
        if (SUCCEEDED(result_)) {
            CoUninitialize();
        }
    }
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    HRESULT result() const { return result_; }

private:
    HRESULT result_;
};

std::expected<wil::com_ptr_nothrow<IMMDeviceEnumerator>, HRESULT> make_enumerator() {
    wil::com_ptr_nothrow<IMMDeviceEnumerator> enumerator;
    const HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(enumerator.put()));
    if (FAILED(hr)) {
        return std::unexpected(hr);
    }
    return enumerator;
}

std::string endpoint_id(IMMDevice& device) {
    wil::unique_cotaskmem_string id;
    if (FAILED(device.GetId(id.put())) || !id) {
        return {};
    }
    return narrow(id.get());
}

std::string friendly_name(IMMDevice& device) {
    wil::com_ptr_nothrow<IPropertyStore> store;
    if (FAILED(device.OpenPropertyStore(STGM_READ, store.put()))) {
        return {};
    }
    PROPVARIANT value;
    PropVariantInit(&value);
    std::string name;
    if (SUCCEEDED(store->GetValue(kFriendlyName, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr) {
        name = narrow(value.pwszVal);
    }
    PropVariantClear(&value);
    return name;
}

std::expected<std::vector<LoopbackEndpoint>, std::string> list_endpoints() {
    const ComApartment apartment;
    if (FAILED(apartment.result())) {
        return std::unexpected(hresult_text(apartment.result()));
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::unexpected(hresult_text(enumerator.error()));
    }
    std::string default_id;
    wil::com_ptr_nothrow<IMMDevice> fallback;
    // E_NOTFOUND when there is no render endpoint at all: no default then.
    if (SUCCEEDED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, fallback.put())) &&
        fallback) {
        default_id = endpoint_id(*fallback);
    }
    wil::com_ptr_nothrow<IMMDeviceCollection> collection;
    HRESULT hr = (*enumerator)->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put());
    if (FAILED(hr)) {
        return std::unexpected(hresult_text(hr));
    }
    UINT count = 0;
    hr = collection->GetCount(&count);
    if (FAILED(hr)) {
        return std::unexpected(hresult_text(hr));
    }
    std::vector<LoopbackEndpoint> endpoints;
    for (UINT i = 0; i < count; ++i) {
        wil::com_ptr_nothrow<IMMDevice> device;
        if (FAILED(collection->Item(i, device.put())) || !device) {
            continue; // removed while listing
        }
        LoopbackEndpoint endpoint;
        endpoint.id = endpoint_id(*device);
        if (endpoint.id.empty()) {
            continue;
        }
        endpoint.name = friendly_name(*device);
        endpoint.is_default = endpoint.id == default_id;
        // An endpoint whose mix format cannot be read keeps the 48 kHz stereo defaults, which
        // the audio engine converts to when it opens.
        wil::com_ptr_nothrow<IAudioClient> client;
        if (SUCCEEDED(
                device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void()))) {
            wil::unique_cotaskmem_ptr<WAVEFORMATEX> mix;
            if (SUCCEEDED(client->GetMixFormat(wil::out_param(mix))) && mix &&
                mix->nSamplesPerSec > 0 && mix->nChannels > 0) {
                endpoint.rate = static_cast<int>(mix->nSamplesPerSec);
                endpoint.channels = mix->nChannels;
            }
        }
        endpoints.push_back(std::move(endpoint));
    }
    return endpoints;
}

std::optional<std::string> default_endpoint_id() {
    const ComApartment apartment;
    if (FAILED(apartment.result())) {
        return std::nullopt;
    }
    auto enumerator = make_enumerator();
    if (!enumerator) {
        return std::nullopt;
    }
    wil::com_ptr_nothrow<IMMDevice> device;
    // E_NOTFOUND when there is no render endpoint at all.
    if (FAILED((*enumerator)->GetDefaultAudioEndpoint(eRender, eConsole, device.put())) ||
        !device) {
        return std::nullopt;
    }
    std::string id = endpoint_id(*device);
    if (id.empty()) {
        return std::nullopt;
    }
    return id;
}

// What the stream and its thread share; the thread holds it too, so a stream destroyed from its
// own on_lost leaves nothing the thread still reads.
struct Shared {
    CaptureFn fn = nullptr;
    void* user = nullptr;
    std::function<void(std::string)> on_lost;
    std::atomic<bool> closing{false};
    std::atomic<bool> running{false};
    wil::unique_event_nothrow stop;

    enum class Phase { Pending, Opened, Failed, Abandoned };
    std::mutex mutex;
    std::condition_variable settled;
    Phase phase = Phase::Pending;
    std::string error;

    void fail(std::string message) {
        const std::lock_guard lock(mutex);
        if (phase == Phase::Pending) {
            phase = Phase::Failed;
            error = std::move(message);
        }
        settled.notify_all();
    }

    // false when the opener gave up waiting: the stream is then stopped unseen.
    bool opened() {
        const std::lock_guard lock(mutex);
        if (phase != Phase::Pending) {
            return false;
        }
        phase = Phase::Opened;
        running.store(true);
        settled.notify_all();
        return true;
    }
};

// Opens, then reads packets until stopped. Returns why the stream ended by itself, if it did.
std::optional<std::string> capture(Shared& shared, const LoopbackEndpoint& endpoint) {
    const std::string label = std::format("Error opening loopback endpoint '{}'",
                                          endpoint.name.empty() ? endpoint.id : endpoint.name);
    auto enumerator = make_enumerator();
    if (!enumerator) {
        shared.fail(std::format("{}: {}", label, hresult_text(enumerator.error())));
        return std::nullopt;
    }
    wil::com_ptr_nothrow<IMMDevice> device;
    const std::wstring id = widen(endpoint.id);
    HRESULT hr = (*enumerator)->GetDevice(id.c_str(), device.put());
    if (FAILED(hr) || !device) {
        shared.fail(std::format("Error querying loopback endpoint '{}': {}", endpoint.id,
                                hresult_text(FAILED(hr) ? hr : E_POINTER)));
        return std::nullopt;
    }
    wil::com_ptr_nothrow<IAudioClient> client;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
    if (FAILED(hr)) {
        shared.fail(std::format("{}: {}", label, hresult_text(hr)));
        return std::nullopt;
    }
    wil::unique_cotaskmem_ptr<WAVEFORMATEX> mix;
    hr = client->GetMixFormat(wil::out_param(mix));
    if (FAILED(hr) || !mix) {
        shared.fail(std::format("{}: {}", label, hresult_text(FAILED(hr) ? hr : E_POINTER)));
        return std::nullopt;
    }
    if (endpoint.rate <= 0 || endpoint.channels <= 0) {
        shared.fail(std::format("{}: {} Hz with {} channels is not a format", label, endpoint.rate,
                                endpoint.channels));
        return std::nullopt;
    }
    const LoopbackFormatChoice choice =
        choose_loopback_format(describe(*mix), endpoint.rate, endpoint.channels);
    const WAVEFORMATEXTENSIBLE converted = float_format(endpoint.rate, endpoint.channels, *mix);
    const WAVEFORMATEX* format =
        choice.convert ? reinterpret_cast<const WAVEFORMATEX*>(&converted) : mix.get();
    DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    if (choice.convert) {
        flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    }
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, format, nullptr);
    wil::unique_event_nothrow ready;
    const bool polling = FAILED(hr) || !ready.try_create(wil::EventOptions::None, nullptr) ||
                         FAILED(client->SetEventHandle(ready.get()));
    if (polling) {
        // Event-driven loopback needs Windows 10 1703: older systems refuse the flag or the
        // event handle, so the stream polls instead. That takes a fresh client, since an
        // initialized one cannot be initialized again and one with the event flag but no handle
        // does not start.
        client.reset();
        HRESULT plain =
            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.put_void());
        if (SUCCEEDED(plain)) {
            plain = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                       flags & ~DWORD{AUDCLNT_STREAMFLAGS_EVENTCALLBACK},
                                       kBufferDuration, 0, format, nullptr);
        }
        if (FAILED(plain)) {
            shared.fail(std::format("{}: {}", label, hresult_text(FAILED(hr) ? hr : plain)));
            return std::nullopt;
        }
    }
    wil::com_ptr_nothrow<IAudioCaptureClient> reader;
    hr = client->GetService(__uuidof(IAudioCaptureClient), reader.put_void());
    if (FAILED(hr)) {
        shared.fail(std::format("{}: {}", label, hresult_text(hr)));
        return std::nullopt;
    }
    // The one allocation of the stream, before the first packet.
    LoopbackPump pump(endpoint.rate, endpoint.channels, choice.format, shared.fn, shared.user);
    hr = client->Start();
    if (FAILED(hr)) {
        shared.fail(std::format("Error starting loopback endpoint '{}': {}",
                                endpoint.name.empty() ? endpoint.id : endpoint.name,
                                hresult_text(hr)));
        return std::nullopt;
    }
    if (!shared.opened()) {
        client->Stop();
        return std::nullopt;
    }

    const auto ended = [&](HRESULT failure) {
        return std::optional<std::string>(std::format(
            "Loopback capture of '{}' ended: {}",
            endpoint.name.empty() ? endpoint.id : endpoint.name, hresult_text(failure)));
    };
    const HANDLE waits[2] = {shared.stop.get(), ready.get()};
    pump.start(qpc_seconds());
    std::optional<std::string> lost;
    while (!shared.closing.load()) {
        const DWORD woke = polling
                               ? WaitForSingleObject(shared.stop.get(), kLoopbackPollMs)
                               : WaitForMultipleObjects(2, waits, FALSE, kLoopbackSilenceAfterMs);
        if (woke == WAIT_OBJECT_0 || shared.closing.load()) {
            break;
        }
        if (woke == WAIT_FAILED) {
            lost = ended(HRESULT_FROM_WIN32(GetLastError()));
            break;
        }
        bool got = false;
        while (!shared.closing.load()) {
            UINT32 next = 0;
            hr = reader->GetNextPacketSize(&next);
            if (FAILED(hr)) {
                lost = ended(hr);
                break;
            }
            if (next == 0) {
                break;
            }
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD status = 0;
            UINT64 qpc_position = 0;
            hr = reader->GetBuffer(&data, &frames, &status, nullptr, &qpc_position);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                break;
            }
            if (FAILED(hr)) {
                lost = ended(hr);
                break;
            }
            pump.read({.data = data,
                       .frames = frames,
                       .silent = (status & AUDCLNT_BUFFERFLAGS_SILENT) != 0,
                       .discontinuity = (status & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0,
                       .timestamp_error = (status & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0,
                       .qpc_position = qpc_position},
                      qpc_seconds());
            hr = reader->ReleaseBuffer(frames);
            if (FAILED(hr)) {
                lost = ended(hr);
                break;
            }
            got = true;
        }
        if (lost) {
            break;
        }
        if (!got && !shared.closing.load()) {
            pump.idle(qpc_seconds());
        }
    }
    client->Stop();
    shared.running.store(false);
    return lost;
}

void run(std::shared_ptr<Shared> shared, LoopbackEndpoint endpoint) {
    name_this_thread("fv-loopback");
    std::optional<std::string> lost;
    {
        const ComApartment apartment;
        if (FAILED(apartment.result())) {
            shared->fail(std::format("Error opening loopback endpoint '{}': {}",
                                     endpoint.name.empty() ? endpoint.id : endpoint.name,
                                     hresult_text(apartment.result())));
            return;
        }
        lost = capture(*shared, endpoint);
    }
    shared->running.store(false);
    // closing is set by abort() and destruction, so a stream its owner stopped never reports
    // a loss, even one that raced the stop.
    if (lost && !shared->closing.exchange(true) && shared->on_lost) {
        shared->on_lost(std::move(*lost));
    }
}

class WasapiLoopbackStream final : public Stream {
public:
    WasapiLoopbackStream(std::shared_ptr<Shared> shared, std::thread thread)
        : shared_(std::move(shared)), thread_(std::move(thread)) {}
    ~WasapiLoopbackStream() override {
        abort();
        // Still joinable only when destroyed on fv-loopback itself, inside on_lost: that thread
        // returns right after, holding its own reference to what it reads.
        if (thread_.joinable()) {
            thread_.detach();
        }
    }
    WasapiLoopbackStream(const WasapiLoopbackStream&) = delete;
    WasapiLoopbackStream& operator=(const WasapiLoopbackStream&) = delete;

    bool active() const override { return shared_->running.load() && !shared_->closing.load(); }

    void abort() override {
        shared_->closing.store(true);
        shared_->stop.SetEvent();
        const std::lock_guard lock(join_mutex_);
        if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
            thread_.join();
        }
    }

private:
    std::shared_ptr<Shared> shared_;
    std::mutex join_mutex_;
    std::thread thread_;
};

} // namespace

std::vector<LoopbackEndpoint> wasapi_loopbacks() {
    std::expected<std::vector<LoopbackEndpoint>, std::string> listed;
    try {
        std::thread worker([&listed] {
            name_this_thread("fv-loopback");
            listed = list_endpoints();
        });
        worker.join();
    } catch (const std::system_error& error) {
        listed = std::unexpected(std::string(error.what()));
    }
    if (!listed) {
        throw std::runtime_error("Error listing loopback endpoints: " + listed.error());
    }
    return std::move(*listed);
}

std::optional<std::string> wasapi_default_loopback_id() {
    std::optional<std::string> id;
    try {
        std::thread worker([&id] {
            name_this_thread("fv-loopback");
            try {
                id = default_endpoint_id();
            } catch (const std::exception&) {
                id.reset(); // an allocation that failed: no answer
            }
        });
        worker.join();
    } catch (const std::system_error&) {
        return std::nullopt;
    }
    return id;
}

std::expected<std::unique_ptr<Stream>, std::string>
open_wasapi_loopback(const LoopbackEndpoint& endpoint, CaptureFn fn, void* user,
                     std::function<void(std::string)> on_lost) {
    auto shared = std::make_shared<Shared>();
    shared->fn = fn;
    shared->user = user;
    shared->on_lost = std::move(on_lost);
    if (!shared->stop.try_create(wil::EventOptions::ManualReset, nullptr)) {
        return std::unexpected(std::format("Error opening loopback endpoint '{}': {}",
                                           endpoint.name.empty() ? endpoint.id : endpoint.name,
                                           hresult_text(HRESULT_FROM_WIN32(GetLastError()))));
    }
    std::thread thread;
    try {
        thread = std::thread(run, shared, endpoint);
    } catch (const std::system_error& error) {
        return std::unexpected(std::string(error.what()));
    }
    std::unique_lock lock(shared->mutex);
    if (!shared->settled.wait_for(lock, kOpenTimeout,
                                  [&] { return shared->phase != Shared::Phase::Pending; })) {
        shared->phase = Shared::Phase::Abandoned;
        lock.unlock();
        shared->closing.store(true);
        shared->stop.SetEvent();
        // The thread ends by itself once the call it is stuck in returns.
        thread.detach();
        return std::unexpected(std::format("Loopback endpoint '{}' did not open within 4.0 s",
                                           endpoint.name.empty() ? endpoint.id : endpoint.name));
    }
    if (shared->phase == Shared::Phase::Failed) {
        std::string error = shared->error;
        lock.unlock();
        thread.join();
        return std::unexpected(std::move(error));
    }
    lock.unlock();
    return std::unique_ptr<Stream>(
        std::make_unique<WasapiLoopbackStream>(std::move(shared), std::move(thread)));
}

} // namespace fv::audio::detail
