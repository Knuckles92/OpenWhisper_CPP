#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "audio/backend.h"
#include "audio/devices.h"
#include "base/json.h"
#include "meeting_capture/capture_block.h"
#include "meeting_capture/failure_log.h"
#include "meeting_capture/health.h"
#include "meeting_capture/meeting_clock.h"
#include "meeting_capture/sources.h"
#include "meeting_capture/spool.h"
#include "meeting_state/repository.h"
#include "meeting_state/types.h"

// meeting/capture_session.py MeetingCapture: one meeting's live audio capture. It owns the
// microphone and system-audio sources, their per-channel spools and signal monitors, microphone
// selection and failover, and the watchdog that restarts failed sources and follows the default
// devices, and it reports capture health into the meeting's state.
namespace fv::meeting {

// Poll often enough that failure detection plus a loopback's first-block wait (4 s) fit the 12 s
// restoration bound.
inline constexpr double CAPTURE_WATCHDOG_INTERVAL_S = 1.0;
inline constexpr double CAPTURE_RETRY_INTERVAL_S = 3.0;
// The worst case from a failed source until a restart attempt must begin.
inline constexpr double CAPTURE_RECOVERY_BUDGET_S = 12.0;
// Microphones one open tries before waiting for the next watchdog pass.
inline constexpr int MIC_OPEN_ATTEMPTS = 4;

// The meeting state's capture field, as the engine's store holds it (implemented by the engine).
// Affinity: called from the capture's start, end and watchdog threads; implementations are
// thread-safe.
class CaptureStatusSink {
public:
    virtual ~CaptureStatusSink() = default;
    // state.capture; null while the meeting has no store, which makes update_status a no-op.
    virtual Json capture_state() = 0;
    // store.update_runtime_fields(capture=...): true when it was written.
    virtual bool update_capture(const Json& capture) = 0;
    // emit_status(note=...); an empty note is Python's None.
    virtual void emit_status(std::string note) = 0;
    // emit("error", {"code", "message"}).
    virtual void emit_error(std::string code, std::string message) = 0;
};

struct CaptureOptions {
    std::string system_audio_policy = "auto"; // auto | required | disabled
    std::vector<audio::DeviceKey> mic_priority;
    std::optional<int> mic_device_id; // the legacy device index, used without a priority
};

// How sources are made; the default opens MicSource and LoopbackSource over the backend. The
// engine's tests give fakes (testing/fake_capture_source.h).
struct SourceFactory {
    std::function<std::shared_ptr<CaptureSource>(const audio::InputDevice&)> mic;
    std::function<std::shared_ptr<CaptureSource>(const audio::LoopbackEndpoint&)> loopback;
};

struct CaptureTiming {
    double watchdog_interval_s = CAPTURE_WATCHDOG_INTERVAL_S;
    double retry_interval_s = CAPTURE_RETRY_INTERVAL_S;
};

// Affinity: start, stop and flush on the engine's start and end workers, one at a time; the
// watchdog runs on its own thread "fv-meeting-watchdog"; blocks reach the router on the
// sources' device threads. `recording` and `preview_feed` run on device threads, so they must
// not block, allocate or throw (a throwing preview feed is caught and logged later). The
// sink, the clock and the functions must outlive the capture.
class MeetingCapture {
public:
    MeetingCapture(CaptureOptions options, MeetingClock& clock,
                   std::shared_ptr<meeting_state::MeetingRepository> repository,
                   std::shared_ptr<audio::Backend> backend, CaptureStatusSink& sink,
                   std::function<bool()> recording,
                   std::function<void(const meeting_state::SpooledChunk&)> on_chunk,
                   std::function<void(const CaptureBlock&)> preview_feed);
    // Stops the watchdog and the sources; the spools finish as they are destroyed.
    ~MeetingCapture();
    MeetingCapture(const MeetingCapture&) = delete;
    MeetingCapture& operator=(const MeetingCapture&) = delete;

    // Before start(): how sources are made, the watchdog's pacing, and what flush() calls first
    // (the live preview's stop; Python's asr.stop_preview).
    void set_source_factory(SourceFactory factory);
    void set_timing(CaptureTiming timing);
    void set_stop_preview(std::function<void()> stop_preview);

    // Opens the microphone and system audio and starts the watchdog. Every source is checked with
    // is_active() after it starts: a source that opened nothing must not pass for a silent room.
    // Returns the user-facing note when a channel is unavailable. Throws std::runtime_error "No
    // audio devices could be opened." when neither opened, and "LINUX_SYSTEM_AUDIO_REQUIRED:
    // System-audio capture is required but unavailable." when the policy requires system audio.
    std::optional<std::string> start(const std::string& meeting_id,
                                     const std::filesystem::path& spool_dir);
    // Stops the watchdog, then every source; the spools stay for flush().
    void stop();
    // Stops the preview, flushes every spool, hands each final chunk to on_chunk, and checks the
    // spools' health.
    void flush();
    // Records the first spool failure as the meeting's audio integrity error.
    void check_spool_health();
    std::optional<std::string> integrity_error() const;
    // The folders the spools write to.
    std::vector<std::filesystem::path> spool_dirs() const;

    // Starts one source and checks it really delivers; on failure a spool it created is released
    // and the caller may try another source for the channel. reuse_spool keeps the channel's
    // spool, so a restart loses none of the audio before it.
    bool start_source(std::shared_ptr<CaptureSource> source, bool reuse_spool = false);
    // Stops the channel's source and opens its replacement, keeping its spool.
    bool restart_channel(std::string_view channel);
    // Writes state.capture when it changed and announces it.
    void update_status(std::string message = "");
    // One watchdog pass (the thread runs one per interval while recording).
    void run_watchdog_pass();

    std::shared_ptr<CaptureSource> source(std::string_view channel) const;
    std::shared_ptr<SpoolWriter> spool(std::string_view channel) const;
    std::optional<CaptureHealth> channel_health(std::string_view channel) const;
    std::set<std::string> stall_channels() const;
    std::vector<audio::DeviceKey> mic_excluded() const;
    std::optional<audio::InputDevice> mic_device() const;

private:
    struct Probe {
        std::optional<audio::InputDevice> mic;
        std::optional<audio::LoopbackEndpoint> loopback;
    };
    struct RouterState {
        std::string channel;
        std::atomic<bool> failed{false};
        bool logged = false; // guarded by mutex_
    };

    BlockSink make_block_router(std::shared_ptr<SpoolWriter> spool,
                                std::shared_ptr<CaptureSignalMonitor> monitor,
                                std::shared_ptr<RouterState> state);
    void open_system_audio();
    // The endpoint `tried` (whose open failed) asked for stereo, as OpenWhisper's SoundCard
    // fallback records; false when it already was.
    bool start_fallback_loopback(const std::optional<audio::LoopbackEndpoint>& tried,
                                 bool reuse_spool = false);
    std::vector<std::string> notes(const std::set<std::string, std::less<>>& active) const;
    bool open_mic(std::optional<audio::InputDevice> desired, bool reuse_spool);
    std::optional<audio::InputDevice> find_mic();
    bool exclude_mic(const std::optional<audio::InputDevice>& device);
    bool restart_channel(std::string_view channel, Probe desired);
    void stop_source(std::string_view channel);
    void watch_channel(std::string_view channel);
    Probe probe_device(std::string_view channel);
    bool channel_healthy(std::string_view channel, const std::shared_ptr<CaptureSource>& source);
    // Whether `current` records the default render endpoint (Backend::default_loopback_id).
    bool loopback_follows_default(const CaptureSource& current);
    bool device_changed(std::string_view channel, const std::shared_ptr<CaptureSource>& source,
                        const Probe& desired);
    std::vector<std::string> missing_parts(const std::set<std::string, std::less<>>& active) const;
    std::string gap_message(const std::set<std::string, std::less<>>& active,
                            bool loopback_available) const;
    void record_integrity_error(const std::string& message);
    void log_router_failures();
    void start_watchdog();
    void stop_watchdog();
    void watchdog_loop();

    const CaptureOptions options_;
    MeetingClock& clock_;
    const std::shared_ptr<meeting_state::MeetingRepository> repository_;
    const std::shared_ptr<audio::Backend> backend_;
    CaptureStatusSink& sink_;
    const std::function<bool()> recording_;
    const std::function<void(const meeting_state::SpooledChunk&)> on_chunk_;
    const std::function<void(const CaptureBlock&)> preview_feed_;
    SourceFactory factory_;
    CaptureTiming timing_;
    std::function<void()> stop_preview_;
    FailureLog failures_;

    mutable std::mutex mutex_;
    std::string meeting_id_;
    std::filesystem::path spool_dir_;
    std::vector<std::shared_ptr<CaptureSource>> sources_;
    // Insertion-ordered, as Python's dicts: flush() hands final chunks on in this order.
    std::vector<std::pair<std::string, std::shared_ptr<SpoolWriter>>> spools_;
    std::vector<std::pair<std::string, std::shared_ptr<CaptureSignalMonitor>>> monitors_;
    std::vector<std::shared_ptr<RouterState>> routers_;
    std::set<std::string> stall_channels_;
    // Keys of microphones that failed this meeting: PortAudio's list is a snapshot, so an
    // unplugged microphone keeps being found until it is excluded here.
    std::vector<audio::DeviceKey> mic_excluded_;
    std::optional<audio::InputDevice> mic_device_;
    std::map<std::string, double, std::less<>> last_attempt_; // watchdog thread only
    bool system_audio_disabled_ = false;
    bool loopback_was_available_ = false;
    std::optional<std::string> explicit_message_;
    std::optional<std::string> integrity_error_;

    std::mutex watchdog_mutex_;
    std::condition_variable watchdog_wake_;
    bool watchdog_stop_ = true; // guarded by watchdog_mutex_
    std::thread watchdog_;
};

} // namespace fv::meeting
