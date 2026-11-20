#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/testing/fake_backend.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "db/database.h"
#include "meeting/engine.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_capture/testing/fake_capture_source.h"
#include "meeting_state/sql_repository.h"
#include "testkit/testkit.h"

// The engine's tests (tests/test_meeting_engine.py): OpenWhisper's in-process fakes for the
// subsystems the engine builds (FakeAsr, FakeServer, FakeDiarizer, fake capture sources), its
// `repo` fixture (a SQL repository on a fresh database), its make_engine factory and a probe
// into the engine's private state, which Python's tests reach through underscore attributes.
namespace fv::meeting {

// Python's test access to the engine's internals.
class MeetingEngineProbe {
public:
    explicit MeetingEngineProbe(MeetingEngine& engine) : engine_(engine) {}

    MeetingCapture& capture() { return *engine_.capture_; }
    bool asr_ready() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.asr_ready_;
    }
    std::shared_ptr<MeetingAsr> asr() const { return engine_.asr(); }
    std::shared_ptr<diarize::Diarizer> diarizer() const { return engine_.diarizer(); }
    bool degraded_diarization() const { return engine_.degraded_diarization_.load(); }
    std::optional<std::string> me_participant_id() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.me_participant_id_;
    }
    std::filesystem::path spool_dir() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.spool_dir_;
    }
    bool processing_thread_started() const {
        const std::lock_guard lock(engine_.lifecycle_mutex_);
        return engine_.processing_.started;
    }
    bool end_started() const {
        const std::lock_guard lock(engine_.lifecycle_mutex_);
        return engine_.end_.started;
    }
    bool end_done() const {
        const std::lock_guard lock(engine_.lifecycle_mutex_);
        return engine_.end_.done;
    }
    std::thread::id end_thread() const {
        const std::lock_guard lock(engine_.lifecycle_mutex_);
        return engine_.end_.id;
    }
    std::map<std::string, double, std::less<>> preview_frontiers() const {
        const std::lock_guard lock(engine_.index_mutex_);
        return engine_.preview_frontiers_;
    }
    void index_chunk(const meeting_state::SpooledChunk& chunk) {
        const std::lock_guard lock(engine_.index_mutex_);
        engine_.chunk_index_[chunk.chunk_id] = chunk;
    }
    void index_frontier(const std::string& channel, double end_s) {
        const std::lock_guard lock(engine_.index_mutex_);
        engine_.preview_frontiers_[channel] = end_s;
    }
    void broadcast(const Json& message, bool host_only) { engine_.broadcast(message, host_only); }
    // The processing start alone, without the speaker model's thread.
    bool processing_complete(std::chrono::milliseconds timeout) {
        return engine_.wait_worker(engine_.processing_, timeout);
    }
    // Before start().
    void skip_processing() { engine_.hooks_.skip_processing = true; }

    void on_chunk(const meeting_state::SpooledChunk& chunk) { engine_.on_chunk(chunk); }
    void notify_intelligence_segments(int count) { engine_.notify_intelligence_segments(count); }

    void on_chunk_result(const meeting_state::SpooledChunk& chunk,
                         std::vector<meeting_state::TranscriptSegment> segments) {
        engine_.on_chunk_result(chunk, std::move(segments));
    }
    void on_speech_preview(const Json& preview) { engine_.on_speech_preview(preview); }
    void on_diarizer_relabel(const diarize::RelabelOps& ops) { engine_.on_diarizer_relabel(ops); }
    void on_asr_backlog_status(const std::string& message, bool overloaded) {
        engine_.on_asr_backlog_status(message, overloaded);
    }
    void on_asr_chunk_failure(const std::string& message) { engine_.on_asr_chunk_failure(message); }
    void start_asr(bool defer_load) { engine_.start_asr(defer_load); }
    bool set_finalization(const FinalizationUpdate& update) {
        return engine_.set_finalization(update);
    }
    Json finalization_summary_stats() { return engine_.finalization_summary_stats(); }
    Json run_cloud_speaker_pass(const std::function<void()>& on_start, bool injected_decoder) {
        return engine_.run_cloud_speaker_pass({}, on_start, injected_decoder);
    }
    std::string intelligence_offline_message() const {
        return engine_.intelligence_offline_message();
    }
    // Whether a live highlights, citation or voice-command worker is still attached (Python's
    // _live_signals and its siblings).
    bool fast_features_running() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.voice_commands_ != nullptr || engine_.citation_verifier_ != nullptr ||
               engine_.live_highlights_ != nullptr;
    }

    // Which fast-feature workers are attached (their identity, null when absent), and the
    // engine's own retry of them (_start_fast_features) and status emit (_emit_status).
    const void* voice_listener() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.voice_commands_.get();
    }
    const void* live_highlights() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.live_highlights_.get();
    }
    const void* citation_verifier() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.citation_verifier_.get();
    }
    void start_fast_features() { engine_.start_fast_features(); }
    void emit_status() { engine_.emit_status(); }

    // Before start(): an exception thrown from these is what Python's monkeypatched flush() and
    // _finalize_highlights raise.
    void before_flush(std::function<void()> hook) { engine_.hooks_.before_flush = std::move(hook); }
    void before_drain(std::function<void()> hook) { engine_.hooks_.before_drain = std::move(hook); }
    // engine.py _notify_human_guidance, and how many notices reached the live agent.
    void notify_intelligence_guidance(const std::vector<meeting_state::OpResult>& ops) {
        engine_.notify_intelligence_guidance(ops);
    }
    int guidance_notices() const { return engine_.guidance_notices_.load(); }
    // Whether a live agent session is attached (Python's _agent_core / _scheduler).
    bool live_intelligence_attached() const {
        const std::lock_guard lock(engine_.state_mutex_);
        return engine_.live_intelligence_ != nullptr;
    }
    // The topic_judge the attached scheduler was built with (Python reads it from the
    // CheckpointScheduler factory's call kwargs); empty without a live agent.
    MeetingEngine::TopicJudgeFn attached_topic_judge() const {
        return engine_.attached_topic_judge();
    }
    void final_highlights(std::function<void()> hook) {
        engine_.hooks_.final_highlights = std::move(hook);
    }

private:
    MeetingEngine& engine_;
};

namespace test {

using meeting_state::SpooledChunk;
using meeting_state::TranscriptSegment;

// Runs every task at once on the posting thread, and counts as current everywhere: a listener
// on it is Python's synchronous callback.
class InlineExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task) override { return {}; }
    bool is_current() const override { return true; }
};

// A latch a fake blocks on, as Python's threading.Event.
class Event {
public:
    void set() {
        {
            const std::lock_guard lock(mutex_);
            set_ = true;
        }
        changed_.notify_all();
    }
    bool is_set() const {
        const std::lock_guard lock(mutex_);
        return set_;
    }
    bool wait(std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return set_; });
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool set_ = false;
};

// tests/helpers.py write_wav: a mono 16-bit WAV of `value` for duration_s seconds.
inline void write_wav(const std::filesystem::path& path, double duration_s = 10.0,
                      std::int16_t value = 0, int rate = 16000) {
    const auto frames = static_cast<std::uint32_t>(duration_s * rate);
    const auto u32 = [](std::ofstream& out, std::uint32_t v) {
        const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                               static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.write(bytes, 4);
    };
    const auto u16 = [](std::ofstream& out, std::uint16_t v) {
        const char bytes[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(bytes, 2);
    };
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    u32(out, 36 + frames * 2);
    out.write("WAVEfmt ", 8);
    u32(out, 16);
    u16(out, 1);
    u16(out, 1);
    u32(out, static_cast<std::uint32_t>(rate));
    u32(out, static_cast<std::uint32_t>(rate * 2));
    u16(out, 2);
    u16(out, 16);
    out.write("data", 4);
    u32(out, frames * 2);
    for (std::uint32_t i = 0; i < frames; ++i) {
        u16(out, static_cast<std::uint16_t>(value));
    }
}

// The ASR engine stand-in of test_meeting_engine.py: records lifecycle calls and enqueued chunks.
// Affinity: thread-safe.
class FakeAsr final : public MeetingAsr {
public:
    FakeAsr(std::shared_ptr<meeting_state::MeetingRepository> repository, std::string id,
            std::string model_name, std::string language_code)
        : meeting_id(std::move(id)), model(std::move(model_name)),
          language(std::move(language_code)), repository_(std::move(repository)) {}

    // What load_backend does instead of answering is_available; given the stop's cancel.
    std::function<bool(const CancelToken&)> on_load;
    // Runs after requeue_pending's enqueues.
    std::function<void(FakeAsr&)> after_requeue;
    // Replaces transcribe_offline_session.
    std::function<std::vector<TranscriptSegment>()> on_offline;

    bool load_backend(const CancelToken& cancel) override {
        if (on_load) {
            return on_load(stop_token(cancel));
        }
        return available;
    }
    bool is_available() const override { return available; }
    std::string last_error() const override { return {}; }
    void start(ChunkResult callback) override {
        const std::lock_guard lock(mutex_);
        on_segments = std::move(callback);
    }
    bool enqueue(const SpooledChunk& chunk) override {
        const std::lock_guard lock(mutex_);
        if (!queued_ids_.insert(chunk.chunk_id).second) {
            return true;
        }
        chunks_.push_back(chunk);
        return true;
    }
    std::size_t requeue_pending() override {
        ++requeues;
        std::vector<Json> rows = repository_->get_pending_chunks(meeting_id, std::nullopt, {});
        std::sort(rows.begin(), rows.end(), [](const Json& a, const Json& b) {
            const auto key = [](const Json& row) {
                return std::tuple(row.value("start_s", 0.0), row.value("seq", std::int64_t{0}),
                                  row.value("channel", std::string()),
                                  row.at("id").get<std::int64_t>());
            };
            return key(a) < key(b);
        });
        for (const Json& row : rows) {
            SpooledChunk chunk;
            chunk.chunk_id = row.at("id").get<std::int64_t>();
            chunk.meeting_id = row.at("meeting_id").get<std::string>();
            chunk.channel = row.at("channel").get<std::string>();
            chunk.seq = row.at("seq").get<std::int64_t>();
            chunk.file_path = row.at("file_path").get<std::string>();
            chunk.start_s = row.at("start_s").get<double>();
            chunk.duration_s = row.at("duration_s").get<double>();
            chunk.sample_rate = row.at("sample_rate").get<int>();
            enqueue(chunk);
        }
        if (after_requeue) {
            after_requeue(*this);
        }
        return rows.size();
    }
    bool drain(std::chrono::milliseconds) override {
        ++drains;
        return true;
    }
    void stop() override {
        ++stops;
        stopped_.cancel();
    }
    void start_preview(PreviewCallback) override { ++previews_started; }
    void feed_preview(const CaptureBlock&, double) noexcept override {}
    void stop_preview() override {}
    std::vector<TranscriptSegment> transcribe_offline_session(const std::filesystem::path&,
                                                              const std::vector<Json>&,
                                                              OfflineProgress,
                                                              const CancelToken&) override {
        ++offline_passes;
        if (on_offline) {
            return on_offline();
        }
        const std::lock_guard lock(mutex_);
        return offline_segments;
    }

    std::vector<SpooledChunk> chunks() const {
        const std::lock_guard lock(mutex_);
        return chunks_;
    }
    std::vector<std::int64_t> chunk_ids() const {
        std::vector<std::int64_t> ids;
        for (const SpooledChunk& chunk : chunks()) {
            ids.push_back(chunk.chunk_id);
        }
        return ids;
    }
    void set_offline_segments(std::vector<TranscriptSegment> segments) {
        const std::lock_guard lock(mutex_);
        offline_segments = std::move(segments);
    }
    ChunkResult segments_callback() const {
        const std::lock_guard lock(mutex_);
        return on_segments;
    }

    const std::string meeting_id;
    const std::string model;
    const std::string language;
    // What the engine gave the ASR factory; set before the engine sees this fake.
    AsrCallbacks callbacks;
    std::atomic<bool> available{true};
    std::atomic<int> drains{0};
    std::atomic<int> stops{0};
    std::atomic<int> requeues{0};
    std::atomic<int> offline_passes{0};
    std::atomic<int> previews_started{0};

private:
    // The load's token, cancelled by stop() as well.
    CancelToken stop_token(const CancelToken& load_cancel) {
        if (load_cancel.cancelled()) {
            stopped_.cancel();
        }
        link_ = load_cancel.on_cancel([this] { stopped_.cancel(); });
        return stopped_.token();
    }

    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    mutable std::mutex mutex_;
    ChunkResult on_segments;
    std::set<std::int64_t> queued_ids_;
    std::vector<SpooledChunk> chunks_;
    std::vector<TranscriptSegment> offline_segments;
    CancelSource stopped_;
    Subscription link_;
};

// The web server stand-in that records broadcasts instead of sending them.
// Affinity: thread-safe.
class FakeServer final : public DashboardTransport {
public:
    std::function<std::expected<std::string, std::string>()> on_start;
    bool host_only_support = true;

    std::expected<std::string, std::string> start() override {
        if (on_start) {
            return on_start();
        }
        return std::string(kHostUrl);
    }
    void stop() override { stopped = true; }
    void broadcast(const Json& message, bool host_only) override {
        const std::lock_guard lock(mutex_);
        messages_.push_back(message);
        host_only_flags_.push_back(host_only);
    }
    bool supports_host_only() const override { return host_only_support; }
    // Each broadcast's message and whether it went to host sockets only, in order.
    std::vector<std::pair<Json, bool>> sent_host_only() const {
        const std::lock_guard lock(mutex_);
        std::vector<std::pair<Json, bool>> out;
        for (std::size_t i = 0; i < messages_.size() && i < host_only_flags_.size(); ++i) {
            out.emplace_back(messages_[i], host_only_flags_[i]);
        }
        return out;
    }
    std::string base_url() const override { return "http://127.0.0.1:9999"; }
    std::string host_url() const override { return std::string(kHostUrl); }
    std::string guest_url() const override { return "http://127.0.0.1:9999/g/token"; }
    void invalidate_connections() override { ++invalidations; }

    std::vector<Json> messages() const {
        const std::lock_guard lock(mutex_);
        return messages_;
    }
    std::vector<Json> messages_of(std::string_view type) const {
        std::vector<Json> out;
        for (const Json& message : messages()) {
            if (message.value("type", std::string()) == type) {
                out.push_back(message);
            }
        }
        return out;
    }

    static constexpr std::string_view kHostUrl = "http://127.0.0.1:9999/h/token";
    std::atomic<bool> stopped{false};
    std::atomic<int> invalidations{0};

private:
    mutable std::mutex mutex_;
    std::vector<Json> messages_;
    std::vector<bool> host_only_flags_;
};

// A diarizer whose availability and assignments the test drives.
// Affinity: thread-safe.
class FakeDiarizer final : public diarize::Diarizer {
public:
    std::optional<std::string> assign(diarize::Segment& segment, std::span<const float>,
                                      int) override {
        const std::lock_guard lock(mutex_);
        assigned_.push_back(segment.id);
        return next_participant;
    }
    void pin(const std::string& segment_id, const std::string& participant_id) override {
        const std::lock_guard lock(mutex_);
        pins_.emplace_back(segment_id, participant_id);
    }
    bool is_available() const override {
        ++availability_probes;
        if (on_probe) {
            on_probe();
        }
        return available.load();
    }

    std::vector<std::string> assigned() const {
        const std::lock_guard lock(mutex_);
        return assigned_;
    }
    std::vector<std::pair<std::string, std::string>> pins() const {
        const std::lock_guard lock(mutex_);
        return pins_;
    }
    void set_next(std::optional<std::string> participant) {
        const std::lock_guard lock(mutex_);
        next_participant = std::move(participant);
    }

    std::atomic<bool> available{true};
    mutable std::atomic<int> availability_probes{0};
    std::function<void()> on_probe;

private:
    mutable std::mutex mutex_;
    std::optional<std::string> next_participant;
    std::vector<std::string> assigned_;
    std::vector<std::pair<std::string, std::string>> pins_;
};

// A diarizer the engine owns, forwarding to a FakeDiarizer the test keeps.
class SharedDiarizer final : public diarize::Diarizer {
public:
    explicit SharedDiarizer(std::shared_ptr<FakeDiarizer> inner) : inner_(std::move(inner)) {}
    std::optional<std::string> assign(diarize::Segment& segment, std::span<const float> audio,
                                      int rate) override {
        return inner_->assign(segment, audio, rate);
    }
    void pin(const std::string& segment_id, const std::string& participant_id) override {
        inner_->pin(segment_id, participant_id);
    }
    bool is_available() const override { return inner_->is_available(); }

private:
    std::shared_ptr<FakeDiarizer> inner_;
};

// OpenWhisper's `repo` fixture: a SQL meeting repository on a fresh database.
struct SqlRepository {
    SqlRepository()
        : database(
              std::make_unique<Database>(Database::open_or_create(root.path() / "fiorivoce.db"))),
          repo(std::make_shared<meeting_state::SqlMeetingRepository>(*database)) {}

    testkit::TempDataRoot root;
    std::unique_ptr<Database> database;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repo;
};

// The options and seams make_engine overrides for one test.
// The base of a test provider whose meetings come online: each live start gets a healthy
// FakeAgentCore, as Python's engine fixture gives every cloud meeting a core. End runs its cleanup
// and report passes only while the live agent is online (engine.py _end_worker, EndRun.plan).
class OnlineProvider : public IntelligenceProvider {
public:
    bool available() const override { return true; }
    std::shared_ptr<meeting_agent::AgentCore> create_live_core(const RerunContext&,
                                                               const CancelToken&) override {
        return std::make_shared<meeting_agent::testing::FakeAgentCore>();
    }
};

struct EngineSetup {
    MeetingEngineOptions options;
    bool processing_inline = true;
    const Clock* clock = nullptr;
    meeting_state::IdSource* ids = nullptr;
    int mics = 1;
    // Replaces the `mics` microphones "Mic i" on host API "MME" when not empty; the first is the
    // default input.
    std::vector<audio::testing::FakeDevice> mic_devices;
    // Called on each microphone source the capture opens, before it starts (fail_start(...)
    // plays a device that will not open).
    std::function<void(testing::FakeCaptureSource&, const audio::InputDevice&)> on_mic_source;
    bool loopback = true;
    // Replaces the FakeAsr the factory builds (one per call).
    std::function<std::shared_ptr<FakeAsr>(const std::string& meeting_id, const std::string& model,
                                           const std::string& language)>
        asr;
    std::function<void(FakeServer&)> server;
    // Replaces the FakeDiarizer the factory hands out.
    std::function<std::unique_ptr<diarize::Diarizer>(
        const std::optional<std::filesystem::path>& model)>
        diarizer;
    std::optional<CaptureTiming> timing;
    bool sources_need_blocks = false;
    // The heartbeat's strand; null: the engine's own thread.
    Executor* strand = nullptr;
    std::optional<std::int64_t> pid;
    std::shared_ptr<IntelligenceProvider> intelligence;
    std::function<Json()> settings;
    std::function<std::shared_ptr<llm::Judge>()> local_fast_judge;
};

// make_engine: engines wired to the fakes, every event recorded, shut down after the test.
class EngineRig {
public:
    EngineRig() { std::filesystem::create_directories(spool_root()); }
    ~EngineRig() {
        for (const auto& engine : engines_) {
            if (!engine) {
                continue; // taken by the test
            }
            try {
                engine->shutdown();
            } catch (const std::exception&) {
            }
        }
        engines_.clear();
    }
    EngineRig(const EngineRig&) = delete;
    EngineRig& operator=(const EngineRig&) = delete;

    static MeetingEngineOptions default_options() {
        MeetingEngineOptions options;
        options.title = "Test meeting";
        options.cloud_enabled = true;
        options.agent_core_kind = "pi";
        return options;
    }

    std::filesystem::path spool_root() const { return sql.root.path() / "spool"; }

    MeetingEngine& make(EngineSetup setup = {.options = default_options()}) {
        setup.options.spool_root = spool_root();
        auto backend = std::make_shared<audio::testing::FakeBackend>();
        std::vector<audio::testing::FakeDevice> devices = setup.mic_devices;
        for (int i = 0; devices.empty() && i < setup.mics; ++i) {
            devices.push_back({.name = "Mic " + std::to_string(i),
                               .hostapi = 0,
                               .max_input_channels = 1,
                               .default_samplerate = 16000});
        }
        const int default_input = devices.empty() ? -1 : 0;
        backend->set_layout({{.name = "MME", .default_input_device = default_input}},
                            std::move(devices), default_input);
        if (setup.loopback) {
            backend->set_loopbacks({{.id = "{a}", .name = "Speakers {a}", .is_default = true}});
        }
        audio_backends.push_back(backend);

        EngineDeps deps;
        deps.repository = sql.repo;
        deps.audio = backend;
        deps.clock = setup.clock;
        deps.ids = setup.ids;
        deps.processing_inline = setup.processing_inline;
        deps.strand = setup.strand;
        deps.pid = setup.pid;
        deps.intelligence = std::move(setup.intelligence);
        deps.settings = std::move(setup.settings);
        deps.local_fast_judge = std::move(setup.local_fast_judge);
        deps.capture_timing =
            setup.timing ? *setup.timing
                         : CaptureTiming{.watchdog_interval_s = 3600.0, .retry_interval_s = 3.0};
        const bool need_blocks = setup.sources_need_blocks;
        SourceFactory sources;
        sources.mic = [this, need_blocks,
                       on_mic = setup.on_mic_source](const audio::InputDevice& device) {
            auto source =
                std::make_shared<testing::FakeCaptureSource>("mic", device.index, need_blocks);
            if (on_mic) {
                on_mic(*source, device);
            }
            const std::lock_guard lock(mutex_);
            mic_sources.push_back(source);
            return source;
        };
        sources.loopback = [this, need_blocks](const audio::LoopbackEndpoint& endpoint) {
            auto source =
                std::make_shared<testing::FakeCaptureSource>("loopback", endpoint.id, need_blocks);
            const std::lock_guard lock(mutex_);
            loopback_sources.push_back(source);
            return source;
        };
        deps.capture_sources = sources;
        auto make_asr = setup.asr;
        deps.asr = [this, make_asr](const std::string& meeting_id, const std::string& model,
                                    const std::string& language,
                                    AsrCallbacks callbacks) -> std::unique_ptr<MeetingAsr> {
            std::shared_ptr<FakeAsr> fake =
                make_asr ? make_asr(meeting_id, model, language)
                         : std::make_shared<FakeAsr>(sql.repo, meeting_id, model, language);
            fake->callbacks = std::move(callbacks);
            {
                const std::lock_guard lock(mutex_);
                asrs.push_back(fake);
            }
            return std::make_unique<SharedAsr>(fake);
        };
        auto configure_server = setup.server;
        deps.dashboard = [this,
                          configure_server](MeetingEngine&, const std::string&,
                                            std::uint16_t) -> std::unique_ptr<DashboardTransport> {
            auto fake = std::make_shared<FakeServer>();
            if (configure_server) {
                configure_server(*fake);
            }
            {
                const std::lock_guard lock(mutex_);
                servers.push_back(fake);
            }
            return std::make_unique<SharedServer>(fake);
        };
        auto make_diarizer = setup.diarizer;
        deps.diarizer = [this, make_diarizer](
                            const std::optional<std::filesystem::path>& model,
                            const std::optional<std::filesystem::path>&, diarize::DiarizerHost&,
                            const std::string&) -> std::unique_ptr<diarize::Diarizer> {
            {
                const std::lock_guard lock(mutex_);
                diarizers_created.push_back(model);
            }
            if (make_diarizer) {
                return make_diarizer(model);
            }
            return std::make_unique<SharedDiarizer>(diarizer);
        };
        auto engine = std::make_unique<MeetingEngine>(std::move(setup.options), std::move(deps));
        auto log = std::make_shared<EventLog>();
        subscriptions_.push_back(engine->add_listener(inline_, [log](const MeetingEvent& event) {
            const std::lock_guard lock(log->mutex);
            log->events.emplace_back(event.kind, event.payload);
        }));
        logs_.push_back(log);
        engines_.push_back(std::move(engine));
        return *engines_.back();
    }

    // Hands the engine made `index`-th to the test, which then owns its shutdown.
    std::unique_ptr<MeetingEngine> take(std::size_t index = 0) {
        return std::move(engines_.at(index));
    }

    // Every event of the engine made `index`-th, in order.
    std::vector<std::pair<std::string, Json>> events(std::size_t index = 0) const {
        const std::lock_guard lock(logs_.at(index)->mutex);
        return logs_.at(index)->events;
    }
    std::vector<Json> events_of(std::string_view kind, std::size_t index = 0) const {
        std::vector<Json> out;
        for (const auto& [event_kind, payload] : events(index)) {
            if (event_kind == kind) {
                out.push_back(payload);
            }
        }
        return out;
    }
    std::vector<std::string> kinds(std::size_t index = 0) const {
        std::vector<std::string> out;
        for (const auto& event : events(index)) {
            out.push_back(event.first);
        }
        return out;
    }

    std::shared_ptr<FakeAsr> asr(std::size_t index = 0) const {
        const std::lock_guard lock(mutex_);
        return index < asrs.size() ? asrs[index] : nullptr;
    }
    std::size_t asr_count() const {
        const std::lock_guard lock(mutex_);
        return asrs.size();
    }
    std::shared_ptr<FakeServer> server(std::size_t index = 0) const {
        const std::lock_guard lock(mutex_);
        return index < servers.size() ? servers[index] : nullptr;
    }
    std::size_t server_count() const {
        const std::lock_guard lock(mutex_);
        return servers.size();
    }
    std::size_t diarizers_made() const {
        const std::lock_guard lock(mutex_);
        return diarizers_created.size();
    }

    // end_and_wait: End, wait for the worker, and the finalization.
    static Json end_and_wait(MeetingEngine& engine) {
        engine.end();
        engine.wait_for_end(std::chrono::seconds(10));
        return engine.store()->with_state(
            [](const meeting_state::MeetingState& state) { return state.finalization.to_json(); });
    }

    // A chunk row registered as TestDeferredProcessingStartup._register does.
    SpooledChunk register_chunk(const std::string& meeting_id, std::int64_t seq, double start_s,
                                const std::string& channel = "mic", double duration_s = 1.0,
                                std::optional<std::filesystem::path> file = std::nullopt) {
        const std::string path = file ? path_to_utf8(*file) : std::string("nul");
        Json fields = Json::object();
        fields["meeting_id"] = meeting_id;
        fields["channel"] = channel;
        fields["seq"] = seq;
        fields["file_path"] = path;
        fields["start_s"] = start_s;
        fields["duration_s"] = duration_s;
        fields["sample_rate"] = 16000;
        SpooledChunk chunk;
        chunk.chunk_id = sql.repo->register_chunk(fields);
        chunk.meeting_id = meeting_id;
        chunk.channel = channel;
        chunk.seq = seq;
        chunk.file_path = path;
        chunk.start_s = start_s;
        chunk.duration_s = duration_s;
        chunk.sample_rate = 16000;
        return chunk;
    }

    // The sources the capture opened, in order; the watchdog adds to them on its own thread.
    std::vector<std::shared_ptr<testing::FakeCaptureSource>> mics() const {
        const std::lock_guard lock(mutex_);
        return mic_sources;
    }
    std::vector<std::shared_ptr<testing::FakeCaptureSource>> loopbacks() const {
        const std::lock_guard lock(mutex_);
        return loopback_sources;
    }

    SqlRepository sql;
    std::shared_ptr<FakeDiarizer> diarizer = std::make_shared<FakeDiarizer>();
    std::vector<std::shared_ptr<audio::testing::FakeBackend>> audio_backends;

private:
    std::vector<std::shared_ptr<testing::FakeCaptureSource>> mic_sources;
    std::vector<std::shared_ptr<testing::FakeCaptureSource>> loopback_sources;
    struct EventLog {
        std::mutex mutex;
        std::vector<std::pair<std::string, Json>> events;
    };
    // The engine owns what its factories return; the test keeps the fakes.
    class SharedAsr final : public MeetingAsr {
    public:
        explicit SharedAsr(std::shared_ptr<FakeAsr> inner) : inner_(std::move(inner)) {}
        bool load_backend(const CancelToken& cancel) override {
            return inner_->load_backend(cancel);
        }
        bool is_available() const override { return inner_->is_available(); }
        std::string last_error() const override { return inner_->last_error(); }
        void start(ChunkResult callback) override { inner_->start(std::move(callback)); }
        bool enqueue(const SpooledChunk& chunk) override { return inner_->enqueue(chunk); }
        std::size_t requeue_pending() override { return inner_->requeue_pending(); }
        bool drain(std::chrono::milliseconds timeout) override { return inner_->drain(timeout); }
        void stop() override { inner_->stop(); }
        void start_preview(PreviewCallback callback) override {
            inner_->start_preview(std::move(callback));
        }
        void feed_preview(const CaptureBlock& block, double start_s) noexcept override {
            inner_->feed_preview(block, start_s);
        }
        void stop_preview() override { inner_->stop_preview(); }
        std::vector<TranscriptSegment>
        transcribe_offline_session(const std::filesystem::path& spool_dir,
                                   const std::vector<Json>& chunks, OfflineProgress progress,
                                   const CancelToken& cancel) override {
            return inner_->transcribe_offline_session(spool_dir, chunks, std::move(progress),
                                                      cancel);
        }

    private:
        std::shared_ptr<FakeAsr> inner_;
    };
    class SharedServer final : public DashboardTransport {
    public:
        explicit SharedServer(std::shared_ptr<FakeServer> inner) : inner_(std::move(inner)) {}
        std::expected<std::string, std::string> start() override { return inner_->start(); }
        void stop() override { inner_->stop(); }
        void broadcast(const Json& message, bool host_only) override {
            inner_->broadcast(message, host_only);
        }
        bool supports_host_only() const override { return inner_->supports_host_only(); }
        std::string base_url() const override { return inner_->base_url(); }
        std::string host_url() const override { return inner_->host_url(); }
        std::string guest_url() const override { return inner_->guest_url(); }
        void invalidate_connections() override { inner_->invalidate_connections(); }

    private:
        std::shared_ptr<FakeServer> inner_;
    };

    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<FakeAsr>> asrs;
    std::vector<std::shared_ptr<FakeServer>> servers;
    std::vector<std::optional<std::filesystem::path>> diarizers_created;
    InlineExecutor inline_;
    std::vector<std::shared_ptr<EventLog>> logs_;
    std::vector<Subscription> subscriptions_;
    std::vector<std::unique_ptr<MeetingEngine>> engines_;
};

// loopback_segment of test_meeting_engine.py.
inline TranscriptSegment loopback_segment(const std::string& meeting_id, const std::string& id,
                                          double start = 1.0, double end = 3.0,
                                          std::optional<std::int64_t> chunk_id = std::nullopt) {
    TranscriptSegment segment;
    segment.segment_id = id;
    segment.meeting_id = meeting_id;
    segment.chunk_id = chunk_id;
    segment.channel = "loopback";
    segment.start_s = start;
    segment.end_s = end;
    segment.text = "hello there";
    return segment;
}

inline TranscriptSegment mic_segment(const std::string& meeting_id, const std::string& id,
                                     double start, double end, const std::string& text) {
    TranscriptSegment segment;
    segment.segment_id = id;
    segment.meeting_id = meeting_id;
    segment.channel = "mic";
    segment.start_s = start;
    segment.end_s = end;
    segment.text = text;
    return segment;
}

// Polls `condition` until it holds or `timeout` passes.
template <class F>
bool eventually(F condition, std::chrono::milliseconds timeout = std::chrono::seconds(5)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return true;
}

} // namespace test

} // namespace fv::meeting
