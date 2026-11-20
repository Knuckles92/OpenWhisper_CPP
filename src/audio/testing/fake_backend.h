#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "audio/backend.h"
#include "base/json.h"

// audio::Backend without devices, for tests: device lists from tests/fixtures/audio/devices-*.json
// (PortAudio's own query_devices/query_hostapis shapes, as sounddevice reports them), scripted
// format refusals, open and start failures, capture and loopback blocks delivered by the test,
// device loss, scripted render endpoints, and playback streams the test pulls like a device would.
namespace fv::audio::testing {

struct FakeHostApi {
    std::string name;
    int default_input_device = -1;
    int default_output_device = -1;
};

struct FakeDevice {
    std::string name;
    int hostapi = 0; // index into the host APIs
    int max_input_channels = 0;
    int max_output_channels = 0;
    double default_samplerate = 44100.0;
};

class FakeBackend;

// One stream the fake opened. The test plays the device: pull() for playback, deliver() and
// lose() for capture and loopback, each on the calling thread.
// Affinity: thread-safe.
class FakeStream {
public:
    enum class Kind { Capture, Playback, Loopback };

    Kind kind() const { return kind_; }
    // A loopback stream's config is the endpoint's rate and channels with blocks of 1024 frames
    // (device_index -1).
    const StreamConfig& config() const { return config_; }
    const LoopbackEndpoint& endpoint() const { return endpoint_; } // loopback only
    bool started() const { return started_.load(); }
    bool aborted() const { return aborted_.load(); }
    bool closed() const { return closed_.load(); }
    int finished() const { return finished_.load(); }

    // Playback: one device callback of `frames`, its buffer first filled with 99 (sounddevice's
    // fake does the same); true while the stream keeps going. The stream ends, and its source
    // hears finished(), when the source gives fewer frames than asked.
    bool pull(std::size_t frames);
    // What the last pull() handed to the device.
    std::vector<std::int16_t> last() const;

    // Capture and loopback: one block of `frames` interleaved samples to the CaptureFn. A
    // loopback block of more than 1024 frames throws std::logic_error, as WASAPI's never has.
    void deliver(std::span<const std::int16_t> frames, BlockStatus status = {});
    // Capture and loopback: `pcm` (interleaved, read_pcm of a WAV, say) in blocks of the stream's
    // frames_per_block, as the device thread would hand them over; a last partial block stays
    // undelivered, since PortAudio only ever passes whole blocks. Returns the frames delivered.
    std::size_t deliver_blocks(std::span<const std::int16_t> pcm, BlockStatus status = {});
    // Capture and loopback: the device went away; on_lost runs unless the owner aborted or
    // closed it.
    void lose(const std::string& reason = {});

private:
    friend class FakeBackend;
    friend class FakeStreamHandle;

    void end(); // finished_ += 1 and the playback source's finished()

    Kind kind_ = Kind::Capture;
    StreamConfig config_{.device_index = -1};
    LoopbackEndpoint endpoint_;
    CaptureFn capture_ = nullptr;
    void* user_ = nullptr;
    std::function<void(std::string)> on_lost_;
    PlaybackSource* source_ = nullptr;
    std::atomic<bool> started_{false};
    std::atomic<bool> aborted_{false};
    std::atomic<bool> closed_{false};
    std::atomic<int> finished_{0};
    mutable std::mutex mutex_;
    std::vector<std::int16_t> last_;
    std::weak_ptr<FakeBackend> backend_;
};

// Affinity: thread-safe.
class FakeBackend final : public Backend, public std::enable_shared_from_this<FakeBackend> {
public:
    FakeBackend() = default;

    // A dumped device list: {"hostapis": [{"name", "default_input_device",
    // "default_output_device"}], "devices": [{"name", "hostapi", "max_input_channels",
    // "max_output_channels", "default_samplerate"}], "default": {"input", "output", "hostapi"},
    // "refuse": [device indexes whose format check fails]}.
    static std::shared_ptr<FakeBackend> from_fixture(const std::filesystem::path& file);
    void load(const Json& dump);
    void set_layout(std::vector<FakeHostApi> hostapis, std::vector<FakeDevice> devices,
                    int default_input, int default_hostapi = 0, int default_output = -1);
    // Keeps only the first `count` devices (a microphone unplugged).
    void keep_devices(std::size_t count);
    std::vector<FakeDevice> devices() const;

    // Format checks: a refused index fails "Invalid sample rate [PaErrorCode -9997]", as does a
    // WASAPI device checked without auto-convert while that rule is on (it is by default).
    void refuse_format(int device_index);
    void set_wasapi_requires_auto_convert(bool required);
    std::vector<StreamConfig> checks() const;
    // inputs() and outputs() throw std::runtime_error(message) while set.
    void fail_listing(std::optional<std::string> message);
    int listings() const; // inputs() and outputs() calls

    // Capture opens of `device_index` (-1: the default device) fail with `message`.
    void fail_open(int device_index, std::string message);
    void fail_playback_open(std::optional<std::string> message);
    // The playback stream is created, then start() fails and it is closed.
    void fail_playback_start(std::optional<std::string> message);

    // Render endpoints for loopbacks() and open_loopback(); none by default.
    void set_loopbacks(std::vector<LoopbackEndpoint> endpoints);
    // Loopback opens of `endpoint_id` fail with `message`. Opening an id that is not among the
    // endpoints fails as WASAPI's lookup does: "Error querying loopback endpoint '<id>': Element
    // not found (0x80070490)".
    // With `channels` above 0, only opens asking for that many channels fail.
    void fail_loopback_open(std::string endpoint_id, std::string message, int channels = 0);
    // loopbacks() throws std::runtime_error(message) while set.
    void fail_loopback_listing(std::optional<std::string> message);
    int loopback_listings() const;        // loopbacks() calls
    int default_loopback_queries() const; // default_loopback_id() calls

    // refresh() throws std::runtime_error(message) while set and no stream is open (the audio
    // system did not restart), after recording "terminate". Loopback streams are not PortAudio
    // streams and do not hold a refresh off.
    void fail_refresh(std::optional<std::string> message);

    std::vector<std::shared_ptr<FakeStream>> streams() const;
    std::size_t open_streams() const;
    // "terminate" and "initialize" for each refresh that ran.
    std::vector<std::string> lifecycle() const;

    std::vector<DeviceInfo> inputs() override;
    std::vector<DeviceInfo> outputs() override;
    std::vector<LoopbackEndpoint> loopbacks() override;
    // The endpoint marked is_default, read without listing (as GetDefaultAudioEndpoint does).
    std::optional<std::string> default_loopback_id() override;
    std::expected<void, std::string> check_input(const StreamConfig& config) override;
    std::expected<std::unique_ptr<Stream>, std::string>
    open_capture(const StreamConfig& config, CaptureFn fn, void* user,
                 std::function<void(std::string)> on_lost) override;
    std::expected<std::unique_ptr<Stream>, std::string>
    open_loopback(const LoopbackEndpoint& endpoint, CaptureFn fn, void* user,
                  std::function<void(std::string)> on_lost) override;
    std::expected<std::unique_ptr<Stream>, std::string>
    open_playback(const StreamConfig& config, PlaybackSource& source) override;
    bool refresh() override;

private:
    friend class FakeStreamHandle;

    std::vector<DeviceInfo> list(bool input);
    int resolve(int device_index, bool input) const;
    void closed(const FakeStream* stream);

    mutable std::mutex mutex_;
    std::vector<FakeHostApi> hostapis_;
    std::vector<FakeDevice> devices_;
    int default_input_ = -1;
    int default_output_ = -1;
    int default_hostapi_ = 0;
    std::set<int> refused_;
    bool wasapi_requires_auto_convert_ = true;
    std::vector<StreamConfig> checks_;
    std::optional<std::string> listing_failure_;
    int listings_ = 0;
    std::vector<std::pair<int, std::string>> open_failures_;
    std::optional<std::string> playback_open_failure_;
    std::optional<std::string> playback_start_failure_;
    std::optional<std::string> refresh_failure_;
    std::vector<LoopbackEndpoint> loopbacks_;
    struct LoopbackOpenFailure {
        std::string endpoint_id;
        std::string message;
        int channels = 0;
    };
    std::vector<LoopbackOpenFailure> loopback_open_failures_;
    int loopback_listings_ = 0;
    int default_loopback_queries_ = 0;
    std::optional<std::string> loopback_listing_failure_;
    std::vector<std::shared_ptr<FakeStream>> streams_;
    std::set<const FakeStream*> open_;
    std::vector<std::string> lifecycle_;
};

// The whole file decoded to interleaved int16 at its own rate and layout (AudioFileDecoder), for
// scripting capture blocks from a WAV.
std::vector<std::int16_t> read_pcm(const std::filesystem::path& file);

} // namespace fv::audio::testing
