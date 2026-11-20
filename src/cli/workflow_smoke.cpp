#include "cli/workflow_smoke.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <exception>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <variant>

#include "audio/file_decoder.h"
#include "audio/pcm_tape.h"
#include "audio/wav_writer.h"
#include "backup/create.h"
#include "backup/restore.h"
#include "backup/startup_gate.h"
#include "base/clock.h"
#include "base/data_lease.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "base/utf8.h"
#include "cli/detail/text_mode.h"
#include "db/database.h"
#include "engine/error.h"
#include "engine/version.h"
#include "history/entry.h"
#include "history/repository.h"
#include "pyport/json.h"
#include "recorder/journal.h"
#include "speech/service.h"
#include "speech/window_preview.h"

namespace fv::cli {
namespace {

using Steady = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr log::Logger kLog{"services.release_checks"};

constexpr int kRate = 16000;
constexpr std::int16_t kAmplitude = 1000;
constexpr std::string_view kFixtureText = "synthetic fixture";
// Every wait of the smoke but the first: the decoder's release, a worker's exit, a delivery.
constexpr auto kDeadline = 2s;
// The fixture engine's load; release_checks.py waits on nothing there, and the gate allows
// fixture_ready_s up to 15 s.
constexpr auto kReadyDeadline = 15s;
// How long an idle event loop sleeps before it looks again (release_checks.py's time.sleep).
constexpr auto kIdle = 1ms;
constexpr std::string_view kTimestamp = "2000-01-01T00:00:00";

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

double seconds_since(Steady::time_point start) {
    return std::chrono::duration<double>(Steady::now() - start).count();
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(), [](char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    });
    return out;
}

// The UI thread's queue as the smoke drives it: posts and timers wait until the smoke's own
// thread processes events, as release_checks.py's queued Qt deliveries wait for
// qt_app.processEvents().
// Affinity: post and post_after from any thread; process_events and run_until on the thread
// that made it.
class EventLoop final : public Executor {
public:
    EventLoop() : owner_(std::this_thread::get_id()) {}
    ~EventLoop() override {
        std::vector<Timer> timers;
        {
            std::lock_guard lock(mutex_);
            queue_.clear();
            timers.swap(timers_);
        }
        for (Timer& timer : timers) {
            timer.state->cancel();
        }
    }
    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    void post(Task task) override {
        {
            std::lock_guard lock(mutex_);
            queue_.push_back(std::move(task));
        }
        changed_.notify_all();
    }

    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        auto state = std::make_shared<TimerState>();
        {
            std::lock_guard lock(mutex_);
            timers_.push_back(
                Timer{Steady::now() + std::max(delay, 0ms), next_++, state, std::move(task)});
        }
        changed_.notify_all();
        return TimerHandle(std::move(state));
    }

    bool is_current() const override { return std::this_thread::get_id() == owner_; }

    // QApplication::processEvents: the tasks posted so far, then the timers due now, each once.
    void process_events() {
        std::deque<Task> tasks;
        std::vector<Timer> due;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(queue_);
            const auto now = Steady::now();
            const auto split = std::stable_partition(timers_.begin(), timers_.end(),
                                                     [now](const Timer& t) { return t.due > now; });
            due.assign(std::make_move_iterator(split), std::make_move_iterator(timers_.end()));
            timers_.erase(split, timers_.end());
        }
        std::ranges::sort(due, [](const Timer& a, const Timer& b) {
            return std::pair(a.due, a.order) < std::pair(b.due, b.order);
        });
        for (const Task& task : tasks) {
            run(task);
        }
        for (const Timer& timer : due) {
            if (timer.state->fire()) {
                run(timer.task);
            }
        }
    }

    // Processes events until `done` says so (true) or the deadline passes (false).
    bool run_until(const std::function<bool()>& done, Steady::time_point deadline) {
        for (;;) {
            process_events();
            if (done()) {
                return true;
            }
            const auto now = Steady::now();
            if (now >= deadline) {
                return false;
            }
            std::unique_lock lock(mutex_);
            changed_.wait_until(lock, std::min(deadline, now + kIdle),
                                [this] { return !queue_.empty(); });
        }
    }

private:
    struct Timer {
        Steady::time_point due;
        std::uint64_t order = 0;
        std::shared_ptr<TimerState> state;
        Task task;
    };

    static void run(const Task& task) {
        try {
            task();
        } catch (const std::exception& error) {
            kLog.error("A queued task failed: {}", error.what());
        }
    }

    const std::thread::id owner_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<Task> queue_;
    std::vector<Timer> timers_;
    std::uint64_t next_ = 0;
};

// _FixtureDecoder's events: while `block` is set, a decode announces itself and waits for the
// release (which, like a threading.Event, stays given).
// Affinity: thread-safe.
class FixtureGate {
public:
    void set_block(bool block) {
        std::lock_guard lock(mutex_);
        block_ = block;
    }
    void release() {
        {
            std::lock_guard lock(mutex_);
            released_ = true;
        }
        changed_.notify_all();
    }
    bool entered() const {
        std::lock_guard lock(mutex_);
        return entered_;
    }
    // False when a blocked decode was not released in time.
    bool pass() {
        std::unique_lock lock(mutex_);
        if (!block_) {
            return true;
        }
        entered_ = true;
        return changed_.wait_for(lock, kDeadline, [this] { return released_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool block_ = false;
    bool entered_ = false;
    bool released_ = false;
};

// The deterministic speech model: every window reads "synthetic fixture".
// Affinity: the speech thread, as any Backend.
class FixtureBackend final : public speech::Backend {
public:
    explicit FixtureBackend(std::shared_ptr<FixtureGate> gate) : gate_(std::move(gate)) {}

    std::string_view id() const override { return "fixture"; }
    speech::Capabilities capabilities() const override { return {}; }
    speech::Readiness load(const CancelToken&) override {
        return {speech::ReadyState::Ready, "Synthetic fixture", ""};
    }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        const auto* clip = std::get_if<speech::AudioClip>(&job.input);
        if (clip == nullptr || !clip->samples || clip->end <= clip->begin) {
            throw EngineError("Decoded fixture audio is empty");
        }
        if (!gate_->pass()) {
            throw EngineError("Fixture cancel did not release decoder");
        }
        speech::DecodeResult result;
        result.transcript.text = std::string(kFixtureText);
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<FixtureGate> gate_;
};

// tempfile.TemporaryDirectory(prefix="<product>-smoke-"): a new folder in the system's
// temporary folder, removed with what it holds when the smoke ends.
class TemporaryFolder {
public:
    TemporaryFolder() {
        const std::filesystem::path parent = std::filesystem::temp_directory_path();
        const std::string prefix = lower(identity().product) + "-smoke-";
        for (int attempt = 0;; ++attempt) {
            path_ = parent / (prefix + random_hex(8));
            if (std::filesystem::create_directory(path_)) {
                return;
            }
            if (attempt == 100) {
                throw std::runtime_error("No temporary folder could be created for the smoke.");
            }
        }
    }
    ~TemporaryFolder() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryFolder(const TemporaryFolder&) = delete;
    TemporaryFolder& operator=(const TemporaryFolder&) = delete;

    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

// _join_worker: the preview's window decode has ended.
void join_worker(const speech::WindowPreview& stream) {
    const auto deadline = Steady::now() + kDeadline;
    while (stream.decoding()) {
        require(Steady::now() < deadline, "Synthetic preview worker did not exit before deadline");
        std::this_thread::sleep_for(kIdle);
    }
}

std::string pcm_bytes(std::span<const std::int16_t> samples) {
    std::string bytes;
    bytes.reserve(samples.size() * 2);
    for (const std::int16_t sample : samples) {
        const auto value = static_cast<std::uint16_t>(sample);
        bytes += static_cast<char>(value & 0xFF);
        bytes += static_cast<char>(value >> 8);
    }
    return bytes;
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        throw std::runtime_error("Could not read " + path_to_utf8(path.filename()));
    }
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_file(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    out.close();
    if (!out) {
        throw std::system_error(std::make_error_code(std::errc::io_error),
                                "writing " + path_to_utf8(path.filename()));
    }
}

HistoryEntry fixture_entry(std::string id, std::string text) {
    HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = std::move(text);
    entry.timestamp = std::string(kTimestamp);
    entry.model = "fixture";
    return entry;
}

// AudioProcessor()._load_audio_data: the file's native-rate mono int16 PCM, through FFmpeg.
std::vector<std::int16_t> decode_fixture(const std::filesystem::path& path, int& rate) {
    audio::AudioFileDecoder decoder(path);
    rate = decoder.rate();
    std::vector<std::int16_t> samples;
    while (decoder.read_s16(samples)) {
    }
    return samples;
}

// _check_data_recovery.
Json check_data_recovery(const std::filesystem::path& root, const std::string& pcm) {
    const AppIdentity& app = identity();
    const std::filesystem::path source = root / "source-data";
    const std::filesystem::path target = root / "restored-data";
    std::filesystem::create_directory(source);
    std::filesystem::create_directory(target);

    std::mutex errors_mutex;
    std::vector<std::string> errors;
    {
        RecordingJournal journal(source / "recorded_audio.wav", kRate, 1, 2,
                                 [&](const std::string& error) {
                                     std::lock_guard lock(errors_mutex);
                                     errors.push_back(error);
                                 });
        std::exception_ptr failure;
        try {
            require(journal.append(std::as_bytes(std::span<const char>(pcm))),
                    "Recovery fixture could not queue audio");
            require(journal.finish(), "Recovery fixture writer did not finish");
        } catch (...) {
            failure = std::current_exception();
        }
        // The close is checked even after a failure, and its failure wins, as in Python's
        // finally clause.
        require(journal.close(), "Recovery fixture writer did not close");
        if (failure) {
            std::rethrow_exception(failure);
        }
    }
    {
        std::lock_guard lock(errors_mutex);
        require(errors.empty(), "Recovery fixture reported a storage error");
    }
    const std::vector<std::filesystem::path> recovered =
        recover_recordings(source / "recorded_audio.wav", source / "recordings");
    require(recovered.size() == 1, "Interrupted recording was not recovered exactly once");
    require(wav_data(recovered.front()) == pcm, "Recovered recording changed its audio");
    require(recover_recordings(source / "recorded_audio.wav", source / "recordings").empty(),
            "Recording recovery duplicated an earlier result");

    {
        Database database = Database::open_or_create(source / app.database_file);
        HistoryEntry entry = fixture_entry("recoverable", "backup fixture");
        entry.audio_file = path_to_utf8(recovered.front().filename());
        HistoryRepository(database).add_history_entry(entry);
        backup::CreateOptions options;
        options.data_root = source;
        options.app_version = kVersion;
        backup::create_backup(root / ("portable" + std::string(app.backup_extension)), options);
        database.close();
    }
    const std::string original_settings = R"({"before_restore": true})";
    write_file(target / app.settings_file, original_settings);
    backup::prepare_restore(root / ("portable" + std::string(app.backup_extension)), target);

    DataLease lease;
    std::exception_ptr failure;
    try {
        lease = backup::acquire_startup_data_lease(target, app);
        {
            Database database = Database::open_or_create(target / app.database_file);
            const std::optional<HistoryEntry> row =
                HistoryRepository(database).get_history_entry_by_id("recoverable");
            require(row && row->text == "backup fixture", "Restored history is missing");
            require(row->audio_file.has_value() &&
                        wav_data(target / "recordings" / path_from_utf8(*row->audio_file)) == pcm,
                    "Restored recording differs");
            database.close();
        }
        const std::optional<std::filesystem::path> previous = backup::previous_data_dir(target);
        require(previous.has_value() &&
                    read_file(*previous / app.settings_file) == original_settings,
                "Restore did not retain previous user data");
    } catch (...) {
        failure = std::current_exception();
    }
    backup::release_startup_data_lease();
    lease.release();
    if (failure) {
        std::rethrow_exception(failure);
    }
    Json checks = Json::object();
    checks["recording_recovery"] = true;
    checks["backup_restore"] = true;
    checks["previous_data_preserved"] = true;
    return checks;
}

} // namespace

std::vector<std::int16_t> fixture_samples() {
    return std::vector<std::int16_t>(kRate, kAmplitude);
}

void write_fixture_wav(const std::filesystem::path& path, std::span<const std::int16_t> samples) {
    audio::WavWriter wav(path, {kRate, 1, 2});
    wav.write(samples);
    wav.commit(std::nullopt);
}

std::string wav_data(const std::filesystem::path& path) {
    const std::string bytes = read_file(path);
    const auto u32 = [&](std::size_t at) {
        std::uint32_t value = 0;
        for (std::size_t i = 4; i-- > 0;) {
            value = (value << 8) | static_cast<unsigned char>(bytes[at + i]);
        }
        return value;
    };
    if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) {
        throw std::runtime_error(path_to_utf8(path.filename()) + " is not a WAV file");
    }
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::uint32_t size = u32(at + 4);
        if (bytes.compare(at, 4, "data") == 0) {
            const std::size_t begin = at + 8;
            return bytes.substr(begin, std::min<std::size_t>(size, bytes.size() - begin));
        }
        // Chunks are padded to an even size.
        at += 8 + std::size_t{size} + (size & 1u);
    }
    throw std::runtime_error(path_to_utf8(path.filename()) + " has no audio data");
}

double median(std::vector<double> values) {
    std::ranges::sort(values);
    const std::size_t middle = values.size() / 2;
    if (values.size() % 2 == 1) {
        return values[middle];
    }
    return (values[middle - 1] + values[middle]) / 2;
}

double percentile(std::vector<double> values, double q) {
    std::ranges::sort(values);
    const double rank = q / 100.0 * static_cast<double>(values.size() - 1);
    const auto below = static_cast<std::size_t>(std::floor(rank));
    const std::size_t above = std::min(below + 1, values.size() - 1);
    const double fraction = rank - static_cast<double>(below);
    // numpy's _lerp: a + (b - a) * t, taken from the upper end when t >= 0.5.
    const double a = values[below];
    const double b = values[above];
    const double difference = b - a;
    return fraction >= 0.5 ? b - difference * (1.0 - fraction) : a + difference * fraction;
}

std::string report_file_text(const Json& report) {
    py::DumpOptions options;
    options.indent = 2;
    const std::string text = py::dumps(report, options);
    const std::string_view newline = detail::text_mode_newline();
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (const char c : text) {
        if (c == '\n') {
            out += newline;
        } else {
            out += c;
        }
    }
    return out;
}

Json run_workflow_smoke(int repeats) {
    if (repeats < 1 || repeats > 1000) {
        throw std::invalid_argument("repeats must be between 1 and 1000");
    }
    const auto started = Steady::now();

    EventLoop loop;
    const auto gate = std::make_shared<FixtureGate>();
    speech::SpeechService service(
        [gate](const speech::BackendConfig&) -> std::unique_ptr<speech::Backend> {
            return std::make_unique<FixtureBackend>(gate);
        },
        loop);
    speech::BackendConfig config;
    config.backend = "fixture";
    config.model_label = "Synthetic fixture";
    service.select(speech::Slot::Dictation, config);
    require(loop.run_until(
                [&] {
                    return service.readiness(speech::Slot::Dictation).state ==
                           speech::ReadyState::Ready;
                },
                started + kReadyDeadline),
            "The fixture speech engine did not load");
    // StreamingTranscriber(chunk_duration_sec=.1, overlap_sec=0). The fixture is fed as one
    // block, as feed_audio queued the whole array, so one window holds all of it.
    speech::WindowPreviewOptions options;
    options.chunk_duration_s = 0.1;
    options.overlap_s = 0.0;
    options.block_frames = kRate;
    speech::WindowPreview stream(service, loop, system_clock(), options);
    const double ready_s = seconds_since(started);

    // Delivery: the (text, final) pairs the preview's callback received through the loop.
    std::vector<std::pair<std::string, bool>> values;
    const auto deliver = [&values](std::string text, bool final) {
        values.emplace_back(std::move(text), final);
    };
    std::vector<double> timings;
    double cancel_s = 0.0;
    double restart_s = 0.0;
    double cleanup_s = 0.0;
    Json checks;
    {
        const TemporaryFolder temporary;
        const std::filesystem::path& root = temporary.path();
        const std::filesystem::path audio_path = root / "fixture.wav";
        const std::vector<std::int16_t> samples = fixture_samples();
        write_fixture_wav(audio_path, samples);
        int rate = 0;
        const std::vector<std::int16_t> decoded = decode_fixture(audio_path, rate);
        require(rate == kRate && decoded.size() == samples.size(),
                "Fixture sample rate/duration changed");
        require(std::ranges::all_of(decoded,
                                    [](std::int16_t sample) {
                                        // np.allclose(atol=1, rtol=1e-05): within 1.01 of 1000,
                                        // which for integers is within 1.
                                        return std::abs(sample - kAmplitude) <= 1;
                                    }),
                "PCM amplitude changed during decoding");

        // The recording the preview follows: a tape fed the decoded fixture after start, as
        // feed_audio queued it after start_streaming.
        const auto feed = [&](const std::string& job) {
            auto tape = std::make_shared<audio::PcmTape>(rate, 1);
            stream.start(tape, job, deliver);
            tape->append(decoded);
        };

        std::exception_ptr failure;
        try {
            for (int run = 0; run < repeats; ++run) {
                feed("workflow-smoke");
                const auto stop = Steady::now();
                stream.stop();
                const std::optional<std::string> result = stream.finalize(kDeadline);
                join_worker(stream);
                timings.push_back(seconds_since(stop));
                require(result == kFixtureText, "Fixture transcription was lost or duplicated");
            }
            // Keep a decode in flight so cancel proves late text cannot escape.
            values.clear();
            gate->set_block(true);
            feed("workflow-smoke-cancel");
            require(loop.run_until([&] { return gate->entered(); }, Steady::now() + kDeadline),
                    "Preview worker did not start decoding");
            const auto cancel_started = Steady::now();
            stream.cancel();
            gate->release();
            join_worker(stream);
            loop.process_events();
            cancel_s = seconds_since(cancel_started);
            require(stream.preview_text().empty() && values.empty(),
                    "Canceled text escaped through the queued delivery");
            gate->set_block(false);
            const auto restart_started = Steady::now();
            feed("workflow-smoke-restart");
            loop.run_until([&] { return !values.empty(); }, restart_started + kDeadline);
            require(values == std::vector<std::pair<std::string, bool>>{{std::string(kFixtureText),
                                                                         true}},
                    "Restart did not deliver exactly one queued result");
            restart_s = seconds_since(restart_started);

            // services/database.py imports a legacy history file beside the database; none is
            // in this folder, so nothing of the user's is read.
            {
                Database database = Database::open_or_create(root / "history.db");
                HistoryRepository(database).add_history_entry(
                    fixture_entry("synthetic", values.front().first));
                database.close();
            }
            {
                Database database = Database::open_or_create(root / "history.db");
                const std::optional<HistoryEntry> row =
                    HistoryRepository(database).get_history_entry_by_id("synthetic");
                require(row && row->text == kFixtureText,
                        "History did not survive a database restart");
                database.close();
            }
            checks = check_data_recovery(root, pcm_bytes(samples));
        } catch (...) {
            failure = std::current_exception();
        }
        const auto cleanup_started = Steady::now();
        gate->release();
        stream.cleanup();
        if (failure) {
            try {
                join_worker(stream);
            } catch (const std::exception&) {
                // The first failure is the one to report.
            }
            std::rethrow_exception(failure);
        }
        join_worker(stream);
        cleanup_s = seconds_since(cleanup_started);
    }

    Json metrics = Json::object();
    metrics["fixture_ready_s"] = ready_s;
    metrics["stop_to_result_p50_s"] = median(timings);
    metrics["stop_to_result_p95_s"] = percentile(timings, 95);
    metrics["cancel_s"] = cancel_s;
    metrics["restart_s"] = restart_s;
    metrics["cleanup_s"] = cleanup_s;
    const std::optional<double> rss = peak_rss_mb();
    metrics["peak_rss_mb"] = rss ? Json(*rss) : Json(nullptr);

    Json report = Json::object();
    report["schema"] = 2;
    report["kind"] = "synthetic_service_lifecycle";
    report["repeats"] = repeats;
    report["metrics"] = std::move(metrics);
    report["checks"] = std::move(checks);
    report["passed"] = true;
    report["limitations"] =
        Json::array({"Fixture decoder; no real speech recognition or model startup",
                     "No physical capture device, native inference, VRAM or dropped-audio "
                     "measurement",
                     "Service/database restart and cleanup; not full application process "
                     "restart"});

    py::DumpOptions sorted;
    sorted.sort_keys = true;
    std::fputs((py::dumps(report, sorted) + "\n").c_str(), stdout);
    std::fflush(stdout);
    return report;
}

int run_workflow_smoke_command(std::span<const std::string> args) {
    if (args.size() > 1) {
        std::fprintf(stderr, "%s\n", kWorkflowSmokeUsage);
        return 1;
    }
    Json report;
    try {
        report = run_workflow_smoke();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "fiorivoce-cli: workflow smoke failed: %s\n", error.what());
        return 1;
    }
    if (args.size() == 1) {
        try {
            write_file(path_from_utf8(args.front()), report_file_text(report));
        } catch (const std::exception&) {
            std::fprintf(stderr, "fiorivoce-cli: The report could not be written to %s.\n",
                         args.front().c_str());
            return 1;
        }
    }
    return 0;
}

} // namespace fv::cli
