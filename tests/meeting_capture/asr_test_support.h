#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
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
#include <vector>

#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "meeting_state/repository.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "speech/service.h"

// Seams for the meeting ASR tests: a scripted speech backend behind a real SpeechService (so jobs
// cross the "fv-speech" thread with their lanes, slots and languages), a repository that records
// what the engine does to it, and WAV files as Python's tests write them.
namespace fv::meeting::test {

// What one decode returns, or the error it throws.
struct ScriptedDecode {
    std::vector<Segment> segments;
    std::optional<std::string> error;
    EngineError::Retry retry = EngineError::Retry::None;
    // The wait a retryable error suggests (a busy host's retry_after_ms); nullopt: none, and the
    // engine's own remote backoff applies.
    std::optional<double> retry_after_s = 0.5;
};

// One decode the backend took.
struct DecodeCall {
    speech::Lane lane = speech::Lane::Final;
    speech::Slot slot = speech::Slot::Dictation;
    std::string language;
    std::string coalesce_key;
    std::vector<float> samples;
    std::chrono::steady_clock::time_point at;
};

// The script every ScriptedBackend of a rig shares. Decodes take the queued results in order,
// then `fallback`; while held, they wait at the gate.
// Affinity: thread-safe.
class SpeechScript {
public:
    void push(ScriptedDecode decode) {
        const std::lock_guard lock(mutex_);
        queued_.push_back(std::move(decode));
    }
    void set_fallback(ScriptedDecode decode) {
        const std::lock_guard lock(mutex_);
        fallback_ = std::move(decode);
    }
    void set_ready(speech::Readiness readiness) {
        const std::lock_guard lock(mutex_);
        readiness_ = std::move(readiness);
    }
    void set_load_delay(std::chrono::milliseconds delay) {
        const std::lock_guard lock(mutex_);
        load_delay_ = delay;
    }
    void set_accepts_files(bool accepts) {
        const std::lock_guard lock(mutex_);
        accepts_files_ = accepts;
    }
    // Capabilities::live_preview: false stands for a backend that does not preview (a Whisper
    // model on a paired host).
    void set_live_preview(bool previews) {
        const std::lock_guard lock(mutex_);
        live_preview_ = previews;
    }
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
    // Waits until a decode is at the gate.
    bool wait_entered(std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [this] { return waiting_ > 0; });
    }
    bool wait_calls(std::size_t count,
                    std::chrono::milliseconds timeout = std::chrono::seconds(10)) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, timeout, [&] { return calls_.size() >= count; });
    }
    std::vector<DecodeCall> calls() const {
        const std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<DecodeCall> calls_on(speech::Lane lane) const {
        std::vector<DecodeCall> out;
        for (DecodeCall& call : calls()) {
            if (call.lane == lane) {
                out.push_back(std::move(call));
            }
        }
        return out;
    }

    speech::Readiness readiness() const {
        const std::lock_guard lock(mutex_);
        return readiness_;
    }
    std::chrono::milliseconds load_delay() const {
        const std::lock_guard lock(mutex_);
        return load_delay_;
    }
    bool accepts_files() const {
        const std::lock_guard lock(mutex_);
        return accepts_files_;
    }
    bool live_preview() const {
        const std::lock_guard lock(mutex_);
        return live_preview_;
    }

    ScriptedDecode take(const speech::DecodeJob& job) {
        DecodeCall call;
        call.lane = job.lane;
        call.slot = job.slot;
        call.language = job.options.language;
        call.coalesce_key = job.coalesce_key;
        call.at = std::chrono::steady_clock::now();
        if (const auto* clip = std::get_if<speech::AudioClip>(&job.input)) {
            call.samples.assign(clip->samples->begin() + static_cast<std::ptrdiff_t>(clip->begin),
                                clip->samples->begin() + static_cast<std::ptrdiff_t>(clip->end));
        }
        std::unique_lock lock(mutex_);
        calls_.push_back(std::move(call));
        changed_.notify_all();
        if (held_) {
            ++waiting_;
            changed_.notify_all();
            changed_.wait(lock, [this] { return !held_; });
            --waiting_;
        }
        if (queued_.empty()) {
            return fallback_;
        }
        ScriptedDecode next = std::move(queued_.front());
        queued_.pop_front();
        return next;
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::deque<ScriptedDecode> queued_;
    ScriptedDecode fallback_;
    speech::Readiness readiness_{speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 | cpu", ""};
    std::chrono::milliseconds load_delay_{0};
    bool accepts_files_ = false;
    bool live_preview_ = true;
    bool held_ = false;
    int waiting_ = 0;
    std::vector<DecodeCall> calls_;
};

// speech::Backend on the script. Runs on the speech thread.
class ScriptedBackend final : public speech::Backend {
public:
    explicit ScriptedBackend(std::shared_ptr<SpeechScript> script) : script_(std::move(script)) {}

    std::string_view id() const override { return "parakeet"; }
    speech::Capabilities capabilities() const override {
        speech::Capabilities out;
        out.accepts_files = script_->accepts_files();
        out.live_preview = script_->live_preview();
        return out;
    }
    speech::Readiness load(const CancelToken&) override {
        std::this_thread::sleep_for(script_->load_delay());
        return script_->readiness();
    }
    speech::DecodeResult decode(const speech::DecodeJob& job) override {
        ScriptedDecode next = script_->take(job);
        if (next.error) {
            throw EngineError(*next.error, next.retry, next.retry_after_s);
        }
        speech::DecodeResult result;
        for (const Segment& segment : next.segments) {
            if (!result.transcript.text.empty() && !segment.text.empty()) {
                result.transcript.text += " ";
            }
            result.transcript.text += segment.text;
        }
        result.transcript.segments = std::move(next.segments);
        return result;
    }
    void release() override {}

private:
    std::shared_ptr<SpeechScript> script_;
};

// A real SpeechService over ScriptedBackends, its dictation slot selected (the meeting slot
// falls back to it).
struct SpeechRig {
    SpeechRig()
        : service(
              [script = script](const speech::BackendConfig&) -> std::unique_ptr<speech::Backend> {
                  return std::make_unique<ScriptedBackend>(script);
              },
              notify) {}

    // Selects the dictation slot and waits until it left Loading.
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

// FakeMeetingRepository with a record of chunk status writes and switchable failures.
// Affinity: thread-safe.
class RecordingRepository final : public meeting_state::MeetingRepository {
public:
    struct Status {
        std::int64_t chunk_id;
        std::string status;
        std::optional<std::string> error;
    };

    meeting_state::testing::FakeMeetingRepository inner;

    std::vector<Status> statuses() const {
        const std::lock_guard lock(mutex_);
        return statuses_;
    }
    std::vector<std::string> statuses_of(std::int64_t chunk_id) const {
        std::vector<std::string> out;
        for (const Status& status : statuses()) {
            if (status.chunk_id == chunk_id) {
                out.push_back(status.status);
            }
        }
        return out;
    }
    void fail_pending_reads(bool fail) { fail_pending_ = fail; }
    int pending_reads() const { return pending_reads_.load(); }

    void create_meeting(const Json& fields) override { inner.create_meeting(fields); }
    void update_meeting(std::string_view id, const Json& fields) override {
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
    std::vector<Json> list_meetings() override { return inner.list_meetings(); }
    void delete_meeting(std::string_view id) override { inner.delete_meeting(id); }
    void heartbeat(std::string_view id, std::int64_t pid, std::string_view utc) override {
        inner.heartbeat(id, pid, utc);
    }
    std::vector<Json> find_interrupted_meetings() override {
        return inner.find_interrupted_meetings();
    }
    std::int64_t register_chunk(const Json& fields) override {
        return inner.register_chunk(fields);
    }
    meeting_state::ChunkRegistration register_chunk_if_missing(const Json& fields) override {
        return inner.register_chunk_if_missing(fields);
    }
    void set_chunk_status(std::int64_t chunk_id, std::string_view status,
                          std::optional<std::string_view> error) override {
        {
            const std::lock_guard lock(mutex_);
            statuses_.push_back(
                {chunk_id, std::string(status),
                 error ? std::optional<std::string>(std::string(*error)) : std::nullopt});
        }
        inner.set_chunk_status(chunk_id, status, error);
    }
    std::vector<Json> get_pending_chunks(std::string_view id, std::optional<std::int64_t> limit,
                                         const std::vector<std::int64_t>& exclude) override {
        ++pending_reads_;
        if (fail_pending_.load()) {
            throw std::runtime_error("database unavailable");
        }
        return inner.get_pending_chunks(id, limit, exclude);
    }
    std::int64_t count_unfinished_chunks(std::string_view id) override {
        return inner.count_unfinished_chunks(id);
    }
    std::int64_t reset_unfinished_chunks(std::string_view id, bool force) override {
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
        return inner.commit_chunk_transcription(id, chunk_id, s);
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
    std::vector<Json> get_segments(std::string_view id, double after,
                                   std::optional<std::int64_t> limit) override {
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
    std::vector<Status> statuses_;
    std::atomic<bool> fail_pending_{false};
    std::atomic<int> pending_reads_{0};
};

// tests/helpers.py write_wav: a mono 16-bit WAV of `value` for duration_s seconds.
inline void write_wav(const std::filesystem::path& path, std::int16_t value = 1000,
                      double duration_s = 0.5, int rate = 16000, int channels = 1) {
    const auto frames = static_cast<std::size_t>(duration_s * rate);
    const std::vector<std::int16_t> samples(frames * static_cast<std::size_t>(channels), value);
    const auto u32 = [](std::ofstream& out, std::uint32_t v) {
        const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                               static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.write(bytes, 4);
    };
    const auto u16 = [](std::ofstream& out, std::uint16_t v) {
        const char bytes[2] = {static_cast<char>(v), static_cast<char>(v >> 8)};
        out.write(bytes, 2);
    };
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
    std::ofstream out(path, std::ios::binary);
    out.write("RIFF", 4);
    u32(out, 36 + data);
    out.write("WAVEfmt ", 8);
    u32(out, 16);
    u16(out, 1);
    u16(out, static_cast<std::uint16_t>(channels));
    u32(out, static_cast<std::uint32_t>(rate));
    u32(out, static_cast<std::uint32_t>(rate * channels * 2));
    u16(out, static_cast<std::uint16_t>(channels * 2));
    u16(out, 16);
    out.write("data", 4);
    u32(out, data);
    for (const std::int16_t sample : samples) {
        u16(out, static_cast<std::uint16_t>(sample));
    }
}

// The same for given samples.
inline void write_wav_samples(const std::filesystem::path& path,
                              std::span<const std::int16_t> samples, int rate = 16000) {
    write_wav(path, 0, 0.0, rate);
    std::fstream out(path, std::ios::binary | std::ios::in | std::ios::out);
    out.seekp(0, std::ios::end);
    for (const std::int16_t sample : samples) {
        const char bytes[2] = {static_cast<char>(sample & 0xFF),
                               static_cast<char>((static_cast<std::uint16_t>(sample) >> 8) & 0xFF)};
        out.write(bytes, 2);
    }
    const auto data = static_cast<std::uint32_t>(samples.size() * 2);
    const auto patch = [&](std::streamoff at, std::uint32_t v) {
        const char bytes[4] = {static_cast<char>(v), static_cast<char>(v >> 8),
                               static_cast<char>(v >> 16), static_cast<char>(v >> 24)};
        out.seekp(at);
        out.write(bytes, 4);
    };
    patch(4, 36 + data);
    patch(40, data);
}

// A registered chunk: its WAV written, its row added, and the SpooledChunk the spool would hand on.
inline meeting_state::SpooledChunk register_chunk(meeting_state::MeetingRepository& repository,
                                                  const std::filesystem::path& folder, int index,
                                                  double start_s, std::string meeting_id = "m_test",
                                                  std::string channel = "mic",
                                                  std::int16_t value = 1000) {
    const std::filesystem::path path = folder / ("c" + std::to_string(index) + ".wav");
    write_wav(path, value);
    Json fields = Json::object();
    fields["meeting_id"] = meeting_id;
    fields["channel"] = channel;
    fields["seq"] = index;
    fields["file_path"] = path_to_utf8(path);
    fields["start_s"] = start_s;
    fields["duration_s"] = 0.5;
    fields["sample_rate"] = 16000;
    meeting_state::SpooledChunk chunk;
    chunk.chunk_id = repository.register_chunk(fields);
    chunk.meeting_id = std::move(meeting_id);
    chunk.channel = std::move(channel);
    chunk.seq = index;
    chunk.file_path = path_to_utf8(path);
    chunk.start_s = start_s;
    chunk.duration_s = 0.5;
    chunk.sample_rate = 16000;
    return chunk;
}

} // namespace fv::meeting::test
