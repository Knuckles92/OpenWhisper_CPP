#include "meeting_capture/capture_session.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <exception>
#include <format>
#include <stdexcept>

#include "base/log.h"
#include "meeting_capture/detail/files.h"
#include "meeting_capture/devices.h"
#include "pyport/json.h"

namespace fv::meeting {

namespace {

constexpr log::Logger logger("meeting.capture_session");

using meeting_state::kChannelLoopback;
using meeting_state::kChannelMic;
using ChannelSet = std::set<std::string, std::less<>>;

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

// Python's str.strip() for ASCII whitespace.
std::string strip(std::string_view text) {
    constexpr std::string_view space = " \t\n\r\f\v";
    const std::size_t first = text.find_first_not_of(space);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(space);
    return std::string(text.substr(first, last - first + 1));
}

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

std::string key(std::string_view kind, std::string_view channel) {
    return std::format("{}/{}", kind, channel);
}

} // namespace

MeetingCapture::MeetingCapture(CaptureOptions options, MeetingClock& clock,
                               std::shared_ptr<meeting_state::MeetingRepository> repository,
                               std::shared_ptr<audio::Backend> backend, CaptureStatusSink& sink,
                               std::function<bool()> recording,
                               std::function<void(const meeting_state::SpooledChunk&)> on_chunk,
                               std::function<void(const CaptureBlock&)> preview_feed)
    : options_(std::move(options)), clock_(clock), repository_(std::move(repository)),
      backend_(std::move(backend)), sink_(sink), recording_(std::move(recording)),
      on_chunk_(std::move(on_chunk)), preview_feed_(std::move(preview_feed)), failures_(logger) {
    factory_.mic = [this](const audio::InputDevice& device) -> std::shared_ptr<CaptureSource> {
        return std::make_shared<MicSource>(backend_, clock_, std::string(kChannelMic), device.index,
                                           device.samplerate, device.channels);
    };
    factory_.loopback =
        [this](const audio::LoopbackEndpoint& endpoint) -> std::shared_ptr<CaptureSource> {
        return std::make_shared<LoopbackSource>(backend_, clock_, endpoint);
    };
}

MeetingCapture::~MeetingCapture() {
    stop();
    std::vector<std::pair<std::string, std::shared_ptr<SpoolWriter>>> spools;
    {
        const std::lock_guard lock(mutex_);
        spools.swap(spools_);
    }
    spools.clear(); // each spool finishes its writer as it goes
}

void MeetingCapture::set_source_factory(SourceFactory factory) {
    if (factory.mic) {
        factory_.mic = std::move(factory.mic);
    }
    if (factory.loopback) {
        factory_.loopback = std::move(factory.loopback);
    }
}

void MeetingCapture::set_timing(CaptureTiming timing) {
    timing_ = timing;
}

void MeetingCapture::set_stop_preview(std::function<void()> stop_preview) {
    stop_preview_ = std::move(stop_preview);
}

std::optional<std::string> MeetingCapture::start(const std::string& meeting_id,
                                                 const std::filesystem::path& spool_dir) {
    {
        const std::lock_guard lock(mutex_);
        meeting_id_ = meeting_id;
        spool_dir_ = spool_dir;
        sources_.clear();
        monitors_.clear();
        stall_channels_.clear();
        system_audio_disabled_ = lower(options_.system_audio_policy.empty()
                                           ? std::string("auto")
                                           : options_.system_audio_policy) == "disabled";
        loopback_was_available_ = false;
        explicit_message_.reset();
        integrity_error_.reset();
        mic_excluded_.clear();
        mic_device_.reset();
    }
    if (!open_mic(std::nullopt, false)) {
        logger.warning("No microphone could be opened; mic channel disabled");
    }
    bool disabled = false;
    {
        const std::lock_guard lock(mutex_);
        disabled = system_audio_disabled_;
    }
    if (disabled) {
        logger.info("System audio disabled for this meeting by user choice");
    } else {
        open_system_audio();
    }

    ChannelSet active;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& source : sources_) {
            active.emplace(source->channel());
        }
    }
    const std::vector<std::string> parts = notes(active);
    if (active.empty()) {
        update_status("No audio devices could be opened.");
        throw std::runtime_error("No audio devices could be opened.");
    }
    std::optional<std::string> note;
    if (std::string joined = join(parts, " "); !joined.empty()) {
        note = std::move(joined);
    }
    {
        const std::lock_guard lock(mutex_);
        if (system_audio_disabled_) {
            explicit_message_ = note;
        }
        loopback_was_available_ = active.contains(kChannelLoopback);
    }
    update_status(note.value_or(""));
    start_watchdog();
    return note;
}

void MeetingCapture::open_system_audio() {
    bool loop_ok = false;
    // find_loopback_endpoint logs its own failures and gives nullopt.
    const std::optional<audio::LoopbackEndpoint> endpoint = find_loopback_endpoint(*backend_);
    if (endpoint) {
        try {
            loop_ok = start_source(factory_.loopback(*endpoint));
        } catch (const std::exception& error) {
            logger.error_with(error, "Failed to open WASAPI loopback stream");
        }
    }
    if (!loop_ok) {
        loop_ok = start_fallback_loopback(endpoint);
    }
    if (loop_ok) {
        return;
    }
    logger.warning("No loopback source available; meeting continues mic-only");
    if (lower(options_.system_audio_policy.empty() ? std::string("auto")
                                                   : options_.system_audio_policy) == "required") {
        throw std::runtime_error(
            "LINUX_SYSTEM_AUDIO_REQUIRED: System-audio capture is required but unavailable.");
    }
}

bool MeetingCapture::start_fallback_loopback(const std::optional<audio::LoopbackEndpoint>& tried,
                                             bool reuse_spool) {
    // OpenWhisper's Windows fallback is SoundCard's loopback of the default speaker at
    // min(2, channels) channels, which the audio engine downmixes to: the same endpoint asked for
    // stereo. An endpoint already opened at two channels or fewer has nothing else to try; the
    // watchdog retries that open.
    if (!tried || tried->channels <= 2) {
        return false;
    }
    audio::LoopbackEndpoint stereo = *tried;
    stereo.channels = 2;
    try {
        if (start_source(factory_.loopback(stereo), reuse_spool)) {
            failures_.recovered(key("fallback", kChannelLoopback));
            return true;
        }
    } catch (...) {
        // The watchdog retries this every few seconds while system audio is missing.
        failures_.failed(key("fallback", kChannelLoopback), "Stereo loopback fallback unavailable");
    }
    return false;
}

std::vector<std::string> MeetingCapture::notes(const ChannelSet& active) const {
    std::vector<std::string> out;
    if (!active.contains(kChannelMic)) {
        out.emplace_back("Microphone capture is unavailable.");
    }
    if (active.contains(kChannelLoopback)) {
        return out;
    }
    bool disabled = false;
    {
        const std::lock_guard lock(mutex_);
        disabled = system_audio_disabled_;
    }
    if (disabled) {
        out.emplace_back("System audio disabled for this meeting by your microphone-only choice.");
    } else if (active.contains(kChannelMic)) {
        out.emplace_back("System-audio capture is unavailable; recording microphone only.");
    } else {
        out.emplace_back("System-audio capture is unavailable.");
    }
    return out;
}

bool MeetingCapture::start_source(std::shared_ptr<CaptureSource> source, bool reuse_spool) {
    const std::string channel(source->channel());
    std::shared_ptr<SpoolWriter> spool;
    std::shared_ptr<CaptureSignalMonitor> monitor;
    std::string meeting_id;
    std::filesystem::path spool_dir;
    {
        const std::lock_guard lock(mutex_);
        if (reuse_spool) {
            for (const auto& [name, existing] : spools_) {
                if (name == channel) {
                    spool = existing;
                }
            }
        }
        for (const auto& [name, existing] : monitors_) {
            if (name == channel) {
                monitor = existing;
            }
        }
        meeting_id = meeting_id_;
        spool_dir = spool_dir_;
    }
    if (monitor) {
        monitor->reset_stream();
    } else {
        monitor = std::make_shared<CaptureSignalMonitor>(clock_.clock());
        const std::lock_guard lock(mutex_);
        monitors_.emplace_back(channel, monitor);
    }
    bool created_spool = false;
    if (!spool) {
        SpoolOptions spool_options;
        spool_options.initial_seq = repository_->next_chunk_seq(meeting_id, channel);
        spool = std::make_shared<SpoolWriter>(meeting_id, channel, spool_dir, clock_, repository_,
                                              on_chunk_, std::move(spool_options));
        created_spool = true;
    }
    auto router = std::make_shared<RouterState>();
    router->channel = channel;
    bool started = false;
    bool active = false;
    try {
        source->start(make_block_router(spool, monitor, router));
        started = true;
        active = source->is_active();
    } catch (...) {
        // Device drivers can fail any way they like; reached on every watchdog restart while a
        // device keeps failing.
        failures_.failed(key("start", channel), std::format("Failed to start {} capture", channel));
        active = false;
    }
    if (!active) {
        if (started) {
            logger.error("{} capture reported inactive right after starting; treating it as "
                         "unavailable",
                         channel);
            try {
                source->stop();
            } catch (const std::exception& error) {
                logger.error_with(error, std::format("Failed to stop inactive {} source", channel));
            }
        }
        if (created_spool) {
            try {
                spool->flush(); // joins the writer thread; nothing to write
            } catch (const std::exception& error) {
                logger.error_with(error, std::format("Failed to release spool for {}", channel));
            }
        }
        return false;
    }
    {
        const std::lock_guard lock(mutex_);
        bool replaced = false;
        for (auto& [name, existing] : spools_) {
            if (name == channel) {
                existing = spool;
                replaced = true;
            }
        }
        if (!replaced) {
            spools_.emplace_back(channel, spool);
        }
        std::erase_if(sources_, [&](const std::shared_ptr<CaptureSource>& existing) {
            return existing->channel() == channel;
        });
        sources_.push_back(std::move(source));
        // The channel's earlier routers are done; one whose failure is not reported yet stays
        // until check_spool_health reports it.
        std::erase_if(routers_, [&](const std::shared_ptr<RouterState>& existing) {
            return existing->channel == channel && (existing->logged || !existing->failed.load());
        });
        routers_.push_back(std::move(router));
    }
    failures_.recovered({key("start", channel), key("open", channel), key("restart", channel)});
    return true;
}

BlockSink MeetingCapture::make_block_router(std::shared_ptr<SpoolWriter> spool,
                                            std::shared_ptr<CaptureSignalMonitor> monitor,
                                            std::shared_ptr<RouterState> state) {
    // Runs on the device thread: drops blocks while paused, never throws.
    return [this, spool = std::move(spool), monitor = std::move(monitor),
            state = std::move(state)](const CaptureBlock& block) {
        const bool live = recording_() && !clock_.is_paused();
        monitor->observe(block, live);
        if (!live) {
            return;
        }
        // Recording comes first, so a failing preview never costs audio.
        spool->feed(block);
        if (preview_feed_) {
            try {
                preview_feed_(block);
            } catch (...) {
                state->failed.store(true);
            }
        }
    };
}

void MeetingCapture::stop() {
    stop_watchdog();
    std::vector<std::shared_ptr<CaptureSource>> sources;
    {
        const std::lock_guard lock(mutex_);
        sources.swap(sources_);
    }
    for (const auto& source : sources) {
        try {
            source->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, "Capture source stop failed");
        }
    }
}

void MeetingCapture::flush() {
    if (stop_preview_) {
        stop_preview_();
    }
    std::vector<std::shared_ptr<SpoolWriter>> spools;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [name, spool] : spools_) {
            spools.push_back(spool);
        }
    }
    for (const auto& spool : spools) {
        std::optional<meeting_state::SpooledChunk> chunk;
        try {
            chunk = spool->flush();
        } catch (const std::exception& error) {
            logger.error_with(error, "Spool flush failed");
            record_integrity_error("Meeting audio storage could not finish. Partial audio was "
                                   "kept.");
            continue;
        }
        if (chunk && on_chunk_) {
            on_chunk_(*chunk);
        }
    }
    check_spool_health();
}

void MeetingCapture::log_router_failures() {
    std::vector<std::string> newly_failed;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& router : routers_) {
            if (!router->logged && router->failed.load()) {
                router->logged = true;
                newly_failed.push_back(router->channel);
            }
        }
    }
    // The device thread may not log; the first failure of each router is reported here.
    for (const std::string& channel : newly_failed) {
        logger.error("Spool feed failed on channel {} (suppressing repeats)", channel);
    }
}

void MeetingCapture::check_spool_health() {
    log_router_failures();
    std::vector<std::shared_ptr<SpoolWriter>> spools;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [name, spool] : spools_) {
            spools.push_back(spool);
        }
    }
    for (const auto& spool : spools) {
        if (const std::optional<std::string> error = spool->health_error()) {
            record_integrity_error(*error);
        }
    }
}

std::optional<std::string> MeetingCapture::integrity_error() const {
    const std::lock_guard lock(mutex_);
    return integrity_error_;
}

std::vector<std::filesystem::path> MeetingCapture::spool_dirs() const {
    const std::lock_guard lock(mutex_);
    std::vector<std::filesystem::path> out;
    for (const auto& [name, spool] : spools_) {
        if (std::find(out.begin(), out.end(), spool->spool_dir()) == out.end()) {
            out.push_back(spool->spool_dir());
        }
    }
    return out;
}

void MeetingCapture::record_integrity_error(const std::string& message) {
    {
        const std::lock_guard lock(mutex_);
        if (integrity_error_) {
            return;
        }
        integrity_error_ = message;
    }
    logger.error("Meeting audio incomplete: {}", message);
    update_status();
    sink_.emit_error("capture_incomplete",
                     message + " Check available audio in Meeting History and free storage or "
                               "reduce system load before recording again.");
}

void MeetingCapture::start_watchdog() {
    if (watchdog_.joinable()) {
        return;
    }
    {
        const std::lock_guard lock(watchdog_mutex_);
        watchdog_stop_ = false;
    }
    watchdog_ = std::thread([this] {
        detail::set_thread_name("fv-meeting-watchdog");
        watchdog_loop();
    });
}

void MeetingCapture::stop_watchdog() {
    {
        const std::lock_guard lock(watchdog_mutex_);
        watchdog_stop_ = true;
    }
    watchdog_wake_.notify_all();
    if (!watchdog_.joinable()) {
        return;
    }
    if (watchdog_.get_id() == std::this_thread::get_id()) {
        // Called from a watchdog pass: the loop ends after this pass.
        watchdog_.detach();
        return;
    }
    // A pass in progress finishes first (a restart's open waits at most a few seconds), so no
    // pass can touch a stopped capture.
    watchdog_.join();
}

void MeetingCapture::watchdog_loop() {
    try {
        while (true) {
            {
                std::unique_lock lock(watchdog_mutex_);
                if (watchdog_wake_.wait_for(
                        lock, std::chrono::duration<double>(timing_.watchdog_interval_s),
                        [this] { return watchdog_stop_; })) {
                    return;
                }
            }
            if (!recording_()) {
                return;
            }
            run_watchdog_pass();
        }
    } catch (const std::exception& error) {
        // Python's thread would end the same way; an exception must not leave a std::thread.
        logger.error_with(error, "Capture watchdog stopped");
    } catch (...) {
        // A sink or callback that throws something else must not terminate the process either.
        logger.error("Capture watchdog stopped");
    }
}

void MeetingCapture::run_watchdog_pass() {
    for (std::string_view channel : {kChannelMic, kChannelLoopback}) {
        try {
            watch_channel(channel);
            failures_.recovered(key("watchdog", channel));
        } catch (...) {
            // A failing probe must not end the watchdog; it runs every second, so only the first
            // failure of a run warns.
            failures_.failed(key("watchdog", channel),
                             std::format("Capture watchdog failed for {}", channel));
        }
    }
    check_spool_health();
    update_status();
}

void MeetingCapture::watch_channel(std::string_view channel) {
    if (channel == kChannelLoopback) {
        const std::lock_guard lock(mutex_);
        if (system_audio_disabled_) {
            return;
        }
    }
    // The microphone is probed first, as OpenWhisper probes every channel. The loopback asks
    // only for the default endpoint's id while its stream is healthy: listing the endpoints
    // activates each one, too much for every second.
    Probe desired;
    if (channel != kChannelLoopback) {
        desired = probe_device(channel);
    }
    const std::shared_ptr<CaptureSource> current = source(channel);
    const bool active = channel_healthy(channel, current);
    if (channel == kChannelLoopback) {
        if (active && loopback_follows_default(*current)) {
            return;
        }
        desired = probe_device(channel);
    }
    if (active && !device_changed(channel, current, desired)) {
        return;
    }
    const double now = clock_.monotonic();
    // Python starts from 0.0 on its monotonic clock, which is always long ago.
    const auto found = last_attempt_.find(channel);
    if (found != last_attempt_.end() && now - found->second < timing_.retry_interval_s) {
        return;
    }
    last_attempt_.insert_or_assign(std::string(channel), now);
    restart_channel(channel, std::move(desired));
}

bool MeetingCapture::channel_healthy(std::string_view channel,
                                     const std::shared_ptr<CaptureSource>& current) {
    const bool active = current && current->is_active();
    std::shared_ptr<CaptureSignalMonitor> monitor;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [name, existing] : monitors_) {
            if (name == channel) {
                monitor = existing;
            }
        }
    }
    const std::optional<CaptureHealth> health =
        monitor ? std::optional<CaptureHealth>(monitor->snapshot()) : std::nullopt;
    if (active && health && health->receiving) {
        const std::lock_guard lock(mutex_);
        stall_channels_.erase(std::string(channel));
    }
    if (active && health && health->stalled && current->requires_audio_blocks()) {
        // PortAudio can report an open, active stream that never calls back; other backends
        // already count block delivery in is_active().
        {
            const std::lock_guard lock(mutex_);
            stall_channels_.insert(std::string(channel));
        }
        logger.warning("{} capture stream stalled despite an open device", channel);
        return false;
    }
    return active;
}

bool MeetingCapture::loopback_follows_default(const CaptureSource& current) {
    const DeviceId source_id = current.device_id();
    if (!std::holds_alternative<std::string>(source_id)) {
        return false;
    }
    const std::optional<std::string> default_id = backend_->default_loopback_id();
    return default_id && *default_id == std::get<std::string>(source_id);
}

bool MeetingCapture::device_changed(std::string_view channel,
                                    const std::shared_ptr<CaptureSource>& current,
                                    const Probe& desired) {
    const DeviceId source_id = current ? current->device_id() : DeviceId{};
    if (channel == kChannelMic && desired.mic) {
        // Only "System default" follows the default; a ranked microphone stays until it fails.
        return !options_.mic_device_id && options_.mic_priority.empty() &&
               std::holds_alternative<int>(source_id) &&
               std::get<int>(source_id) != desired.mic->index;
    }
    if (channel == kChannelLoopback && desired.loopback &&
        std::holds_alternative<std::string>(source_id)) {
        return std::get<std::string>(source_id) != desired.loopback->id;
    }
    if (channel == kChannelLoopback && current) {
        if (const std::optional<bool> is_current = current->is_default_device_current()) {
            return !*is_current;
        }
    }
    return false;
}

MeetingCapture::Probe MeetingCapture::probe_device(std::string_view channel) {
    Probe probe;
    if (channel == kChannelLoopback) {
        probe.loopback = find_loopback_endpoint(*backend_);
    } else {
        probe.mic = find_mic();
    }
    return probe;
}

std::optional<audio::InputDevice> MeetingCapture::find_mic() {
    std::vector<audio::DeviceKey> excluded;
    {
        const std::lock_guard lock(mutex_);
        excluded = mic_excluded_;
    }
    if (!options_.mic_priority.empty()) {
        return find_mic_device(*backend_, options_.mic_priority, std::nullopt, excluded);
    }
    return find_mic_device(*backend_, {}, options_.mic_device_id, excluded);
}

bool MeetingCapture::exclude_mic(const std::optional<audio::InputDevice>& device) {
    if (!device) {
        return false;
    }
    const audio::DeviceKey device_key{device->name, device->hostapi};
    const std::lock_guard lock(mutex_);
    if (std::find(mic_excluded_.begin(), mic_excluded_.end(), device_key) != mic_excluded_.end()) {
        return false;
    }
    mic_excluded_.push_back(device_key);
    return true;
}

bool MeetingCapture::open_mic(std::optional<audio::InputDevice> desired, bool reuse_spool) {
    bool retried_all = false;
    for (int attempt = 0; attempt < MIC_OPEN_ATTEMPTS; ++attempt) {
        std::optional<audio::InputDevice> device = std::move(desired);
        desired.reset();
        if (!device) {
            try {
                device = find_mic();
                failures_.recovered(key("probe", kChannelMic));
            } catch (...) {
                // Device enumeration; retried on each watchdog restart.
                failures_.failed(key("probe", kChannelMic), "Microphone probe failed");
                return false;
            }
        }
        bool excluded_any = false;
        {
            const std::lock_guard lock(mutex_);
            excluded_any = !mic_excluded_.empty();
        }
        if (!device && excluded_any && !retried_all) {
            // Every microphone failed once. One may only have hiccuped, and trying it again beats
            // leaving the channel dead.
            retried_all = true;
            const std::lock_guard lock(mutex_);
            mic_excluded_.clear();
            continue;
        }
        if (!device) {
            return false;
        }
        bool opened = false;
        try {
            opened = start_source(factory_.mic(*device), reuse_spool);
        } catch (...) {
            failures_.failed(key("open", kChannelMic), "Failed to open microphone stream");
            opened = false;
        }
        if (opened) {
            const std::lock_guard lock(mutex_);
            mic_device_ = device;
            return true;
        }
        if (!exclude_mic(device)) {
            return false;
        }
    }
    return false;
}

std::shared_ptr<CaptureSource> MeetingCapture::source(std::string_view channel) const {
    const std::lock_guard lock(mutex_);
    for (const auto& existing : sources_) {
        if (existing->channel() == channel) {
            return existing;
        }
    }
    return nullptr;
}

std::shared_ptr<SpoolWriter> MeetingCapture::spool(std::string_view channel) const {
    const std::lock_guard lock(mutex_);
    for (const auto& [name, existing] : spools_) {
        if (name == channel) {
            return existing;
        }
    }
    return nullptr;
}

std::optional<CaptureHealth> MeetingCapture::channel_health(std::string_view channel) const {
    std::shared_ptr<CaptureSignalMonitor> monitor;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& [name, existing] : monitors_) {
            if (name == channel) {
                monitor = existing;
            }
        }
    }
    if (!monitor) {
        return std::nullopt;
    }
    return monitor->snapshot();
}

std::set<std::string> MeetingCapture::stall_channels() const {
    const std::lock_guard lock(mutex_);
    return stall_channels_;
}

std::vector<audio::DeviceKey> MeetingCapture::mic_excluded() const {
    const std::lock_guard lock(mutex_);
    return mic_excluded_;
}

std::optional<audio::InputDevice> MeetingCapture::mic_device() const {
    const std::lock_guard lock(mutex_);
    return mic_device_;
}

bool MeetingCapture::restart_channel(std::string_view channel) {
    return restart_channel(channel, Probe{});
}

bool MeetingCapture::restart_channel(std::string_view channel, Probe desired) {
    bool in_stall = false;
    {
        const std::lock_guard lock(mutex_);
        if (channel == kChannelLoopback && system_audio_disabled_) {
            return false;
        }
        in_stall = stall_channels_.contains(std::string(channel));
    }
    const std::shared_ptr<CaptureSource> current = source(channel);
    const bool lost = !current || !current->is_active() || in_stall;
    stop_source(channel);
    try {
        if (channel == kChannelMic) {
            if (lost && exclude_mic(mic_device())) {
                // The watchdog probed before it knew this source was dead and may have named the
                // same device again.
                desired.mic.reset();
            }
            return open_mic(std::move(desired.mic), true);
        }
        // The watchdog's probe is reused, so recovery does not pay for a second enumeration
        // inside the 12 s budget.
        if (!desired.loopback) {
            desired.loopback = find_loopback_endpoint(*backend_);
        }
        if (desired.loopback && start_source(factory_.loopback(*desired.loopback), true)) {
            return true;
        }
        return start_fallback_loopback(desired.loopback, true);
    } catch (...) {
        // Reached from the watchdog every few seconds while a channel is down; the first failure
        // of a run is an error.
        failures_.failed(key("restart", channel),
                         std::format("Could not restart {} capture", channel));
    }
    return false;
}

void MeetingCapture::stop_source(std::string_view channel) {
    std::vector<std::shared_ptr<CaptureSource>> stopped;
    {
        const std::lock_guard lock(mutex_);
        for (const auto& existing : sources_) {
            if (existing->channel() == channel) {
                stopped.push_back(existing);
            }
        }
        std::erase_if(sources_, [&](const std::shared_ptr<CaptureSource>& existing) {
            return existing->channel() == channel;
        });
    }
    for (const auto& existing : stopped) {
        try {
            existing->stop();
        } catch (const std::exception& error) {
            logger.error_with(error, std::format("Could not stop stale {} source", channel));
        }
    }
}

void MeetingCapture::update_status(std::string message) {
    const Json previous = sink_.capture_state();
    if (previous.is_null()) {
        return; // no store
    }
    std::vector<std::shared_ptr<CaptureSource>> sources;
    std::vector<std::pair<std::string, std::shared_ptr<CaptureSignalMonitor>>> monitors;
    {
        const std::lock_guard lock(mutex_);
        sources = sources_;
        monitors = monitors_;
    }
    ChannelSet active;
    for (const auto& existing : sources) {
        if (existing->is_active()) {
            active.emplace(existing->channel());
        }
    }
    std::map<std::string, CaptureHealth, std::less<>> health;
    for (const auto& [name, monitor] : monitors) {
        health.insert_or_assign(name, monitor->snapshot());
    }
    const bool loopback_available = active.contains(kChannelLoopback);
    std::optional<std::string> integrity;
    {
        const std::lock_guard lock(mutex_);
        if (loopback_available && !loopback_was_available_ && !system_audio_disabled_) {
            // The rising edge only.
            if (message.empty()) {
                message = "System audio restored.";
            }
            explicit_message_.reset();
        }
        loopback_was_available_ = loopback_available;
        integrity = integrity_error_;
    }
    if (message.empty()) {
        message = gap_message(active, loopback_available);
    }
    if (integrity && !integrity->empty()) {
        message = strip(*integrity + " " + message);
    }
    const auto health_of = [&](std::string_view channel) {
        const auto found = health.find(channel);
        return found == health.end() ? CaptureHealth{} : found->second;
    };
    const CaptureHealth mic = health_of(kChannelMic);
    const CaptureHealth loopback = health_of(kChannelLoopback);
    Json capture = Json::object();
    capture["mic_available"] = active.contains(kChannelMic);
    capture["loopback_available"] = loopback_available;
    capture["mic_receiving"] = mic.receiving;
    capture["loopback_receiving"] = loopback.receiving;
    capture["mic_signal_windows"] = mic.signal_windows;
    capture["loopback_signal_windows"] = loopback.signal_windows;
    capture["mic_source_generation"] = mic.generation;
    capture["loopback_source_generation"] = loopback.generation;
    capture["message"] = message;
    capture["audio_incomplete"] = integrity.has_value() && !integrity->empty();
    capture["integrity_error"] = integrity.value_or("");
    if (py::py_equal(capture, previous)) {
        return;
    }
    if (sink_.update_capture(capture)) {
        sink_.emit_status(message);
    }
}

std::vector<std::string> MeetingCapture::missing_parts(const ChannelSet& active) const {
    const std::lock_guard lock(mutex_);
    std::vector<std::string> missing;
    if (!active.contains(kChannelMic)) {
        missing.emplace_back("Microphone unavailable");
    } else if (stall_channels_.contains(std::string(kChannelMic))) {
        missing.emplace_back("Microphone is connected but no audio blocks are arriving; check "
                             "the selected input device");
    }
    if (!active.contains(kChannelLoopback)) {
        if (system_audio_disabled_) {
            missing.emplace_back(
                "System audio disabled for this meeting by your microphone-only choice");
        } else {
            missing.emplace_back("System audio unavailable");
        }
    } else if (stall_channels_.contains(std::string(kChannelLoopback))) {
        missing.emplace_back("System audio is connected but no audio blocks are arriving; check "
                             "the output device");
    }
    return missing;
}

std::string MeetingCapture::gap_message(const ChannelSet& active, bool loopback_available) const {
    const std::vector<std::string> missing = missing_parts(active);
    std::optional<std::string> explicit_message;
    bool mic_stalled = false;
    {
        const std::lock_guard lock(mutex_);
        explicit_message = explicit_message_;
        mic_stalled = stall_channels_.contains(std::string(kChannelMic));
    }
    if (!(explicit_message && !explicit_message->empty() && !loopback_available)) {
        return join(missing, "; ");
    }
    // A deliberate microphone-only message must not hide a new microphone stall.
    std::string message = *explicit_message;
    if (mic_stalled && !missing.empty()) {
        message += " " + missing.front();
    }
    return message;
}

} // namespace fv::meeting
