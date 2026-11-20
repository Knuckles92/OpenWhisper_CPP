#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "db/database.h"
#include "meeting/recovery.h"
#include "meeting_state/repository.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "speech/service.h"
#include "testkit/testkit.h"

// What the recovery and re-run tests share: OpenWhisper's `repo` fixture (a SQL repository on a
// fresh database), the hand-rolled FakeRepository of test_meeting_recovery.py as a recording
// decorator, WAV files as Python's wave module writes them, and a real SpeechService over a
// scripted backend for the engines a finalize or a redecode builds.
namespace fv::meeting::test {

using meeting_state::MeetingRepository;

// A fresh data folder with a SQL meeting repository (OpenWhisper's `repo` fixture).
struct SqlRepo {
    SqlRepo()
        : database(
              std::make_unique<Database>(Database::open_or_create(root.path() / "fiorivoce.db"))),
          repo(std::make_shared<meeting_state::SqlMeetingRepository>(*database, clock)) {}

    std::filesystem::path folder(const std::string& name) const {
        const std::filesystem::path path = root.path() / name;
        std::filesystem::create_directories(path);
        return path;
    }

    // Declared first, so the database closes before the folder is removed.
    testkit::TempDataRoot root;
    FakeClock clock;
    std::unique_ptr<Database> database;
    std::shared_ptr<meeting_state::SqlMeetingRepository> repo;
};

// FakeMeetingRepository behind the switches and records of test_meeting_recovery.py's
// FakeRepository: update_meeting calls, an interrupted list, reset calls, an unfinished count,
// and failures of the meeting list, the interrupted query and the pending chunks.
// Affinity: thread-safe.
class ScriptedRepository final : public MeetingRepository {
public:
    meeting_state::testing::FakeMeetingRepository inner;

    std::vector<std::pair<std::string, Json>> updates() const {
        const std::lock_guard lock(mutex_);
        return updates_;
    }
    std::vector<std::string> reset_calls() const {
        const std::lock_guard lock(mutex_);
        return reset_calls_;
    }
    void set_interrupted(std::vector<Json> rows) {
        const std::lock_guard lock(mutex_);
        interrupted_ = std::move(rows);
    }
    void set_unfinished(std::optional<std::int64_t> count) {
        const std::lock_guard lock(mutex_);
        unfinished_ = count;
    }
    void fail_list(bool fail) { fail_list_ = fail; }
    void fail_pending(bool fail) { fail_pending_ = fail; }
    void fail_list_meetings(std::optional<std::string> message) {
        const std::lock_guard lock(mutex_);
        list_meetings_failure_ = std::move(message);
    }

    void create_meeting(const Json& fields) override { inner.create_meeting(fields); }
    void update_meeting(std::string_view id, const Json& fields) override {
        {
            const std::lock_guard lock(mutex_);
            updates_.emplace_back(std::string(id), fields);
        }
        inner.update_meeting(id, fields);
    }
    void rename_meeting(std::string_view id, std::string_view title) override {
        inner.rename_meeting(id, title);
    }
    void replace_tokens(std::string_view id, std::string_view host,
                        std::string_view guest) override {
        inner.replace_tokens(id, host, guest);
    }
    std::optional<Json> get_meeting(std::string_view id) override { return inner.get_meeting(id); }
    std::vector<Json> list_meetings() override {
        {
            const std::lock_guard lock(mutex_);
            if (list_meetings_failure_) {
                throw std::runtime_error(*list_meetings_failure_);
            }
        }
        return inner.list_meetings();
    }
    void delete_meeting(std::string_view id) override { inner.delete_meeting(id); }
    void heartbeat(std::string_view id, std::int64_t pid, std::string_view utc) override {
        inner.heartbeat(id, pid, utc);
    }
    std::vector<Json> find_interrupted_meetings() override {
        if (fail_list_) {
            throw std::runtime_error("db down");
        }
        const std::lock_guard lock(mutex_);
        return interrupted_;
    }
    std::int64_t register_chunk(const Json& fields) override {
        return inner.register_chunk(fields);
    }
    // The next `times` register_chunk_if_missing calls throw "database is locked".
    void fail_register(int times) { register_failures_ = times; }
    meeting_state::ChunkRegistration register_chunk_if_missing(const Json& fields) override {
        if (register_failures_.load() > 0) {
            register_failures_.fetch_sub(1);
            throw std::runtime_error("database is locked");
        }
        return inner.register_chunk_if_missing(fields);
    }
    void set_chunk_status(std::int64_t chunk_id, std::string_view status,
                          std::optional<std::string_view> error) override {
        inner.set_chunk_status(chunk_id, status, error);
    }
    std::vector<Json> get_pending_chunks(std::string_view id, std::optional<std::int64_t> limit,
                                         const std::vector<std::int64_t>& exclude) override {
        if (fail_pending_) {
            throw std::runtime_error("chunks unavailable");
        }
        return inner.get_pending_chunks(id, limit, exclude);
    }
    std::int64_t count_unfinished_chunks(std::string_view id) override {
        {
            const std::lock_guard lock(mutex_);
            if (unfinished_) {
                return *unfinished_;
            }
        }
        return inner.count_unfinished_chunks(id);
    }
    std::int64_t reset_unfinished_chunks(std::string_view id, bool force) override {
        if (fail_pending_) {
            throw std::runtime_error("chunks unavailable");
        }
        {
            const std::lock_guard lock(mutex_);
            reset_calls_.emplace_back(id);
        }
        return inner.reset_unfinished_chunks(id, force);
    }
    void defer_chunk_after_connection_failure(std::int64_t chunk_id,
                                              std::string_view error) override {
        inner.defer_chunk_after_connection_failure(chunk_id, error);
    }
    std::vector<Json> get_audio_chunks(std::string_view id) override {
        return inner.get_audio_chunks(id);
    }
    std::int64_t next_chunk_seq(std::string_view id, std::string_view channel) override {
        return inner.next_chunk_seq(id, channel);
    }
    std::int64_t mark_chunks_done(std::string_view id) override {
        return inner.mark_chunks_done(id);
    }
    void add_segments(const std::vector<meeting_state::TranscriptSegment>& segments) override {
        inner.add_segments(segments);
    }
    meeting_state::ChunkCommit
    commit_chunk_transcription(std::string_view id, std::int64_t chunk_id,
                               const std::vector<meeting_state::TranscriptSegment>& s) override {
        {
            const std::lock_guard lock(mutex_);
            commits_.emplace_back(std::string(id), chunk_id, s.size());
        }
        return inner.commit_chunk_transcription(id, chunk_id, s);
    }
    std::vector<std::tuple<std::string, std::int64_t, std::size_t>> commits() const {
        const std::lock_guard lock(mutex_);
        return commits_;
    }
    std::optional<Json> get_segment(std::string_view id, std::string_view segment) override {
        return inner.get_segment(id, segment);
    }
    bool segment_exists(std::string_view id, std::string_view segment) override {
        return inner.segment_exists(id, segment);
    }
    std::map<std::string, bool, std::less<>>
    segment_flags(std::string_view id, const std::vector<std::string>& ids) override {
        return inner.segment_flags(id, ids);
    }
    void update_segment_speaker(std::string_view id, std::string_view segment,
                                const std::optional<std::string>& participant,
                                std::string_view source, bool pinned) override {
        inner.update_segment_speaker(id, segment, participant, source, pinned);
    }
    // get_segments answers these rows, or throws while `fail` is set.
    void set_segments(std::optional<std::vector<Json>> rows, bool fail = false) {
        const std::lock_guard lock(mutex_);
        segments_override_ = std::move(rows);
        fail_segments_ = fail;
    }
    std::vector<Json> get_segments(std::string_view id, double after,
                                   std::optional<std::int64_t> limit) override {
        {
            const std::lock_guard lock(mutex_);
            if (fail_segments_) {
                throw std::runtime_error("database is locked");
            }
            if (segments_override_) {
                return *segments_override_;
            }
        }
        return inner.get_segments(id, after, limit);
    }
    std::vector<Json> get_segments_page(std::string_view id, std::optional<double> start,
                                        const std::optional<std::string>& cursor,
                                        std::int64_t limit) override {
        return inner.get_segments_page(id, start, cursor, limit);
    }
    std::vector<Json> get_last_segments(std::string_view id, std::int64_t count) override {
        return inner.get_last_segments(id, count);
    }
    meeting_state::TranscriptReplacement
    replace_final_transcript(std::string_view id,
                             const std::vector<meeting_state::TranscriptSegment>& s) override {
        return inner.replace_final_transcript(id, s);
    }
    std::vector<Json> get_segments_delivery_page(std::string_view id, std::int64_t after,
                                                 std::int64_t limit) override {
        return inner.get_segments_delivery_page(id, after, limit);
    }
    std::int64_t get_delivery_cursor(std::string_view id, std::string_view consumer) override {
        return inner.get_delivery_cursor(id, consumer);
    }
    void advance_delivery_cursor(std::string_view id, std::string_view consumer,
                                 std::int64_t seq) override {
        inner.advance_delivery_cursor(id, consumer, seq);
    }
    std::int64_t get_delivery_high_water(std::string_view id) override {
        return inner.get_delivery_high_water(id);
    }
    Json on_ops_applied(std::string_view id, const Json& state,
                        std::vector<meeting_state::OpResult>& results, std::string_view actor_type,
                        const std::optional<std::string>& actor_id) override {
        return inner.on_ops_applied(id, state, results, actor_type, actor_id);
    }
    Json persist_state(std::string_view id, const Json& state) override {
        return inner.persist_state(id, state);
    }
    std::optional<Json> get_event(std::string_view id, std::int64_t seq) override {
        return inner.get_event(id, seq);
    }
    bool event_is_undone(std::string_view id, std::int64_t seq) override {
        return inner.event_is_undone(id, seq);
    }
    std::vector<Json> list_events(std::string_view id, std::optional<std::int64_t> before,
                                  std::int64_t limit) override {
        return inner.list_events(id, before, limit);
    }
    std::vector<Json> search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude,
                                         std::int64_t limit) override {
        return inner.search_transcripts(query, exclude, limit);
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, Json>> updates_;
    std::vector<std::string> reset_calls_;
    std::vector<Json> interrupted_;
    std::optional<std::int64_t> unfinished_;
    std::optional<std::string> list_meetings_failure_;
    std::vector<std::tuple<std::string, std::int64_t, std::size_t>> commits_;
    std::optional<std::vector<Json>> segments_override_;
    bool fail_segments_ = false;
    std::atomic<int> register_failures_{0};
    std::atomic<bool> fail_list_{false};
    std::atomic<bool> fail_pending_{false};
};

// A mono 16-bit WAV of the given samples, as Python's wave module writes it.
inline void write_wav(const std::filesystem::path& path, std::span<const std::int16_t> samples,
                      int rate = 16000) {
    const auto u32 = [](std::ofstream& out, std::uint32_t v) {
        const char bytes[4] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF),
                               static_cast<char>((v >> 16) & 0xFF),
                               static_cast<char>((v >> 24) & 0xFF)};
        out.write(bytes, 4);
    };
    const auto u16 = [](std::ofstream& out, std::uint16_t v) {
        const char bytes[2] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF)};
        out.write(bytes, 2);
    };
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    u32(out, 36 + data);
    out.write("WAVEfmt ", 8);
    u32(out, 16);
    u16(out, 1);
    u16(out, 1);
    u32(out, static_cast<std::uint32_t>(rate));
    u32(out, static_cast<std::uint32_t>(rate * 2));
    u16(out, 2);
    u16(out, 16);
    out.write("data", 4);
    u32(out, data);
    for (const std::int16_t sample : samples) {
        u16(out, static_cast<std::uint16_t>(sample));
    }
}

// np.arange(n, dtype=np.int16): 0, 1, 2, ... wrapping at 32768.
inline std::vector<std::int16_t> arange_int16(std::size_t count) {
    std::vector<std::int16_t> samples(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples[i] = static_cast<std::int16_t>(static_cast<std::uint16_t>(i & 0xFFFF));
    }
    return samples;
}

// _write_wav(path, seconds, rate) of test_meeting_recovery.py.
inline void write_arange_wav(const std::filesystem::path& path, double seconds = 1.0,
                             int rate = 16000) {
    write_wav(path, arange_int16(static_cast<std::size_t>(seconds * rate)), rate);
}

inline void write_bytes(const std::filesystem::path& path, std::span<const std::int16_t> samples) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(samples.data()),
              static_cast<std::streamsize>(samples.size() * sizeof(std::int16_t)));
}

// What one decode returns, or the error it throws.
struct ScriptedDecode {
    std::vector<Segment> segments;
    std::optional<std::string> error;
};

// The script every ScriptedBackend of a rig shares.
// Affinity: thread-safe.
class SpeechScript {
public:
    void set_fallback(ScriptedDecode decode) {
        const std::lock_guard lock(mutex_);
        fallback_ = std::move(decode);
    }
    void set_ready(speech::Readiness readiness) {
        const std::lock_guard lock(mutex_);
        readiness_ = std::move(readiness);
    }
    std::vector<std::pair<speech::Lane, std::string>> calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }
    speech::Readiness readiness() const {
        const std::lock_guard lock(mutex_);
        return readiness_;
    }
    // While held, decodes wait at the gate.
    void hold() {
        const std::lock_guard lock(mutex_);
        held_ = true;
    }
    void resume() {
        {
            const std::lock_guard lock(mutex_);
            held_ = false;
        }
        changed_.notify_all();
    }
    // Answers each decode from its job instead of the fallback.
    void set_responder(std::function<ScriptedDecode(const speech::DecodeJob&)> respond) {
        const std::lock_guard lock(mutex_);
        respond_ = std::move(respond);
    }
    ScriptedDecode take(const speech::DecodeJob& job) {
        std::unique_lock lock(mutex_);
        calls_.emplace_back(job.lane, job.options.language);
        changed_.wait(lock, [this] { return !held_; });
        return respond_ ? respond_(job) : fallback_;
    }

private:
    std::function<ScriptedDecode(const speech::DecodeJob&)> respond_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool held_ = false;
    ScriptedDecode fallback_;
    speech::Readiness readiness_{speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 | cpu", ""};
    std::vector<std::pair<speech::Lane, std::string>> calls_;
};

class ScriptedBackend final : public speech::Backend {
public:
    explicit ScriptedBackend(std::shared_ptr<SpeechScript> script) : script_(std::move(script)) {}

    std::string_view id() const override { return "parakeet"; }
    speech::Capabilities capabilities() const override { return {}; }
    speech::Readiness load(const CancelToken&) override { return script_->readiness(); }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        ScriptedDecode next = script_->take(job);
        if (next.error) {
            throw EngineError(*next.error);
        }
        speech::DecodeResult result;
        result.transcript.segments = std::move(next.segments);
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<SpeechScript> script_;
};

// A real SpeechService over ScriptedBackends; select() loads the dictation slot, which the
// meeting slot falls back to.
struct SpeechRig {
    SpeechRig()
        : service(
              [script = script](const speech::BackendConfig&) -> std::unique_ptr<speech::Backend> {
                  return std::make_unique<ScriptedBackend>(script);
              },
              notify) {}

    void select() {
        speech::BackendConfig config;
        config.backend = "parakeet";
        config.model_key = "parakeet-v3";
        config.model_label = "Parakeet TDT 0.6B v3";
        service.select(speech::Slot::Dictation, config);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (service.readiness(speech::Slot::Meeting).state == speech::ReadyState::Loading &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    std::shared_ptr<SpeechScript> script = std::make_shared<SpeechScript>();
    ManualExecutor notify;
    speech::SpeechService service;
};

// What an AsrFactory was asked for.
struct FactoryCall {
    std::string meeting_id;
    std::string model;
    std::string language;
};

// An AsrFactory over the rig and `repository` that records its calls (and `on_build` runs
// first).
inline AsrFactory recording_factory(SpeechRig& rig, std::shared_ptr<MeetingRepository> repository,
                                    std::shared_ptr<std::vector<FactoryCall>> calls,
                                    std::function<void()> on_build = {}) {
    return [&rig, repository = std::move(repository), calls = std::move(calls),
            on_build = std::move(on_build)](const std::string& meeting_id, const std::string& model,
                                            const std::string& language) {
        if (on_build) {
            on_build();
        }
        calls->push_back({meeting_id, model, language});
        auto engine = std::make_unique<MeetingAsrEngine>(rig.service, repository, meeting_id,
                                                         language, system_clock());
        AsrTiming timing;
        timing.retry_backoff_s = {0.0, 0.0};
        timing.load_poll_s = 0.005;
        engine->set_timing(timing);
        return engine;
    };
}

// recording_factory's engine, but held through `alive`: the engine keeps the repository through
// a pointer that shares ownership with a fresh token, so `*alive` expires exactly when the
// engine is destroyed (its destructor stops it and joins its worker first). The repository
// itself stays owned by the factory.
inline AsrFactory tracking_factory(SpeechRig& rig, std::shared_ptr<MeetingRepository> repository,
                                   std::shared_ptr<std::weak_ptr<void>> alive) {
    return [&rig, repository = std::move(repository), alive = std::move(alive)](
               const std::string& meeting_id, const std::string&, const std::string& language) {
        auto token = std::make_shared<int>(0);
        *alive = token;
        std::shared_ptr<MeetingRepository> held(token, repository.get());
        auto engine = std::make_unique<MeetingAsrEngine>(rig.service, std::move(held), meeting_id,
                                                         language, system_clock());
        AsrTiming timing;
        timing.retry_backoff_s = {0.0, 0.0};
        timing.load_poll_s = 0.005;
        engine->set_timing(timing);
        return engine;
    };
}

} // namespace fv::meeting::test
