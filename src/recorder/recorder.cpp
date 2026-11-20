#include "recorder/recorder.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>
#include <utility>

#include "audio/wav_writer.h"
#include "base/log.h"
#include "base/metrics.h"
#include "base/utf8.h"
#include "dsp/level.h"
#include "pyport/number.h"
#include "recorder/detail/capture_ring.h"
#include "recorder/detail/event.h"
#include "recorder/detail/platform.h"
#include "settings/store.h"

namespace fv {

namespace {

const log::Logger kLog("services.recorder");

using Steady = std::chrono::steady_clock;
using recorder_detail::BlockFlags;
using recorder_detail::CaptureRing;
using recorder_detail::Event;

// The ring between a device callback and the audio-io thread holds as many blocks as the
// journal's queue: falling that far behind is "Recording storage cannot keep up".
constexpr std::size_t kRingSlots = JOURNAL_QUEUE_BLOCKS;
// How often the audio-io thread looks at the rings while a stream is open. A device callback
// may not signal (no syscalls), so the consumer polls.
constexpr std::chrono::milliseconds kIoPoll{5};
constexpr std::chrono::milliseconds kIoIdle{250};

std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    std::ranges::transform(lowered, lowered.begin(), [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return lowered;
}

double milliseconds_between(Steady::time_point from, Steady::time_point to) {
    return std::chrono::duration<double, std::milli>(to - from).count();
}

// sounddevice's CallbackFlags.__str__ for an input stream's flags: the set flags' names in
// dir() order, which is alphabetical, so overflow comes before underflow.
std::string describe_flags(const BlockFlags& flags) {
    std::string text;
    if (flags.overflow) {
        text = "input overflow";
    }
    if (flags.underflow) {
        text += text.empty() ? "input underflow" : ", input underflow";
    }
    return text;
}

std::chrono::local_seconds local_seconds_now(const Clock& clock) {
    return std::chrono::floor<std::chrono::seconds>(clock.zone()->to_local(clock.utc()));
}

} // namespace

namespace recorder_detail {

struct RecorderImpl;

// One stream the recorder opened. Its callback keeps a block only while `accepting`: from the
// moment the stream is installed as the recording's microphone until the recorder leaves it
// behind (OpenWhisper's _stream_generation), so a replacement's early blocks never land before
// the switch gap and a dead stream's late ones are ignored.
struct StreamContext {
    StreamContext(RecorderImpl* owner, std::size_t slot_samples)
        : impl(owner), ring(kRingSlots, slot_samples) {}

    RecorderImpl* impl;
    CaptureRing ring;
    std::atomic<bool> accepting{false};
    // The stream ended by itself while it was the recording's microphone.
    std::atomic<bool> lost{false};
    // Blocks seen, kept or not (paused): the watcher reads liveness from it.
    std::atomic<std::uint64_t> blocks{0};
    // The ring was full: every later block is dropped, as a stopped journal drops them.
    std::atomic<bool> overrun{false};
    std::atomic<std::uint64_t> overrun_frames{0};
    bool overrun_reported = false; // audio-io thread only
};

// The stream closes before its context goes: a callback may run until the close returns.
struct OpenStream {
    std::shared_ptr<StreamContext> ctx;
    std::unique_ptr<audio::Stream> stream;
};

// abort() drops what the device still buffers instead of draining it (7 ms median against
// stop()'s 33 ms on MME, September 2026); that audio is post-roll either way.
void close_stream(OpenStream& open) noexcept {
    try {
        if (open.stream) {
            open.stream->abort();
        }
    } catch (...) { // NOLINT(bugprone-empty-catch): the stream may already be aborted or dead
    }
    try {
        open.stream.reset();
    } catch (...) { // NOLINT(bugprone-empty-catch): the stream may already be closed
    }
}

// Fresh per recording, so a previous recording's thread can never signal this one.
struct Session {
    Event stop;
    Event post_roll_end;
    Event complete;
    std::thread watcher;
};

// The audio-io thread's work list, in order: a stream's blocks, or the silence of a switch.
struct IoItem {
    std::shared_ptr<StreamContext> ctx;
    std::int64_t gap_frames = 0;
};

// A microphone to try; nullopt is PortAudio's default input.
struct Candidate {
    std::optional<audio::InputDevice> device;
};

void on_block(void* user, const std::int16_t* samples, std::size_t frames,
              audio::BlockStatus status);

struct RecorderImpl : std::enable_shared_from_this<RecorderImpl> {
    RecorderImpl(audio::Backend& audio_backend, RecorderOptions recorder_options)
        : backend(audio_backend), options(std::move(recorder_options)),
          clock(options.clock ? *options.clock : system_clock()),
          gate(RECORDER_SAMPLE_RATE, options.post_roll),
          device_priority(audio::normalize_priority(options.device_priority)) {}

    audio::Backend& backend;
    const RecorderOptions options;
    const Clock& clock;
    const int rate = RECORDER_SAMPLE_RATE;
    const int channels = RECORDER_CHANNELS;
    const int chunk = RECORDER_CHUNK;

    Signal<float> level;
    Signal<std::string> capture_error;
    Signal<std::string, std::string> device_switched;

    // OpenWhisper's _callback_lock: everything below up to the atomics.
    mutable std::mutex mutex;
    PostRollGate gate;
    std::vector<audio::DeviceKey> device_priority;
    std::optional<audio::InputDevice> active_device;
    std::shared_ptr<OpenStream> stream;
    std::shared_ptr<RecordingJournal> journal;
    std::shared_ptr<audio::PcmTape> tape;
    std::uint64_t recorded_bytes = 0;
    std::int64_t recorded_frames = 0;
    std::shared_ptr<Session> session;
    std::string post_roll_end_reason;
    bool stop_requested = false;
    std::optional<Steady::time_point> stop_requested_at;
    std::optional<Steady::time_point> post_roll_deadline;
    std::uint64_t session_token = 0;
    std::deque<Steady::time_point> overflow_times;
    std::deque<IoItem> items;
    double current_level = 0.0;
    Steady::time_point stream_started_at{};
    std::optional<Steady::time_point> last_callback_at;
    std::uint64_t last_blocks_seen = 0;
    int device_switches = 0;
    std::string last_start_error;

    // Touched only by start_recording and the watcher, never at the same time.
    std::vector<audio::DeviceKey> failed_keys;
    std::set<int> failed_indexes;
    bool default_failed = false;

    std::atomic<bool> recording{false};
    std::atomic<bool> canceled{false};
    std::atomic<bool> paused{false};
    std::atomic<bool> capture_failed{false};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> overflows{0};

    // OpenWhisper's _error_lock.
    mutable std::mutex error_mutex;
    std::string last_capture_error;

    std::mutex io_mutex;
    std::condition_variable io_changed;
    std::thread io_thread;
    bool io_running = false;
    bool io_stop = false;
    std::uint64_t drain_requested = 0;
    std::uint64_t drain_done = 0;

    std::mutex release_mutex;
    std::vector<std::shared_ptr<Event>> releases;

    // Serializes start_recording: two callers that both found no recording in progress would
    // otherwise both open a stream and the second would orphan the first's session. Never taken
    // by anything start_recording calls out to.
    std::mutex start_mutex;

    // --- audio-io thread ---------------------------------------------------------------
    void ensure_io_thread();
    void stop_io_thread();
    void io_loop();
    bool process_available();
    void process_block(std::span<const std::int16_t> samples, std::size_t frames, BlockFlags flags);
    void process_overrun(StreamContext& ctx);
    void append_gap(std::int64_t frames);
    std::optional<std::string> status_error(const BlockFlags& flags);
    void drain_io();

    // --- capture state -----------------------------------------------------------------
    void end_post_roll_locked(std::string_view reason);
    void end_post_roll(std::string_view reason);
    void stop_for_capture_error_locked();
    void fail_capture(const std::string& message);
    std::shared_ptr<RecordingJournal> current_journal() const;

    // --- devices -----------------------------------------------------------------------
    class Cursor;
    std::shared_ptr<OpenStream> open_stream(const std::optional<audio::InputDevice>& device,
                                            bool accepting);
    std::pair<std::shared_ptr<OpenStream>, std::optional<audio::InputDevice>> open_first();
    void mark_failed(std::optional<audio::InputDevice> device);
    static std::string describe(const std::optional<audio::InputDevice>& device);
    static std::string switch_name(const std::optional<audio::InputDevice>& device);
    void retire_stream(std::shared_ptr<OpenStream> old);
    void unwind_failed_stream();

    // --- watcher -----------------------------------------------------------------------
    void watch(const std::shared_ptr<Session>& session);
    void watch_capture(Session& session);
    bool capture_lost();
    bool switch_device(Session& session);
    void close_capture_stream();
    void finish_capture_journal();
    void report_capture_stats();
    void log_post_roll(std::string_view reason);

    // --- the facade --------------------------------------------------------------------
    bool start_recording();
    bool stop_recording();
    bool wait_for_stop_completion(std::optional<std::chrono::milliseconds> timeout);
    bool save_recording(std::optional<std::filesystem::path> file, bool allow_incomplete);
    bool capture_saveable(const std::shared_ptr<RecordingJournal>& spool, bool allow_incomplete);
    void clear_recording_data(bool discard, bool wait);
    void cancel_recording();
    void cleanup();
};

// Runs on the device thread: preallocated memory and atomics only.
void on_block(void* user, const std::int16_t* samples, std::size_t frames,
              audio::BlockStatus status) {
    auto* ctx = static_cast<StreamContext*>(user);
    if (!ctx->accepting.load(std::memory_order_acquire)) {
        return; // a stream this recording left behind, or one not yet installed
    }
    const RecorderImpl& impl = *ctx->impl;
    if (impl.canceled.load(std::memory_order_relaxed) ||
        impl.capture_failed.load(std::memory_order_relaxed)) {
        return; // the stream is only closing; keep nothing more
    }
    ctx->blocks.fetch_add(1, std::memory_order_relaxed);
    const bool flagged = status.overflow || status.underflow;
    const bool paused = impl.paused.load(std::memory_order_relaxed);
    if (paused && !flagged) {
        // The microphone stays watched (a lost one still switches); only what it hears is let
        // go.
        return;
    }
    if (ctx->overrun.load(std::memory_order_relaxed)) {
        ctx->overrun_frames.fetch_add(frames, std::memory_order_relaxed);
        return;
    }
    const auto channels = static_cast<std::size_t>(impl.channels);
    const std::size_t slot_frames = std::max<std::size_t>(ctx->ring.slot_samples() / channels, 1);
    const BlockFlags flags{status.overflow, status.underflow, paused};
    std::size_t done = 0;
    while (done < frames) {
        // PortAudio hands over whole blocks of the stream's size; only a larger block than any
        // stream opens would be split.
        const std::size_t part = std::min(frames - done, slot_frames);
        if (!ctx->ring.push(samples + done * channels, part * channels, part, flags)) {
            ctx->overrun_frames.fetch_add(frames - done, std::memory_order_relaxed);
            ctx->overrun.store(true, std::memory_order_release);
            return;
        }
        done += part;
    }
}

// _candidates: microphones to try, best first, read lazily so a failure marked between two
// tries is skipped by the next.
class RecorderImpl::Cursor {
public:
    explicit Cursor(RecorderImpl& impl) : impl_(impl) {}

    std::optional<Candidate> next() {
        if (step_ == 0) {
            step_ = 1;
            if (impl_.options.device_id) {
                audio::InputDevice raw;
                raw.index = *impl_.options.device_id;
                return Candidate{raw};
            }
        }
        if (step_ == 1) {
            step_ = 2;
            bool unranked = false;
            {
                const std::lock_guard lock(impl_.mutex);
                unranked = impl_.device_priority.empty();
            }
            // Without a ranked microphone the first try is the one recording always made, so
            // the common start pays for no device query.
            if (unranked && !impl_.default_failed) {
                return Candidate{};
            }
        }
        if (step_ == 2) {
            step_ = 3;
            std::vector<audio::DeviceKey> priority;
            {
                const std::lock_guard lock(impl_.mutex);
                priority = impl_.device_priority;
            }
            try {
                audio::RankOptions rank;
                rank.exclude = impl_.failed_keys;
                rank.samplerate = impl_.rate;
                rank.channels = impl_.channels;
                for (audio::InputDevice& device :
                     audio::ranked_candidates(impl_.backend, priority, rank)) {
                    ranked_.push_back(Candidate{std::move(device)});
                }
            } catch (const std::exception& error) {
                kLog.warning("Couldn't list audio inputs ({})", error.what());
                ranked_.clear();
                if (!impl_.default_failed) {
                    ranked_.push_back(Candidate{});
                }
            }
        }
        while (position_ < ranked_.size()) {
            const Candidate& candidate = ranked_[position_++];
            if (!candidate.device || !impl_.failed_indexes.contains(candidate.device->index)) {
                return candidate;
            }
        }
        return std::nullopt;
    }

private:
    RecorderImpl& impl_;
    int step_ = 0;
    std::vector<Candidate> ranked_;
    std::size_t position_ = 0;
};

// --- audio-io thread -----------------------------------------------------------------------

void RecorderImpl::ensure_io_thread() {
    const std::lock_guard lock(io_mutex);
    if (io_running) {
        return;
    }
    io_stop = false;
    io_running = true;
    io_thread = std::thread([this] { io_loop(); });
}

void RecorderImpl::stop_io_thread() {
    {
        const std::lock_guard lock(io_mutex);
        if (!io_running) {
            return;
        }
        io_stop = true;
    }
    io_changed.notify_all();
    if (io_thread.joinable() && io_thread.get_id() != std::this_thread::get_id()) {
        io_thread.join();
    }
    const std::lock_guard lock(io_mutex);
    io_running = false;
    io_changed.notify_all();
}

void RecorderImpl::io_loop() {
    recorder_detail::set_thread_name("fv-audio-io");
    std::unique_lock lock(io_mutex);
    while (!io_stop) {
        const std::uint64_t request = drain_requested;
        lock.unlock();
        bool busy = false;
        try {
            busy = process_available();
        } catch (const std::exception& error) {
            kLog.error_with(error, "The audio-io thread failed");
        }
        lock.lock();
        drain_done = request;
        io_changed.notify_all();
        io_changed.wait_for(lock, busy ? kIoPoll : kIoIdle,
                            [&] { return io_stop || drain_requested != request; });
    }
}

void RecorderImpl::drain_io() {
    std::unique_lock lock(io_mutex);
    if (!io_running || io_thread.get_id() == std::this_thread::get_id()) {
        return;
    }
    const std::uint64_t request = ++drain_requested;
    io_changed.notify_all();
    io_changed.wait(lock, [&] { return drain_done >= request || !io_running || io_stop; });
}

bool RecorderImpl::process_available() {
    for (;;) {
        std::shared_ptr<StreamContext> ctx;
        std::int64_t gap = 0;
        {
            const std::lock_guard lock(mutex);
            if (items.empty()) {
                return false;
            }
            if (items.front().ctx) {
                ctx = items.front().ctx;
            } else {
                gap = items.front().gap_frames;
                items.pop_front();
            }
        }
        if (!ctx) {
            append_gap(gap);
            continue;
        }
        // Read before draining: a stream left behind keeps nothing it hands over afterwards.
        const bool left_behind = !ctx->accepting.load(std::memory_order_acquire);
        ctx->ring.drain(
            [&](std::span<const std::int16_t> samples, std::size_t frames, BlockFlags flags) {
                try {
                    process_block(samples, frames, flags);
                } catch (const std::exception& error) {
                    // Python's callback hands this to the watcher rather than log on the device
                    // thread; it is reported the same way here.
                    const std::lock_guard lock(mutex);
                    if (journal) {
                        journal->report_error(std::string("Audio capture failed: ") + error.what());
                    }
                    stop_for_capture_error_locked();
                }
            });
        process_overrun(*ctx);
        if (!left_behind) {
            return true;
        }
        const std::lock_guard lock(mutex);
        if (!items.empty() && items.front().ctx == ctx) {
            items.pop_front();
        }
    }
}

void RecorderImpl::process_block(std::span<const std::int16_t> samples, std::size_t frames,
                                 BlockFlags flags) {
    std::optional<float> report;
    {
        const std::lock_guard lock(mutex);
        if (!journal || canceled.load() || capture_failed.load()) {
            return;
        }
        // Overflow is lost audio, not a lost device: switching microphones on CPU starvation
        // would only hide it. Device loss shows up as a stall or a dead stream.
        if (flags.overflow || flags.underflow) {
            if (const std::optional<std::string> error = status_error(flags)) {
                dropped.fetch_add(frames);
                journal->report_error(*error);
                stop_for_capture_error_locked();
                return;
            }
        }
        if (flags.paused) {
            return;
        }
        const std::span<const std::byte> payload = std::as_bytes(samples);
        if (!journal->append(payload)) {
            dropped.fetch_add(frames);
            stop_for_capture_error_locked();
            return;
        }
        recorded_bytes += payload.size();
        recorded_frames += static_cast<std::int64_t>(frames);
        if (tape) {
            tape->append(samples);
        }
        if (gate.observe(dsp::block_level_db(samples), static_cast<std::int64_t>(frames))) {
            end_post_roll_locked("quiet");
        }
        if (!samples.empty()) {
            double sum = 0.0;
            for (const std::int16_t sample : samples) {
                sum += static_cast<double>(sample) * static_cast<double>(sample);
            }
            const double rms = std::sqrt(sum / static_cast<double>(samples.size())) / 32767.0;
            current_level =
                WAVEFORM_LEVEL_SMOOTHING * current_level + (1.0 - WAVEFORM_LEVEL_SMOOTHING) * rms;
            current_level = std::clamp(current_level, 0.0, 1.0);
            report = static_cast<float>(current_level);
        }
    }
    if (report) {
        // _calculate_and_report_level: a meter that fails costs its reading, never the recording.
        try {
            level.emit(*report);
        } catch (const std::exception& error) {
            kLog.debug("Error calculating audio level: {}", error.what());
        }
    }
}

void RecorderImpl::process_overrun(StreamContext& ctx) {
    if (!ctx.overrun.load(std::memory_order_acquire)) {
        return;
    }
    const std::uint64_t frames = ctx.overrun_frames.exchange(0);
    const std::lock_guard lock(mutex);
    if (!journal || canceled.load()) {
        return;
    }
    dropped.fetch_add(frames);
    if (!ctx.overrun_reported) {
        ctx.overrun_reported = true;
        journal->report_error(STORAGE_CANNOT_KEEP_UP, true);
        stop_for_capture_error_locked();
    }
}

// _append_switch_gap: real silence, so it is never dropped frames.
void RecorderImpl::append_gap(std::int64_t frames) {
    const std::lock_guard lock(mutex);
    if (!journal || frames <= 0) {
        return;
    }
    const std::vector<std::int16_t> silence(static_cast<std::size_t>(frames * channels), 0);
    if (journal->append(std::as_bytes(std::span(silence)))) {
        recorded_bytes += silence.size() * sizeof(std::int16_t);
        recorded_frames += frames;
        if (tape) {
            tape->append(silence);
        }
    }
}

// _status_error: why a flagged block ends the recording. An input overflow discarded audio
// before this block, which is itself real audio, so a lone one costs a short gap rather than
// the dictation.
std::optional<std::string> RecorderImpl::status_error(const BlockFlags& flags) {
    if (!flags.overflow || flags.underflow) {
        return "Audio input error (" + describe_flags(flags) + "); recording stopped.";
    }
    overflows.fetch_add(1);
    const Steady::time_point now = Steady::now();
    overflow_times.push_back(now);
    while (now - overflow_times.front() > options.timings.overflow_window) {
        overflow_times.pop_front();
    }
    if (std::cmp_greater_equal(overflow_times.size(), options.timings.overflow_limit)) {
        return std::string("Audio input kept overflowing because the computer could not keep up; "
                           "recording stopped.");
    }
    return std::nullopt;
}

// --- capture state -------------------------------------------------------------------------

void RecorderImpl::end_post_roll_locked(std::string_view reason) {
    if (session && !session->post_roll_end.is_set()) {
        post_roll_end_reason = reason;
        session->post_roll_end.set();
    }
}

void RecorderImpl::end_post_roll(std::string_view reason) {
    const std::lock_guard lock(mutex);
    end_post_roll_locked(reason);
}

void RecorderImpl::stop_for_capture_error_locked() {
    if (session) {
        session->stop.set();
    }
    end_post_roll_locked("capture error");
}

// _fail_capture: one visible failure per recording, delivered outside the device thread.
void RecorderImpl::fail_capture(const std::string& message) {
    {
        const std::lock_guard lock(error_mutex);
        if (!last_capture_error.empty() || canceled.load()) {
            return;
        }
        last_capture_error = message;
        capture_failed.store(true);
    }
    kLog.error("{}", message);
    {
        const std::lock_guard lock(mutex);
        stop_for_capture_error_locked();
    }
    try {
        capture_error.emit(message);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Recording failure notification failed");
    }
}

std::shared_ptr<RecordingJournal> RecorderImpl::current_journal() const {
    const std::lock_guard lock(mutex);
    return journal;
}

// --- devices -------------------------------------------------------------------------------

std::shared_ptr<OpenStream>
RecorderImpl::open_stream(const std::optional<audio::InputDevice>& device, bool accepting) {
    auto open = std::make_shared<OpenStream>();
    open->ctx =
        std::make_shared<StreamContext>(this, static_cast<std::size_t>(2 * chunk * channels));
    open->ctx->accepting.store(accepting);
    audio::StreamConfig config = audio::input_config(device ? &*device : nullptr, rate, channels);
    config.frames_per_block = chunk;
    StreamContext* raw = open->ctx.get();
    auto opened = backend.open_capture(config, &on_block, raw, [raw](const std::string&) {
        // The device thread: no logging and no audio calls here.
        if (raw->accepting.load(std::memory_order_acquire)) {
            raw->lost.store(true);
        }
    });
    if (!opened) {
        throw std::runtime_error(opened.error());
    }
    open->stream = std::move(*opened);
    return open;
}

std::pair<std::shared_ptr<OpenStream>, std::optional<audio::InputDevice>>
RecorderImpl::open_first() {
    std::optional<std::string> last_error;
    Cursor cursor(*this);
    while (const std::optional<Candidate> candidate = cursor.next()) {
        try {
            return {open_stream(candidate->device, true), candidate->device};
        } catch (const std::exception& error) {
            last_error = error.what();
            kLog.warning("Couldn't open {}: {}", describe(candidate->device), error.what());
            mark_failed(candidate->device);
        }
    }
    throw std::runtime_error(last_error.value_or("No audio device available"));
}

void RecorderImpl::mark_failed(std::optional<audio::InputDevice> device) {
    if (!device) {
        default_failed = true;
        // The default's key is excluded too, so the ranking does not offer it again.
        try {
            device = audio::default_input(audio::list_input_devices(backend));
        } catch (const std::exception& error) {
            kLog.debug("Default microphone lookup failed; using PortAudio's default ({})",
                       error.what());
            device.reset();
        }
        if (!device) {
            return;
        }
    }
    failed_indexes.insert(device->index);
    if (!device->name.empty()) {
        failed_keys.push_back(device->key());
    }
}

std::string RecorderImpl::describe(const std::optional<audio::InputDevice>& device) {
    if (!device) {
        return "the default input";
    }
    std::string shown = device->display();
    return shown.empty() ? std::format("input device {}", device->index) : shown;
}

// The microphone's name for the overlay; "" means the system default.
std::string RecorderImpl::switch_name(const std::optional<audio::InputDevice>& device) {
    if (!device || device->name.empty() || device->role == audio::kRoleMapper) {
        return "";
    }
    return audio::short_name(device->label.empty() ? device->name : device->label);
}

void RecorderImpl::retire_stream(std::shared_ptr<OpenStream> old) {
    if (!old) {
        return;
    }
    auto closed = std::make_shared<Event>();
    std::thread([old = std::move(old), closed]() mutable {
        recorder_detail::set_thread_name("fv-recorder-close-lost-mic");
        close_stream(*old);
        old.reset();
        closed->set();
    }).detach();
    if (!closed->wait_for(options.timings.lost_stream_close)) {
        kLog.warning("The lost microphone's stream is still closing");
    }
}

void RecorderImpl::unwind_failed_stream() {
    std::shared_ptr<OpenStream> open;
    {
        const std::lock_guard lock(mutex);
        open = std::move(stream);
        if (open) {
            open->ctx->accepting.store(false);
        }
    }
    if (open) {
        close_stream(*open);
    }
}

// --- watcher -------------------------------------------------------------------------------

// _wait_for_stop: keeps the stream open until the stop and the post-roll complete. It sleeps on
// events rather than polling, so the stream closes as soon as the tail is quiet.
void RecorderImpl::watch(const std::shared_ptr<Session>& current) {
    recorder_detail::set_thread_name("fv-recorder-watch");
    std::string reason = "cap";
    try {
        kLog.info("Audio stream started");
        watch_capture(*current);
        if (const auto spool = current_journal()) {
            if (const auto error = spool->error()) {
                fail_capture(*error);
            }
        }
        Steady::duration remaining{0};
        {
            const std::lock_guard lock(mutex);
            if (post_roll_deadline) {
                remaining = std::max(Steady::duration{0}, *post_roll_deadline - Steady::now());
            }
        }
        if (current->post_roll_end.wait_for(remaining)) {
            const std::lock_guard lock(mutex);
            reason = post_roll_end_reason.empty() ? "quiet" : post_roll_end_reason;
        }
    } catch (const std::exception& error) {
        kLog.warning("Audio capture watcher failed ({})", error.what());
        fail_capture(std::string("Audio capture stopped unexpectedly: ") + error.what());
    }
    const auto quietly = [](const auto& step) {
        try {
            step();
        } catch (const std::exception& error) {
            kLog.error_with(error, "Finishing the recording failed");
        }
    };
    quietly([&] { close_capture_stream(); });
    quietly([&] { log_post_roll(reason); });
    quietly([&] { finish_capture_journal(); });
    quietly([&] { report_capture_stats(); });
    {
        const std::lock_guard lock(mutex);
        stop_requested = false;
        stop_requested_at.reset();
        post_roll_deadline.reset();
        recording.store(false);
    }
    current->complete.set();
}

// Until the stop: ends on a storage error and moves to the next microphone when one is lost.
void RecorderImpl::watch_capture(Session& current) {
    while (!current.stop.wait_for(options.timings.watch_interval)) {
        if (const auto spool = current_journal()) {
            if (const auto error = spool->error()) {
                fail_capture(*error);
                return;
            }
        }
        if (capture_lost() && !switch_device(current)) {
            fail_capture(std::string(NO_MICROPHONE_LEFT));
            return;
        }
    }
}

// The microphone is gone: its stream ended or went inactive, or its blocks stopped.
bool RecorderImpl::capture_lost() {
    std::shared_ptr<OpenStream> open;
    {
        const std::lock_guard lock(mutex);
        open = stream;
    }
    if (open && (open->ctx->lost.load() || !open->stream->active())) {
        return true;
    }
    const Steady::time_point now = Steady::now();
    const std::lock_guard lock(mutex);
    if (open) {
        const std::uint64_t blocks = open->ctx->blocks.load();
        if (blocks != last_blocks_seen) {
            last_blocks_seen = blocks;
            last_callback_at = now;
        }
    }
    // Unset until the current stream delivers its first block.
    if (last_callback_at) {
        return now - *last_callback_at > options.timings.stall_after_blocks;
    }
    return now - stream_started_at > options.timings.first_block_grace;
}

// _switch_device: carries the recording on to the next microphone after losing this one. The
// replacement opens at the same format before the dead stream is closed and appends to the
// same journal, so everything captured so far stays in this recording. False only when no
// microphone is left to open.
bool RecorderImpl::switch_device(Session& current) {
    std::shared_ptr<OpenStream> old;
    std::optional<audio::InputDevice> old_device;
    {
        const std::lock_guard lock(mutex);
        old = stream;
        old_device = active_device;
        if (old) {
            old->ctx->accepting.store(false);
        }
    }
    // The lost microphone's last blocks go in before the gap.
    drain_io();
    mark_failed(old_device);
    std::shared_ptr<OpenStream> fresh;
    std::optional<audio::InputDevice> fresh_device;
    Cursor cursor(*this);
    while (const std::optional<Candidate> candidate = cursor.next()) {
        if (current.stop.is_set() || canceled.load()) {
            break;
        }
        try {
            // Not accepting until installed below, so none of its blocks reach the journal
            // ahead of the gap.
            fresh = open_stream(candidate->device, false);
            fresh_device = candidate->device;
            break;
        } catch (const std::exception& error) {
            kLog.warning("Couldn't switch to {}: {}", describe(candidate->device), error.what());
            mark_failed(candidate->device);
        }
    }
    const bool ended = current.stop.is_set() || canceled.load();
    if (ended || !fresh) {
        if (fresh) {
            close_stream(*fresh);
        }
        {
            // Detached, so the watcher's own close never touches it.
            const std::lock_guard lock(mutex);
            stream.reset();
        }
        retire_stream(std::move(old));
        if (!ended) {
            return false;
        }
        // Ended while switching: keep what was captured, record no more.
        end_post_roll("microphone lost");
        return true;
    }
    {
        const std::lock_guard lock(mutex);
        stream = fresh;
        active_device = fresh_device;
        if (!stop_requested) {
            // Two microphones' noise floors would make a meaningless quiet threshold.
            gate = PostRollGate(rate, options.post_roll);
        }
        items.push_back(IoItem{nullptr, options.timings.switch_gap.count() * rate / 1000});
        items.push_back(IoItem{fresh->ctx, 0});
        fresh->ctx->accepting.store(true, std::memory_order_release);
        last_callback_at.reset();
        last_blocks_seen = 0;
        stream_started_at = Steady::now();
        ++device_switches;
    }
    io_changed.notify_all();
    retire_stream(std::move(old));
    kLog.info("{} stopped delivering audio; recording continues on {}", describe(old_device),
              describe(fresh_device));
    try {
        device_switched.emit(switch_name(old_device), switch_name(fresh_device));
    } catch (const std::exception& error) {
        kLog.error_with(error, "Microphone switch notification failed");
    }
    return true;
}

void RecorderImpl::close_capture_stream() {
    std::shared_ptr<OpenStream> open;
    {
        const std::lock_guard lock(mutex);
        open = std::move(stream);
        if (open) {
            open->ctx->accepting.store(false);
        }
    }
    if (open) {
        try {
            if (open->stream) {
                open->stream->abort();
            }
            open->stream.reset();
            kLog.info("Audio stream stopped and closed");
        } catch (const std::exception& error) {
            kLog.error("Error closing audio stream: {}", error.what());
        }
        open.reset();
    }
    // What the stream handed over before it closed goes into the journal before it finishes.
    drain_io();
}

void RecorderImpl::finish_capture_journal() {
    const auto spool = current_journal();
    if (!spool) {
        return;
    }
    if (!spool->finish()) {
        fail_capture("Recording storage did not finish; audio kept for recovery.");
    } else if (const auto error = spool->error()) {
        fail_capture(*error);
    }
}

void RecorderImpl::report_capture_stats() {
    const std::uint64_t overflow_count = overflows.load();
    bool failed = false;
    {
        const std::lock_guard lock(error_mutex);
        failed = !last_capture_error.empty();
    }
    if (overflow_count != 0 && !failed) {
        kLog.warning(
            "Audio input overflowed {} time(s); the recording carried on past each short gap",
            overflow_count);
    }
    std::int64_t captured = 0;
    int switches = 0;
    {
        const std::lock_guard lock(mutex);
        captured = recorded_frames;
        switches = device_switches;
    }
    metrics::record("captured_frames", static_cast<double>(captured));
    metrics::record("dropped_frames", static_cast<double>(dropped.load()));
    metrics::record("device_switches", static_cast<double>(switches));
    metrics::record("input_overflows", static_cast<double>(overflow_count));
}

// One INFO line per stop, so log audits can measure the post-roll.
void RecorderImpl::log_post_roll(std::string_view reason) {
    double elapsed_ms = 0.0;
    double audio_ms = 0.0;
    std::string detail;
    {
        const std::lock_guard lock(mutex);
        if (!stop_requested_at) {
            return;
        }
        elapsed_ms = milliseconds_between(*stop_requested_at, Steady::now());
        audio_ms = static_cast<double>(gate.frames_after_stop()) * 1000.0 / rate;
        detail = gate.describe();
    }
    kLog.info("Post-roll ended by {} after {} ms ({} ms of audio after stop; {})", reason,
              py::format_fixed(elapsed_ms, 0), py::format_fixed(audio_ms, 0), detail);
}

// --- the facade ----------------------------------------------------------------------------

bool RecorderImpl::start_recording() {
    const std::lock_guard start_lock(start_mutex);
    std::shared_ptr<Session> previous;
    {
        const std::lock_guard lock(mutex);
        previous = session;
    }
    if (recording.load() || (previous && !previous->complete.is_set())) {
        kLog.warning("Recording already in progress");
        return false;
    }
    if (previous && previous->watcher.joinable() &&
        previous->watcher.get_id() != std::this_thread::get_id()) {
        previous->watcher.join();
    }
    const auto fresh = std::make_shared<Session>();
    {
        const std::lock_guard lock(mutex);
        last_start_error.clear();
        session = fresh;
        post_roll_end_reason.clear();
    }
    try {
        clear_recording_data(false, true);
        failed_keys.clear();
        failed_indexes.clear();
        default_failed = false;
        std::uint64_t token = 0;
        {
            const std::lock_guard lock(mutex);
            gate = PostRollGate(rate, options.post_roll);
            canceled.store(false);
            paused.store(false);
            items.clear();
            device_switches = 0;
            overflow_times.clear();
            last_callback_at.reset();
            last_blocks_seen = 0;
            token = session_token;
        }
        {
            const std::lock_guard lock(error_mutex);
            last_capture_error.clear();
            capture_failed.store(false);
        }
        dropped.store(0);
        overflows.store(0);
        std::weak_ptr<RecorderImpl> weak = weak_from_this();
        auto spool = std::make_shared<RecordingJournal>(
            options.output_file, rate, channels, RECORDER_SAMPLE_WIDTH,
            [weak, token](const std::string& message) {
                // A previous recording's writer can never stop this one.
                const auto self = weak.lock();
                if (!self) {
                    return;
                }
                bool current_session = false;
                {
                    const std::lock_guard lock(self->mutex);
                    current_session = token == self->session_token;
                }
                if (current_session) {
                    self->fail_capture(message);
                }
            },
            options.journal);
        {
            const std::lock_guard lock(mutex);
            journal = std::move(spool);
            tape = std::make_shared<audio::PcmTape>(rate, channels);
        }
        ensure_io_thread();

        auto [open, device] = open_first();
        {
            const std::lock_guard lock(mutex);
            stream = open;
            active_device = device;
            items.push_back(IoItem{open->ctx, 0});
            stream_started_at = Steady::now();
            stop_requested = false;
            stop_requested_at.reset();
            post_roll_deadline.reset();
        }
        io_changed.notify_all();
        recording.store(true);
        fresh->watcher = std::thread([self = shared_from_this(), fresh] { self->watch(fresh); });
        kLog.info("Recording started - audio stream open");
        return true;
    } catch (const std::exception& error) {
        {
            const std::lock_guard lock(mutex);
            last_start_error = Recorder::format_start_error(error.what());
        }
        kLog.error("Failed to start recording: {}", error.what());
        unwind_failed_stream();
        recording.store(false);
        fresh->complete.set();
        try {
            clear_recording_data(false, true);
        } catch (const std::exception& clear_error) {
            kLog.error_with(clear_error, "Could not release the failed recording's journal");
        }
        return false;
    }
}

bool RecorderImpl::stop_recording() {
    if (!recording.load()) {
        kLog.warning("No recording in progress");
        return false;
    }
    try {
        std::shared_ptr<Session> current;
        bool repeated = false;
        {
            const std::lock_guard lock(mutex);
            repeated = stop_requested;
            if (!repeated) {
                gate.arm();
                stop_requested = true;
                stop_requested_at = Steady::now();
                post_roll_deadline = *stop_requested_at + options.timings.post_roll;
                current = session;
            }
        }
        // Logged outside the capture lock (architecture section 4, rule 6).
        if (repeated) {
            kLog.debug("Recording stop already requested; post-roll continues");
            return true;
        }
        if (current) {
            current->stop.set();
        }
        if (paused.load()) {
            // Nobody is mid-word: there is no tail to wait for.
            end_post_roll("paused");
        }
        kLog.info("Recording stop requested, post-roll continuing in background");
        return true;
    } catch (const std::exception& error) {
        kLog.error("Failed to stop recording: {}", error.what());
        return false;
    }
}

bool RecorderImpl::wait_for_stop_completion(std::optional<std::chrono::milliseconds> timeout) {
    std::shared_ptr<Session> current;
    {
        const std::lock_guard lock(mutex);
        current = session;
    }
    if (!current || current->complete.is_set()) {
        return true;
    }
    // The writer drains and syncs after the stream closes; its allowed finish time must fit
    // inside the caller's overall stop deadline.
    const std::chrono::milliseconds wait =
        timeout.value_or(options.timings.post_roll + options.timings.finalize_grace +
                         options.journal.finish_timeout);
    const bool finished = current->complete.wait_for(wait);
    if (!finished) {
        kLog.warning("Recording did not finish capture and storage within {}s; audio kept for "
                     "recovery",
                     py::format_fixed(static_cast<double>(wait.count()) / 1000.0, 2));
    }
    return finished;
}

bool RecorderImpl::save_recording(std::optional<std::filesystem::path> file,
                                  bool allow_incomplete) {
    const std::filesystem::path target = file && !file->empty() ? *file : options.output_file;
    // Blocks the device already handed over belong to this save.
    drain_io();
    const std::shared_ptr<RecordingJournal> spool = current_journal();
    if (!capture_saveable(spool, allow_incomplete)) {
        return false;
    }
    const auto padding_frames = static_cast<std::size_t>(
        static_cast<double>(rate) *
        (static_cast<double>(options.timings.end_padding.count()) / 1000.0));
    const std::size_t padding_bytes =
        padding_frames * static_cast<std::size_t>(channels * RECORDER_SAMPLE_WIDTH);
    try {
        audio::WavWriter writer(
            target, {.rate = rate, .channels = channels, .sample_width = RECORDER_SAMPLE_WIDTH});
        if (!spool || spool->written_bytes() == 0) {
            writer.discard();
            kLog.warning("No audio data to save");
            return false;
        }
        const std::uint64_t recorded = spool->written_bytes();
        const std::uint64_t sample_frames =
            recorded / static_cast<std::uint64_t>(channels * RECORDER_SAMPLE_WIDTH);
        spool->copy_into(writer);
        if (padding_frames > 0) {
            const std::vector<std::int16_t> silence(
                padding_frames * static_cast<std::size_t>(channels), 0);
            writer.write(std::span<const std::int16_t>(silence));
        }
        writer.commit(local_seconds_now(clock));
        if (padding_bytes > 0) {
            kLog.info("Appended {}ms of silence to protect the tail of the recording",
                      options.timings.end_padding.count());
        }
        double duration = 0.0;
        {
            const std::lock_guard lock(mutex);
            duration = static_cast<double>(recorded_frames) / rate;
        }
        kLog.info("Audio saved to {} at {:%Y-%m-%d %H:%M:%S} - {} sample frames, {} bytes, {}s",
                  path_to_utf8(target), local_seconds_now(clock), sample_frames,
                  recorded + padding_bytes, py::format_fixed(duration, 2));
        return true;
    } catch (const std::exception& error) {
        kLog.error("Failed to save audio to {}: {}", path_to_utf8(target), error.what());
        return false;
    }
}

// _capture_saveable: finishes the journal and decides whether its audio may be saved; reports
// any loss.
bool RecorderImpl::capture_saveable(const std::shared_ptr<RecordingJournal>& spool,
                                    bool allow_incomplete) {
    if (spool && !spool->finish()) {
        fail_capture("Recording storage did not finish; audio kept for recovery.");
        return false;
    }
    if (spool) {
        if (const auto error = spool->error()) {
            fail_capture(*error);
            return false;
        }
    }
    bool failed = false;
    {
        const std::lock_guard lock(error_mutex);
        failed = !last_capture_error.empty();
    }
    if (dropped.load() != 0 && !failed) {
        fail_capture("Audio blocks were lost; the partial recording was kept.");
    }
    {
        const std::lock_guard lock(error_mutex);
        failed = !last_capture_error.empty();
    }
    if (failed && !allow_incomplete) {
        kLog.warning("Refusing to save an incomplete recording as complete");
        return false;
    }
    return true;
}

void RecorderImpl::clear_recording_data(bool discard, bool wait) {
    std::shared_ptr<RecordingJournal> spool;
    std::uint64_t old_bytes = 0;
    {
        const std::lock_guard lock(mutex);
        old_bytes = recorded_bytes;
        ++session_token;
        spool = std::move(journal);
        tape.reset();
        recorded_bytes = 0;
        recorded_frames = 0;
    }
    if (spool) {
        if (wait) {
            spool->close(discard);
        } else {
            if (discard) {
                spool->request_discard();
            }
            auto released = std::make_shared<Event>();
            {
                const std::lock_guard lock(release_mutex);
                std::erase_if(releases, [](const auto& done) { return done->is_set(); });
                releases.push_back(released);
            }
            std::thread([spool, discard, released] {
                recorder_detail::set_thread_name("fv-recorder-release");
                try {
                    spool->close(discard);
                } catch (const std::exception& error) {
                    kLog.error_with(error, "Could not release recording journal");
                }
                released->set();
            }).detach();
        }
    }
    kLog.info("Cleared recording data. Old byte count: {}", old_bytes);
}

// A bare clear would let the stream refill a fresh journal until the post-roll ends, so a
// stop pressed in that window would save and paste audio the user had just discarded.
void RecorderImpl::cancel_recording() {
    canceled.store(true);
    if (recording.load()) {
        stop_recording();
        end_post_roll("cancel");
    }
    clear_recording_data(true, false);
}

void RecorderImpl::cleanup() {
    try {
        if (recording.load()) {
            stop_recording();
            // The data is cleared below, so nothing needs the post-roll.
            end_post_roll("cleanup");
            std::shared_ptr<Session> current;
            {
                const std::lock_guard lock(mutex);
                current = session;
            }
            if (current) {
                if (!current->complete.wait_for(options.timings.cleanup_join)) {
                    kLog.warning("Recording thread did not finish during cleanup timeout");
                } else if (current->watcher.joinable() &&
                           current->watcher.get_id() != std::this_thread::get_id()) {
                    current->watcher.join();
                }
            }
        }
        unwind_failed_stream();
        clear_recording_data(false, true);
        std::vector<std::shared_ptr<Event>> pending;
        {
            const std::lock_guard lock(release_mutex);
            pending = releases;
        }
        for (const auto& released : pending) {
            released->wait_for(options.timings.cleanup_join);
        }
        kLog.info("Audio recorder cleaned up");
    } catch (const std::exception& error) {
        kLog.warning("Error during audio recorder cleanup: {}", error.what());
    }
}

} // namespace recorder_detail

// --- Recorder ------------------------------------------------------------------------------

namespace {

RecorderOptions resolved(RecorderOptions options, const Paths& paths) {
    if (options.output_file.empty()) {
        options.output_file = paths.user_file(RECORDED_AUDIO_FILE);
    }
    if (options.journal.clock == nullptr) {
        options.journal.clock = options.clock;
    }
    return options;
}

} // namespace

Recorder::Recorder(audio::Backend& backend, const Paths& paths, RecorderOptions options)
    : impl_(std::make_shared<recorder_detail::RecorderImpl>(backend,
                                                            resolved(std::move(options), paths))),
      level(impl_->level), capture_error(impl_->capture_error),
      device_switched(impl_->device_switched) {
    kLog.info("Audio recorder initialized");
}

Recorder::~Recorder() {
    impl_->cleanup();
    impl_->stop_io_thread();
    std::shared_ptr<recorder_detail::Session> current;
    {
        const std::lock_guard lock(impl_->mutex);
        current = impl_->session;
    }
    if (current && current->watcher.joinable()) {
        if (current->complete.is_set() && current->watcher.get_id() != std::this_thread::get_id()) {
            current->watcher.join();
        } else {
            // A watcher stuck in a driver keeps the recorder's state alive until it returns.
            current->watcher.detach();
        }
    }
}

std::unique_ptr<Recorder> Recorder::from_settings(SettingsStore& settings, audio::Backend& backend,
                                                  const Paths& paths, Executor& io) {
    if (!audio::legacy_migration_pending(*settings.snapshot())) {
        RecorderOptions options;
        options.device_priority = audio::load_priority(settings, backend);
        return std::make_unique<Recorder>(backend, paths, std::move(options));
    }
    auto recorder = std::make_unique<Recorder>(backend, paths);
    std::weak_ptr<recorder_detail::RecorderImpl> weak = recorder->impl_;
    // _adopt_migrated_priority: lists devices, so never on the calling (GUI) thread.
    io.post([weak, &settings, &backend] {
        std::vector<audio::DeviceKey> priority;
        try {
            priority = audio::load_priority(settings, backend);
        } catch (const std::exception& error) {
            kLog.warning("Couldn't migrate the saved microphone ({})", error.what());
            return;
        }
        if (priority.empty()) {
            return;
        }
        if (const auto impl = weak.lock()) {
            const std::lock_guard lock(impl->mutex);
            impl->device_priority = audio::normalize_priority(priority);
        }
    });
    return recorder;
}

std::vector<audio::InputDevice> Recorder::get_input_devices(audio::Backend& backend) {
    try {
        return audio::list_input_devices(backend);
    } catch (const std::exception& error) {
        kLog.error("Failed to enumerate audio devices: {}", error.what());
        return {};
    }
}

std::string Recorder::format_start_error(std::string_view message) {
    const std::string lowered = ascii_lower(message);
    for (const std::string_view token : {"querying device", "no such device", "invalid device",
                                         "device unavailable", "no default input", "portaudio"}) {
        if (lowered.find(token) != std::string::npos) {
            return "No audio device available";
        }
    }
    return message.empty() ? std::string("Could not open the audio stream") : std::string(message);
}

bool Recorder::start_recording() {
    return impl_->start_recording();
}

std::string Recorder::last_start_error() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->last_start_error;
}

bool Recorder::stop_recording() {
    return impl_->stop_recording();
}

bool Recorder::pause() {
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->recording.load() || impl_->stop_requested || impl_->canceled.load()) {
            return false;
        }
        impl_->paused.store(true);
    }
    kLog.info("Recording paused");
    return true;
}

void Recorder::resume() {
    {
        const std::lock_guard lock(impl_->mutex);
        if (!impl_->paused.load()) {
            return;
        }
        impl_->paused.store(false);
    }
    kLog.info("Recording resumed");
}

bool Recorder::paused() const {
    return impl_->paused.load();
}

bool Recorder::wait_for_stop_completion(std::optional<std::chrono::milliseconds> timeout) {
    return impl_->wait_for_stop_completion(timeout);
}

bool Recorder::save_recording(std::optional<std::filesystem::path> file, bool allow_incomplete) {
    return impl_->save_recording(std::move(file), allow_incomplete);
}

double Recorder::get_recording_duration() const {
    const std::lock_guard lock(impl_->mutex);
    return static_cast<double>(impl_->recorded_frames) / impl_->rate;
}

bool Recorder::has_recording_data() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->recorded_bytes > 0;
}

std::optional<std::string> Recorder::read_recorded_bytes(std::int64_t offset) const {
    const auto spool = impl_->current_journal();
    if (!spool) {
        return offset == 0 ? std::optional<std::string>(std::string()) : std::nullopt;
    }
    return spool->read_from(offset);
}

void Recorder::cancel_recording() {
    impl_->cancel_recording();
}

bool Recorder::capture_canceled() const {
    return impl_->canceled.load();
}

bool Recorder::is_recording() const {
    return impl_->recording.load();
}

void Recorder::clear_recording_data(bool discard, bool wait) {
    impl_->clear_recording_data(discard, wait);
}

void Recorder::acknowledge_recording() {
    impl_->clear_recording_data(true, true);
}

void Recorder::cleanup() {
    impl_->cleanup();
}

const std::filesystem::path& Recorder::output_file() const {
    return impl_->options.output_file;
}

std::shared_ptr<const audio::PcmTape> Recorder::tape() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->tape;
}

std::shared_ptr<RecordingJournal> Recorder::journal() const {
    return impl_->current_journal();
}

std::uint64_t Recorder::dropped_frames() const {
    return impl_->dropped.load();
}

std::uint64_t Recorder::input_overflows() const {
    return impl_->overflows.load();
}

std::string Recorder::last_capture_error() const {
    const std::lock_guard lock(impl_->error_mutex);
    return impl_->last_capture_error;
}

void Recorder::fail_capture(const std::string& message) {
    impl_->fail_capture(message);
}

std::optional<audio::InputDevice> Recorder::active_device() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->active_device;
}

std::vector<audio::DeviceKey> Recorder::device_priority() const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->device_priority;
}

void Recorder::set_device_priority(std::vector<audio::DeviceKey> priority) {
    const std::lock_guard lock(impl_->mutex);
    impl_->device_priority = audio::normalize_priority(priority);
}

RecorderSnapshot Recorder::snapshot() const {
    const std::lock_guard lock(impl_->mutex);
    RecorderSnapshot shot;
    shot.recorded_frames = impl_->recorded_frames;
    shot.recorded_bytes = impl_->recorded_bytes;
    shot.post_roll_gate = impl_->gate;
    shot.post_roll_end_reason = impl_->post_roll_end_reason;
    shot.stop_requested = impl_->stop_requested;
    if (impl_->stop_requested_at && impl_->post_roll_deadline) {
        shot.post_roll_cap = *impl_->post_roll_deadline - *impl_->stop_requested_at;
    }
    shot.device_switches = impl_->device_switches;
    return shot;
}

bool Recorder::sync_capture(std::chrono::milliseconds timeout) {
    impl_->drain_io();
    const auto spool = impl_->current_journal();
    return !spool || spool->wait_until_written(timeout);
}

int Recorder::rate() const {
    return impl_->rate;
}

int Recorder::channels() const {
    return impl_->channels;
}

int Recorder::chunk() const {
    return impl_->chunk;
}

} // namespace fv
