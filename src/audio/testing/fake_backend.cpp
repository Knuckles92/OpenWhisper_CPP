#include "audio/testing/fake_backend.h"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>

#include "audio/detail/loopback_pump.h"
#include "audio/devices.h"
#include "audio/file_decoder.h"

namespace fv::audio::testing {

namespace {

constexpr std::string_view kInvalidSampleRate = "Invalid sample rate [PaErrorCode -9997]";

// "Element not found." of E_NOTFOUND, as the WASAPI stream words it.
constexpr std::string_view kEndpointNotFound = "Element not found (0x80070490)";

using detail::kLoopbackBlockFrames;

int json_int(const Json& object, std::string_view key, int fallback) {
    const auto it = object.find(key);
    if (it == object.end() || it->is_null()) {
        return fallback;
    }
    return it->get<int>();
}

} // namespace

// The Stream a caller owns; destroying it closes the fake stream.
class FakeStreamHandle final : public Stream {
public:
    explicit FakeStreamHandle(std::shared_ptr<FakeStream> stream) : stream_(std::move(stream)) {}
    ~FakeStreamHandle() override {
        stream_->closed_.store(true);
        if (const auto backend = stream_->backend_.lock()) {
            backend->closed(stream_.get());
        }
    }
    FakeStreamHandle(const FakeStreamHandle&) = delete;
    FakeStreamHandle& operator=(const FakeStreamHandle&) = delete;

    bool active() const override {
        return stream_->started() && !stream_->aborted() && !stream_->closed() &&
               stream_->finished() == 0;
    }

    void abort() override {
        stream_->aborted_.store(true);
        // A playback stream's finished callback runs inside abort(), as PortAudio's does.
        if (stream_->kind_ == FakeStream::Kind::Playback && stream_->finished() == 0) {
            stream_->end();
        }
    }

private:
    std::shared_ptr<FakeStream> stream_;
};

void FakeStream::end() {
    finished_.fetch_add(1);
    if (source_ != nullptr) {
        source_->finished();
    }
}

bool FakeStream::pull(std::size_t frames) {
    if (kind_ != Kind::Playback || source_ == nullptr) {
        throw std::logic_error("FakeStream::pull is for playback streams");
    }
    const std::size_t samples = frames * static_cast<std::size_t>(config_.channels);
    std::vector<std::int16_t> out(samples, 99);
    const std::size_t written = std::min(source_->read(out), frames);
    const bool keeps_going = written >= frames;
    if (!keeps_going) {
        std::fill(out.begin() + static_cast<std::ptrdiff_t>(written * config_.channels), out.end(),
                  std::int16_t{0});
    }
    {
        const std::lock_guard lock(mutex_);
        last_ = std::move(out);
    }
    if (!keeps_going) {
        end();
    }
    return keeps_going;
}

std::vector<std::int16_t> FakeStream::last() const {
    const std::lock_guard lock(mutex_);
    return last_;
}

void FakeStream::deliver(std::span<const std::int16_t> frames, BlockStatus status) {
    if (kind_ == Kind::Playback || capture_ == nullptr) {
        throw std::logic_error("FakeStream::deliver is for capture and loopback streams");
    }
    const std::size_t count = frames.size() / static_cast<std::size_t>(config_.channels);
    if (kind_ == Kind::Loopback && count > kLoopbackBlockFrames) {
        throw std::logic_error("A loopback block holds at most 1024 frames");
    }
    capture_(user_, frames.data(), count, status);
}

std::size_t FakeStream::deliver_blocks(std::span<const std::int16_t> pcm, BlockStatus status) {
    const auto block = static_cast<std::size_t>(config_.frames_per_block) *
                       static_cast<std::size_t>(config_.channels);
    if (block == 0) {
        throw std::logic_error("FakeStream::deliver_blocks needs a positive block size");
    }
    std::size_t at = 0;
    for (; at + block <= pcm.size(); at += block) {
        deliver(pcm.subspan(at, block), status);
    }
    return at / static_cast<std::size_t>(config_.channels);
}

void FakeStream::lose(const std::string& reason) {
    if (aborted() || closed()) {
        return;
    }
    finished_.fetch_add(1);
    if (on_lost_) {
        on_lost_(reason);
    }
}

std::shared_ptr<FakeBackend> FakeBackend::from_fixture(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Cannot read the device fixture " + file.string());
    }
    auto backend = std::make_shared<FakeBackend>();
    backend->load(Json::parse(in));
    return backend;
}

void FakeBackend::load(const Json& dump) {
    std::vector<FakeHostApi> hostapis;
    for (const Json& api : dump.at("hostapis")) {
        hostapis.push_back(FakeHostApi{api.at("name").get<std::string>(),
                                       json_int(api, "default_input_device", -1),
                                       json_int(api, "default_output_device", -1)});
    }
    std::vector<FakeDevice> devices;
    for (const Json& device : dump.at("devices")) {
        devices.push_back(FakeDevice{
            device.at("name").get<std::string>(), json_int(device, "hostapi", 0),
            json_int(device, "max_input_channels", 0), json_int(device, "max_output_channels", 0),
            device.value("default_samplerate", 44100.0)});
    }
    const Json defaults = dump.value("default", Json::object());
    set_layout(std::move(hostapis), std::move(devices), json_int(defaults, "input", -1),
               json_int(defaults, "hostapi", 0), json_int(defaults, "output", -1));
    if (const auto refuse = dump.find("refuse"); refuse != dump.end()) {
        for (const Json& index : *refuse) {
            refuse_format(index.get<int>());
        }
    }
}

void FakeBackend::set_layout(std::vector<FakeHostApi> hostapis, std::vector<FakeDevice> devices,
                             int default_input, int default_hostapi, int default_output) {
    const std::lock_guard lock(mutex_);
    hostapis_ = std::move(hostapis);
    devices_ = std::move(devices);
    default_input_ = default_input;
    default_hostapi_ = default_hostapi;
    default_output_ = default_output;
}

void FakeBackend::keep_devices(std::size_t count) {
    const std::lock_guard lock(mutex_);
    if (devices_.size() > count) {
        devices_.resize(count);
    }
}

std::vector<FakeDevice> FakeBackend::devices() const {
    const std::lock_guard lock(mutex_);
    return devices_;
}

void FakeBackend::refuse_format(int device_index) {
    const std::lock_guard lock(mutex_);
    refused_.insert(device_index);
}

void FakeBackend::set_wasapi_requires_auto_convert(bool required) {
    const std::lock_guard lock(mutex_);
    wasapi_requires_auto_convert_ = required;
}

std::vector<StreamConfig> FakeBackend::checks() const {
    const std::lock_guard lock(mutex_);
    return checks_;
}

void FakeBackend::fail_listing(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    listing_failure_ = std::move(message);
}

int FakeBackend::listings() const {
    const std::lock_guard lock(mutex_);
    return listings_;
}

void FakeBackend::fail_open(int device_index, std::string message) {
    const std::lock_guard lock(mutex_);
    open_failures_.emplace_back(device_index, std::move(message));
}

void FakeBackend::fail_playback_open(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    playback_open_failure_ = std::move(message);
}

void FakeBackend::fail_playback_start(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    playback_start_failure_ = std::move(message);
}

std::vector<std::shared_ptr<FakeStream>> FakeBackend::streams() const {
    const std::lock_guard lock(mutex_);
    return streams_;
}

std::size_t FakeBackend::open_streams() const {
    const std::lock_guard lock(mutex_);
    return open_.size();
}

std::vector<std::string> FakeBackend::lifecycle() const {
    const std::lock_guard lock(mutex_);
    return lifecycle_;
}

std::vector<DeviceInfo> FakeBackend::list(bool input) {
    const std::lock_guard lock(mutex_);
    ++listings_;
    if (listing_failure_) {
        throw std::runtime_error(*listing_failure_);
    }
    std::vector<DeviceInfo> listed;
    for (std::size_t i = 0; i < devices_.size(); ++i) {
        const FakeDevice& device = devices_[i];
        if ((input ? device.max_input_channels : device.max_output_channels) <= 0) {
            continue;
        }
        const bool known_api =
            device.hostapi >= 0 && static_cast<std::size_t>(device.hostapi) < hostapis_.size();
        DeviceInfo info{.index = static_cast<int>(i),
                        .name = device.name,
                        .host_api = known_api ? hostapis_[device.hostapi].name : std::string(),
                        .max_input_channels = device.max_input_channels,
                        .max_output_channels = device.max_output_channels,
                        .default_rate = device.default_samplerate};
        info.default_api = device.hostapi == default_hostapi_;
        listed.push_back(std::move(info));
    }
    const bool known_default_api =
        default_hostapi_ >= 0 && static_cast<std::size_t>(default_hostapi_) < hostapis_.size();
    int default_index = -1;
    if (input) {
        default_index = default_input_index(
            listed, default_input_,
            known_default_api ? hostapis_[default_hostapi_].default_input_device : -1);
    } else {
        default_index = default_output_ >= 0 ? default_output_
                        : known_default_api  ? hostapis_[default_hostapi_].default_output_device
                                             : -1;
    }
    for (DeviceInfo& info : listed) {
        info.is_default = info.index == default_index;
    }
    return listed;
}

int FakeBackend::resolve(int device_index, bool input) const {
    if (device_index >= 0) {
        return device_index;
    }
    return input ? default_input_ : default_output_;
}

void FakeBackend::closed(const FakeStream* stream) {
    const std::lock_guard lock(mutex_);
    open_.erase(stream);
}

std::vector<DeviceInfo> FakeBackend::inputs() {
    return list(true);
}

std::vector<DeviceInfo> FakeBackend::outputs() {
    return list(false);
}

std::vector<LoopbackEndpoint> FakeBackend::loopbacks() {
    const std::lock_guard lock(mutex_);
    ++loopback_listings_;
    if (loopback_listing_failure_) {
        throw std::runtime_error(*loopback_listing_failure_);
    }
    return loopbacks_;
}

void FakeBackend::set_loopbacks(std::vector<LoopbackEndpoint> endpoints) {
    const std::lock_guard lock(mutex_);
    loopbacks_ = std::move(endpoints);
}

void FakeBackend::fail_loopback_open(std::string endpoint_id, std::string message, int channels) {
    const std::lock_guard lock(mutex_);
    loopback_open_failures_.push_back({std::move(endpoint_id), std::move(message), channels});
}

int FakeBackend::loopback_listings() const {
    const std::lock_guard lock(mutex_);
    return loopback_listings_;
}

int FakeBackend::default_loopback_queries() const {
    const std::lock_guard lock(mutex_);
    return default_loopback_queries_;
}

std::optional<std::string> FakeBackend::default_loopback_id() {
    const std::lock_guard lock(mutex_);
    ++default_loopback_queries_;
    if (loopback_listing_failure_) {
        return std::nullopt;
    }
    for (const LoopbackEndpoint& endpoint : loopbacks_) {
        if (endpoint.is_default) {
            return endpoint.id;
        }
    }
    return std::nullopt;
}

void FakeBackend::fail_loopback_listing(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    loopback_listing_failure_ = std::move(message);
}

std::expected<void, std::string> FakeBackend::check_input(const StreamConfig& config) {
    const std::lock_guard lock(mutex_);
    checks_.push_back(config);
    const int index = resolve(config.device_index, true);
    if (index < 0 || static_cast<std::size_t>(index) >= devices_.size()) {
        return std::unexpected("Error querying device " + std::to_string(index));
    }
    const FakeDevice& device = devices_[static_cast<std::size_t>(index)];
    const bool wasapi = device.hostapi >= 0 &&
                        static_cast<std::size_t>(device.hostapi) < hostapis_.size() &&
                        hostapis_[device.hostapi].name == "Windows WASAPI";
    if (refused_.contains(index) ||
        (wasapi && wasapi_requires_auto_convert_ && !config.wasapi_auto_convert)) {
        return std::unexpected(std::string(kInvalidSampleRate));
    }
    return {};
}

std::expected<std::unique_ptr<Stream>, std::string>
FakeBackend::open_capture(const StreamConfig& config, CaptureFn fn, void* user,
                          std::function<void(std::string)> on_lost) {
    auto stream = std::make_shared<FakeStream>();
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [index, message] : open_failures_) {
            if (index == config.device_index) {
                return std::unexpected(message);
            }
        }
        // An unplugged device (keep_devices) no longer opens, as PortAudio's query fails.
        const int index = resolve(config.device_index, true);
        if (index < 0 || static_cast<std::size_t>(index) >= devices_.size()) {
            return std::unexpected("Error querying device " + std::to_string(index));
        }
        stream->kind_ = FakeStream::Kind::Capture;
        stream->config_ = config;
        stream->capture_ = fn;
        stream->user_ = user;
        stream->on_lost_ = std::move(on_lost);
        stream->backend_ = weak_from_this();
        stream->started_.store(true);
        streams_.push_back(stream);
        open_.insert(stream.get());
    }
    return std::unique_ptr<Stream>(std::make_unique<FakeStreamHandle>(std::move(stream)));
}

std::expected<std::unique_ptr<Stream>, std::string>
FakeBackend::open_loopback(const LoopbackEndpoint& endpoint, CaptureFn fn, void* user,
                           std::function<void(std::string)> on_lost) {
    auto stream = std::make_shared<FakeStream>();
    {
        const std::lock_guard lock(mutex_);
        for (const LoopbackOpenFailure& failure : loopback_open_failures_) {
            if (failure.endpoint_id == endpoint.id &&
                (failure.channels <= 0 || failure.channels == endpoint.channels)) {
                return std::unexpected(failure.message);
            }
        }
        const bool listed = std::ranges::any_of(
            loopbacks_, [&](const LoopbackEndpoint& known) { return known.id == endpoint.id; });
        if (!listed) {
            return std::unexpected(std::format("Error querying loopback endpoint '{}': {}",
                                               endpoint.id, kEndpointNotFound));
        }
        stream->kind_ = FakeStream::Kind::Loopback;
        stream->config_ = StreamConfig{.device_index = -1,
                                       .rate = endpoint.rate,
                                       .channels = endpoint.channels,
                                       .frames_per_block = static_cast<int>(kLoopbackBlockFrames)};
        stream->endpoint_ = endpoint;
        stream->capture_ = fn;
        stream->user_ = user;
        stream->on_lost_ = std::move(on_lost);
        stream->backend_ = weak_from_this();
        stream->started_.store(true);
        streams_.push_back(stream);
        open_.insert(stream.get());
    }
    return std::unique_ptr<Stream>(std::make_unique<FakeStreamHandle>(std::move(stream)));
}

std::expected<std::unique_ptr<Stream>, std::string>
FakeBackend::open_playback(const StreamConfig& config, PlaybackSource& source) {
    auto stream = std::make_shared<FakeStream>();
    std::optional<std::string> start_failure;
    {
        const std::lock_guard lock(mutex_);
        if (playback_open_failure_) {
            return std::unexpected(*playback_open_failure_);
        }
        stream->kind_ = FakeStream::Kind::Playback;
        stream->config_ = config;
        stream->source_ = &source;
        stream->backend_ = weak_from_this();
        streams_.push_back(stream);
        open_.insert(stream.get());
        start_failure = playback_start_failure_;
    }
    auto handle = std::make_unique<FakeStreamHandle>(stream);
    if (start_failure) {
        handle.reset(); // closed, as the player closes a stream that would not start
        return std::unexpected(*start_failure);
    }
    stream->started_.store(true);
    return std::unique_ptr<Stream>(std::move(handle));
}

void FakeBackend::fail_refresh(std::optional<std::string> message) {
    const std::lock_guard lock(mutex_);
    refresh_failure_ = std::move(message);
}

bool FakeBackend::refresh() {
    const std::lock_guard lock(mutex_);
    if (std::ranges::any_of(open_, [](const FakeStream* stream) {
            return stream->kind() != FakeStream::Kind::Loopback;
        })) {
        return false;
    }
    lifecycle_.emplace_back("terminate");
    if (refresh_failure_) {
        throw std::runtime_error(*refresh_failure_);
    }
    lifecycle_.emplace_back("initialize");
    return true;
}

std::vector<std::int16_t> read_pcm(const std::filesystem::path& file) {
    AudioFileDecoder decoder(file);
    std::vector<std::int16_t> samples;
    while (decoder.read_s16(samples)) {
    }
    return samples;
}

} // namespace fv::audio::testing
