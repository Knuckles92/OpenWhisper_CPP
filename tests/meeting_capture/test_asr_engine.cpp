// MeetingAsrEngine (meeting/asr/engine.py) over a real SpeechService with a scripted backend and
// the in-memory repository: timestamps, digital silence, the three attempts, blocked audio, the
// bounded queue and its SQLite refill, stop, and the language every meeting decode carries.
// From tests/test_meeting_asr.py, plus the C++ engine's own seams.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "asr_test_support.h"
#include "base/testing/fake_clock.h"
#include "capture_test_support.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/segment_ids.h"

using Catch::Approx;
using fv::Json;
using fv::meeting::MAX_ATTEMPTS;
using fv::meeting::MAX_IN_MEMORY_CHUNKS;
using fv::meeting::MeetingAsrEngine;
using fv::meeting::test::LogCapture;
using fv::meeting::test::RecordingRepository;
using fv::meeting::test::register_chunk;
using fv::meeting::test::ScriptedDecode;
using fv::meeting::test::SpeechRig;
using fv::meeting::test::TempDir;
using fv::meeting_state::SpooledChunk;
using fv::meeting_state::TranscriptSegment;
using fv::speech::Lane;

namespace {

using namespace std::chrono_literals;

// An engine whose speech service is ready, as _make_engine's stand-in backend is.
struct EngineRig {
    explicit EngineRig(std::string language = "", fv::meeting::AsrCallbacks callbacks = {},
                       const fv::Clock& clock = fv::system_clock())
        : engine(speech.service, repository, "m_test", std::move(language), clock,
                 std::move(callbacks)) {
        speech.select();
        fv::meeting::AsrTiming timing;
        timing.retry_backoff_s = {0.01, 0.02};
        engine.set_timing(timing);
        REQUIRE(engine.load_backend({}));
    }

    // Commits the way the engine's caller does: store, mark done.
    void commit_done() {
        engine.start([this](const SpooledChunk& chunk, std::vector<TranscriptSegment> segments) {
            {
                const std::lock_guard lock(mutex);
                received.emplace_back(chunk, segments);
            }
            repository->set_chunk_status(chunk.chunk_id, "done", std::nullopt);
        });
    }

    std::vector<std::pair<SpooledChunk, std::vector<TranscriptSegment>>> results() {
        const std::lock_guard lock(mutex);
        return received;
    }

    Json row(std::int64_t chunk_id) {
        for (const Json& found : repository->get_audio_chunks("m_test")) {
            if (found.at("id") == chunk_id) {
                return found;
            }
        }
        return Json();
    }

    TempDir folder;
    SpeechRig speech;
    std::shared_ptr<RecordingRepository> repository = std::make_shared<RecordingRepository>();
    MeetingAsrEngine engine;
    std::mutex mutex;
    std::vector<std::pair<SpooledChunk, std::vector<TranscriptSegment>>> received;
};

ScriptedDecode segments(std::vector<fv::Segment> made) {
    return ScriptedDecode{std::move(made), std::nullopt};
}

ScriptedDecode failure(std::string message) {
    return ScriptedDecode{{}, std::move(message)};
}

} // namespace

TEST_CASE("emits segments with meeting timestamps", "[test_meeting_asr]") {
    // from
    // tests/test_meeting_asr.py::TestAsrSuccessPath::test_emits_segments_with_meeting_timestamps
    EngineRig rig;
    rig.speech.script->push(segments({{"  hello world  ", 0.5, 1.5}, {"", 2.0, 3.0}}));
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 10.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(5s));
    const auto received = rig.results();
    REQUIRE(received.size() == 1);
    const auto& segs = received[0].second;
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].text == "hello world");
    CHECK(segs[0].start_s == Approx(10.5));
    CHECK(segs[0].end_s == Approx(11.5));
    CHECK(segs[0].meeting_id == "m_test");
    CHECK(segs[0].channel == "mic");
    CHECK(segs[0].chunk_id == chunk.chunk_id);
    CHECK(segs[0].segment_id == fv::meeting::stable_segment_id("m_test", chunk.chunk_id, 0));
    const auto statuses = rig.repository->statuses_of(chunk.chunk_id);
    CHECK(std::ranges::find(statuses, "done") != statuses.end());
    // Python asserts language None (Whisper's own detection); the speech service needs "auto".
    const auto calls = rig.speech.script->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].lane == Lane::Meeting);
    CHECK(calls[0].slot == fv::speech::Slot::Meeting);
    CHECK(calls[0].language == "auto");
    // The chunk's 0.5 s at 16 kHz, as the decoder's float samples.
    CHECK(calls[0].samples.size() == 8000);
    CHECK(calls[0].samples.front() == Approx(1000.0f / 32768.0f));
}

TEST_CASE("segment ordinals count the empty segments that are dropped", "[asr_engine]") {
    EngineRig rig;
    rig.speech.script->push(segments({{"", 0.0, 0.2}, {" ", 0.2, 0.3}, {"kept", 0.3, 0.4}}));
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 7, 0.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(5s));
    const auto received = rig.results();
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].second.size() == 1);
    CHECK(received[0].second[0].segment_id ==
          fv::meeting::stable_segment_id("m_test", chunk.chunk_id, 2));
}

TEST_CASE("digital silence skips whisper but commits chunk", "[test_meeting_asr]") {
    // from
    // tests/test_meeting_asr.py::TestAsrSuccessPath::test_digital_silence_skips_whisper_but_commits_chunk
    EngineRig rig;
    rig.commit_done();
    const SpooledChunk chunk =
        register_chunk(*rig.repository, rig.folder.path(), 1, 10.0, "m_test", "mic", 0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(5s));
    CHECK(rig.speech.script->calls().empty());
    const auto received = rig.results();
    REQUIRE(received.size() == 1);
    CHECK(received[0].first == chunk);
    CHECK(received[0].second.empty());
    const auto statuses = rig.repository->statuses_of(chunk.chunk_id);
    CHECK(std::ranges::find(statuses, "done") != statuses.end());
}

TEST_CASE("a peak of eight is digital silence and nine is not", "[asr_engine]") {
    const std::vector<std::int16_t> quiet{0, -8, 8, 3};
    const std::vector<std::int16_t> heard{0, -9, 1};
    CHECK(fv::meeting::is_digital_silence(quiet));
    CHECK_FALSE(fv::meeting::is_digital_silence(heard));
    CHECK(fv::meeting::is_digital_silence(std::vector<std::int16_t>{}));
}

TEST_CASE("retries three times then gives up", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_retries_three_times_then_gives_up
    std::vector<std::string> failures;
    std::mutex mutex;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_chunk_failure = [&](const std::string& message) {
        const std::lock_guard lock(mutex);
        failures.push_back(message);
    };
    EngineRig rig("", callbacks);
    rig.speech.script->set_fallback(failure("boom"));
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 10.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(10s));
    CHECK(rig.speech.script->calls().size() == static_cast<std::size_t>(MAX_ATTEMPTS));
    const auto statuses = rig.repository->statuses_of(chunk.chunk_id);
    CHECK(std::ranges::count(statuses, "failed") == MAX_ATTEMPTS);
    CHECK(rig.row(chunk.chunk_id).at("asr_error") == "boom");
    const std::lock_guard lock(mutex);
    CHECK(failures == std::vector<std::string>{
                          "Saved audio chunk 1: Transcription failed after three attempts. Audio "
                          "is saved; check the speech engine and retry this meeting."});
}

TEST_CASE("remote host congestion refunds the durable attempt and resumes the same chunk",
          "[test_meeting_asr]") {
    std::vector<bool> notifications;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_connection_status = [&](const std::string&, bool connected) {
        notifications.push_back(connected);
    };
    EngineRig rig("", callbacks);
    rig.speech.script->push({{}, "Host disconnected.", fv::EngineError::Retry::Connection});
    rig.speech.script->push({{}, "Host busy.", fv::EngineError::Retry::Capacity});
    rig.speech.script->push(segments({{"recovered", 0.0, 1.0}}));
    rig.commit_done();
    const auto chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(5s));
    CHECK(rig.row(chunk.chunk_id).at("asr_attempts") == 1);
    CHECK(rig.results().at(0).second.at(0).text == "recovered");
    CHECK(notifications == std::vector<bool>{false, true, true});
}

TEST_CASE("TestAsrRetry::test_remote_disconnect_after_attempt_refunds_budget",
          "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py. The remote meeting backend reports a dropped request as a
    // Connection EngineError with no suggested wait (RemoteMeetingUnavailable), so the engine's
    // REMOTE_RETRY_BACKOFF_S applies, shortened here as Python patches it.
    EngineRig rig;
    fv::meeting::AsrTiming timing;
    timing.retry_backoff_s = {0.01, 0.02};
    timing.remote_retry_backoff_s = {0.01};
    rig.engine.set_timing(timing);
    rig.speech.script->push(
        {{}, "Host dropped the request", fv::EngineError::Retry::Connection, std::nullopt});
    rig.speech.script->push(segments({{"returned", 0.0, 0.2}}));
    rig.commit_done();
    const auto chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(3s));
    CHECK(rig.speech.script->calls().size() == 2);
    CHECK(rig.row(chunk.chunk_id).at("asr_attempts") == 1);
    CHECK(rig.row(chunk.chunk_id).at("asr_status") == "done");
}

TEST_CASE("TestAsrRetry::test_remote_host_busy_waits_without_spending_decode_budget",
          "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py. Python records the 20 ms waits it asks for; here every gap
    // is at least the host's 20 ms (not the 10 ms floor), and the fallback backoff is made 30 s
    // so the 3 s drain could not finish had it been used instead of the host's suggestion.
    EngineRig rig;
    fv::meeting::AsrTiming timing;
    timing.retry_backoff_s = {0.01, 0.02};
    timing.remote_retry_backoff_s = {30.0};
    timing.remote_busy_min_retry_s = 0.01;
    rig.engine.set_timing(timing);
    for (std::int64_t i = 0; i < MAX_ATTEMPTS + 1; ++i) {
        rig.speech.script->push({{}, "Host at capacity", fv::EngineError::Retry::Capacity, 0.02});
    }
    rig.speech.script->push(segments({{"eventually", 0.0, 0.2}}));
    rig.commit_done();
    const auto chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(3s));
    const auto calls = rig.speech.script->calls();
    REQUIRE(calls.size() == static_cast<std::size_t>(MAX_ATTEMPTS + 2));
    for (std::size_t i = 1; i < calls.size(); ++i) {
        CHECK(calls[i].at - calls[i - 1].at >= 20ms);
    }
    CHECK(rig.row(chunk.chunk_id).at("asr_attempts") == 1);
    CHECK(rig.row(chunk.chunk_id).at("asr_status") == "done");
    const auto statuses = rig.repository->statuses_of(chunk.chunk_id);
    CHECK(std::ranges::find(statuses, "failed") == statuses.end());
    CHECK(rig.results().at(0).second.at(0).text == "eventually");
}

TEST_CASE("succeeds after transient failures", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_succeeds_after_transient_failures
    EngineRig rig;
    rig.speech.script->push(failure("transient"));
    rig.speech.script->push(failure("transient"));
    rig.speech.script->push(segments({{"recovered", 0.0, 1.0}}));
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 5.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(10s));
    CHECK(rig.speech.script->calls().size() == 3);
    const auto received = rig.results();
    REQUIRE(received.size() == 1);
    REQUIRE(received[0].second.size() == 1);
    CHECK(received[0].second[0].text == "recovered");
    CHECK(received[0].second[0].start_s == Approx(5.0));
    const auto statuses = rig.repository->statuses_of(chunk.chunk_id);
    CHECK(std::ranges::find(statuses, "done") != statuses.end());
}

TEST_CASE("missing wav is blocked without retries", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_missing_wav_is_blocked_without_retries
    std::vector<std::string> failures;
    std::mutex mutex;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_chunk_failure = [&](const std::string& message) {
        const std::lock_guard lock(mutex);
        failures.push_back(message);
    };
    EngineRig rig("", callbacks);
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 10.0);
    std::filesystem::remove(rig.folder.path() / "c1.wav");
    std::atomic<int> commits{0};
    rig.engine.start([&](const SpooledChunk&, std::vector<TranscriptSegment>) { ++commits; });
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(3s));
    const Json row = rig.row(chunk.chunk_id);
    CHECK(row.at("asr_status") == "blocked");
    CHECK(row.at("asr_attempts") == 1);
    CHECK_THAT(row.at("asr_error").get<std::string>(),
               Catch::Matchers::ContainsSubstring("restore or replace") &&
                   Catch::Matchers::StartsWith(
                       "Saved audio cannot be read ([Errno 2] No such file or directory: '"));
    {
        const std::lock_guard lock(mutex);
        REQUIRE_FALSE(failures.empty());
        CHECK_THAT(failures[0], Catch::Matchers::ContainsSubstring("Saved audio chunk 1"));
    }
    CHECK(commits.load() == 0);
    CHECK(rig.speech.script->calls().empty());
    CHECK(rig.engine.requeue_pending() == 0);
}

TEST_CASE("a WAV that is not 16-bit is blocked as unreadable", "[asr_engine]") {
    EngineRig rig;
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    {
        // An 8-bit header: wave reads it, load_wav_int16 refuses it.
        std::fstream file(chunk.file_path, std::ios::binary | std::ios::in | std::ios::out);
        file.seekp(34);
        const char bits[2] = {8, 0};
        file.write(bits, 2);
    }
    rig.commit_done();
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(3s));
    const Json row = rig.row(chunk.chunk_id);
    CHECK(row.at("asr_status") == "blocked");
    CHECK_THAT(row.at("asr_error").get<std::string>(),
               Catch::Matchers::StartsWith(
                   "Saved audio cannot be read (Expected 16-bit PCM WAV, got 8-bit: "));
    CHECK(rig.speech.script->calls().empty());
}

TEST_CASE("transient retries are spaced", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_transient_retries_are_spaced
    EngineRig rig;
    fv::meeting::AsrTiming timing;
    timing.retry_backoff_s = {0.1, 0.2};
    rig.engine.set_timing(timing);
    rig.speech.script->set_fallback(failure("temporary backend failure"));
    rig.engine.start([](const SpooledChunk&, std::vector<TranscriptSegment>) {});
    REQUIRE(rig.engine.enqueue(register_chunk(*rig.repository, rig.folder.path(), 1, 10.0)));
    REQUIRE(rig.engine.drain(3s));
    const auto calls = rig.speech.script->calls();
    REQUIRE(calls.size() == static_cast<std::size_t>(MAX_ATTEMPTS));
    CHECK(calls[1].at - calls[0].at >= 80ms);
    CHECK(calls[2].at - calls[1].at >= 180ms);
}

TEST_CASE("a commit that throws is retried", "[asr_engine]") {
    EngineRig rig;
    rig.speech.script->set_fallback(segments({{"words", 0.0, 0.5}}));
    std::atomic<int> commits{0};
    rig.engine.start([&](const SpooledChunk& chunk, std::vector<TranscriptSegment>) {
        if (++commits == 1) {
            throw std::runtime_error("database is locked");
        }
        rig.repository->set_chunk_status(chunk.chunk_id, "done", std::nullopt);
    });
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(5s));
    CHECK(commits.load() == 2);
    CHECK(rig.repository->statuses_of(chunk.chunk_id) ==
          std::vector<std::string>{"processing", "failed", "processing", "done"});
    CHECK(rig.row(chunk.chunk_id).at("asr_attempts") == 2);
}

TEST_CASE("bounded queue refills from persisted rows", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_bounded_queue_refills_from_persisted_rows
    std::vector<std::pair<std::string, bool>> overload;
    std::mutex mutex;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_backlog_status = [&](const std::string& message, bool active) {
        const std::lock_guard lock(mutex);
        overload.emplace_back(message, active);
    };
    EngineRig rig("", callbacks);
    std::vector<SpooledChunk> chunks;
    for (int index = 1; index <= static_cast<int>(MAX_IN_MEMORY_CHUNKS) + 1; ++index) {
        chunks.push_back(register_chunk(*rig.repository, rig.folder.path(), index, index));
    }
    for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
        REQUIRE(rig.engine.enqueue(chunks[i]));
    }
    CHECK(rig.engine.outstanding() == MAX_IN_MEMORY_CHUNKS);
    CHECK_FALSE(rig.engine.enqueue(chunks.back()));
    {
        const std::lock_guard lock(mutex);
        REQUIRE_FALSE(overload.empty());
        CHECK(
            overload.back() ==
            std::pair<std::string, bool>{
                "Live transcription is behind; audio is saved and captions will catch up.", true});
    }
    rig.commit_done();
    REQUIRE(rig.engine.drain(10s));
    for (const Json& row : rig.repository->get_audio_chunks("m_test")) {
        CHECK(row.at("asr_status") == "done");
    }
    const std::lock_guard lock(mutex);
    CHECK(overload.back() == std::pair<std::string, bool>{"Live transcription caught up.", false});
    // Chunks were decoded in meeting-time order, the refilled one last.
    std::vector<double> starts;
    for (const auto& [chunk, unused] : rig.results()) {
        starts.push_back(chunk.start_s);
    }
    CHECK(std::ranges::is_sorted(starts));
    CHECK(starts.size() == chunks.size());
}

TEST_CASE("stop keeps deferred chunks replayable", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestAsrRetry::test_stop_keeps_deferred_chunks_replayable
    TempDir folder;
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<RecordingRepository>();
    std::vector<SpooledChunk> chunks;
    for (int index = 1; index <= static_cast<int>(MAX_IN_MEMORY_CHUNKS) + 1; ++index) {
        chunks.push_back(register_chunk(*repository, folder.path(), index, index));
    }
    {
        MeetingAsrEngine first(speech.service, repository, "m_test", "", fv::system_clock());
        REQUIRE(first.load_backend({}));
        for (std::size_t i = 0; i + 1 < chunks.size(); ++i) {
            first.enqueue(chunks[i]);
        }
        CHECK_FALSE(first.enqueue(chunks.back()));
        first.stop();
        CHECK(first.outstanding() == 0);
    }
    CHECK(repository->get_pending_chunks("m_test", std::nullopt, {}).size() == chunks.size());

    MeetingAsrEngine second(speech.service, repository, "m_test", "", fv::system_clock());
    REQUIRE(second.load_backend({}));
    second.start([&](const SpooledChunk& chunk, std::vector<TranscriptSegment>) {
        repository->set_chunk_status(chunk.chunk_id, "done", std::nullopt);
    });
    CHECK(second.requeue_pending() == MAX_IN_MEMORY_CHUNKS);
    REQUIRE(second.drain(10s));
    for (const Json& row : repository->get_audio_chunks("m_test")) {
        CHECK(row.at("asr_status") == "done");
    }
}

TEST_CASE("failed refill does not poll database in a hot loop", "[test_meeting_asr]") {
    // from
    // tests/test_meeting_asr.py::TestAsrRetry::test_failed_refill_does_not_poll_database_in_a_hot_loop
    EngineRig rig;
    rig.repository->fail_pending_reads(true);
    const LogCapture logs;
    CHECK_FALSE(rig.engine.drain(300ms));
    CHECK(rig.repository->pending_reads() == 1);
    CHECK(logs.levels("Could not refill saved ASR chunks for m_test") ==
          std::vector{fv::log::Level::Error});
}

TEST_CASE("requeue_pending takes given rows in meeting-time order and skips queued ones",
          "[asr_engine]") {
    EngineRig rig;
    const SpooledChunk a = register_chunk(*rig.repository, rig.folder.path(), 1, 3.0);
    const SpooledChunk b = register_chunk(*rig.repository, rig.folder.path(), 2, 1.0);
    const SpooledChunk c = register_chunk(*rig.repository, rig.folder.path(), 3, 2.0);
    REQUIRE(rig.engine.enqueue(c));
    const std::vector<Json> rows = rig.repository->get_pending_chunks("m_test", std::nullopt, {});
    CHECK(rig.engine.requeue_pending(rows) == 2);
    CHECK(rig.engine.outstanding() == 3);
    rig.commit_done();
    REQUIRE(rig.engine.drain(5s));
    std::vector<std::int64_t> order;
    for (const auto& [chunk, unused] : rig.results()) {
        order.push_back(chunk.chunk_id);
    }
    CHECK(order == std::vector<std::int64_t>{c.chunk_id, b.chunk_id, a.chunk_id});
}

TEST_CASE("configured language always wins", "[test_meeting_asr]") {
    // from tests/test_meeting_asr.py::TestOfflineLanguage::test_configured_language_always_wins
    // Parakeet reports no language probabilities, so no vote exists: the configured language
    // goes to every decode, live and offline.
    EngineRig rig(" FR ");
    CHECK(rig.engine.language() == "fr");
    rig.speech.script->set_fallback(segments({{"bonjour", 0.0, 0.4}}));
    rig.commit_done();
    REQUIRE(rig.engine.enqueue(register_chunk(*rig.repository, rig.folder.path(), 1, 0.0)));
    REQUIRE(rig.engine.drain(5s));
    std::vector<std::int16_t> session(16000, 2000);
    fv::meeting::test::write_wav_samples(rig.folder.path() / "mic_session.wav", session);
    const auto offline = rig.engine.transcribe_offline_session(rig.folder.path(), {}, {}, {});
    CHECK_FALSE(offline.empty());
    const auto calls = rig.speech.script->calls();
    REQUIRE(calls.size() >= 2);
    for (const auto& call : calls) {
        CHECK(call.language == "fr");
    }
}

TEST_CASE("no meeting decode carries an empty language", "[asr_engine]") {
    // An empty language on the Meeting and Background lanes would be the dictation language.
    for (const std::string configured : {"", "   ", "auto", "de"}) {
        EngineRig rig(configured);
        const std::string expected = configured == "de" ? "de" : "auto";
        rig.speech.script->set_fallback(segments({{"words", 0.0, 0.4}}));
        rig.commit_done();
        REQUIRE(rig.engine.enqueue(register_chunk(*rig.repository, rig.folder.path(), 1, 0.0)));
        REQUIRE(rig.engine.drain(5s));
        std::vector<std::int16_t> session(16000, 2000);
        fv::meeting::test::write_wav_samples(rig.folder.path() / "loopback_session.wav", session);
        rig.engine.transcribe_offline_session(rig.folder.path(), {}, {}, {});
        const auto meeting = rig.speech.script->calls_on(Lane::Meeting);
        const auto background = rig.speech.script->calls_on(Lane::Background);
        REQUIRE(meeting.size() == 1);
        REQUIRE(background.size() == 1);
        CHECK(meeting[0].language == expected);
        CHECK(background[0].language == expected);
        CHECK(background[0].slot == fv::speech::Slot::Meeting);
    }
}

TEST_CASE("load_backend waits for the meeting slot and reports an unusable engine",
          "[asr_engine]") {
    SECTION("ready after a slow load") {
        SpeechRig speech;
        speech.script->set_load_delay(200ms);
        auto repository = std::make_shared<RecordingRepository>();
        MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fv::system_clock());
        CHECK_FALSE(engine.is_available());
        fv::speech::BackendConfig config;
        config.backend = "parakeet";
        speech.service.select(fv::speech::Slot::Dictation, config);
        CHECK(engine.load_backend({}));
        CHECK(engine.is_available());
    }
    SECTION("a missing model") {
        SpeechRig speech;
        speech.script->set_ready({fv::speech::ReadyState::ModelMissing,
                                  "Download Parakeet TDT 0.6B v3 in Downloads.",
                                  "Download Parakeet TDT 0.6B v3 in Downloads."});
        speech.select();
        auto repository = std::make_shared<RecordingRepository>();
        MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fv::system_clock());
        CHECK_FALSE(engine.load_backend({}));
        CHECK_FALSE(engine.is_available());
        CHECK(engine.last_error() == "Download Parakeet TDT 0.6B v3 in Downloads.");
        CHECK(engine.transcribe_offline_session(".", {}, {}, {}).empty());
    }
    SECTION("an engine that is warming up") {
        SpeechRig speech;
        speech.script->set_ready(
            {fv::speech::ReadyState::Warming, "Parakeet TDT 0.6B v3 | cpu", "Warming up"});
        speech.select();
        auto repository = std::make_shared<RecordingRepository>();
        MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fv::system_clock());
        CHECK(engine.load_backend({}));
        CHECK(engine.is_available());
        CHECK(engine.last_error().empty());
    }
    SECTION("cancelled while nothing is selected") {
        SpeechRig speech;
        auto repository = std::make_shared<RecordingRepository>();
        MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fv::system_clock());
        fv::CancelSource cancel;
        std::thread canceller([&] {
            std::this_thread::sleep_for(100ms);
            cancel.cancel();
        });
        CHECK_FALSE(engine.load_backend(cancel.token()));
        canceller.join();
    }
}

TEST_CASE("an unavailable engine blocks chunks for a manual retry", "[asr_engine]") {
    TempDir folder;
    SpeechRig speech;
    speech.select();
    auto repository = std::make_shared<RecordingRepository>();
    std::vector<std::string> failures;
    std::mutex mutex;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_chunk_failure = [&](const std::string& message) {
        const std::lock_guard lock(mutex);
        failures.push_back(message);
    };
    MeetingAsrEngine engine(speech.service, repository, "m_test", "auto", fv::system_clock(),
                            callbacks);
    // load_backend never ran: the engine is not available.
    engine.start([](const SpooledChunk&, std::vector<TranscriptSegment>) {});
    const SpooledChunk chunk = register_chunk(*repository, folder.path(), 1, 0.0);
    REQUIRE(engine.enqueue(chunk));
    REQUIRE(engine.drain(3s));
    CHECK(repository->statuses_of(chunk.chunk_id) == std::vector<std::string>{"blocked"});
    const Json row = repository->get_audio_chunks("m_test").at(0);
    CHECK(row.at("asr_error") ==
          "Speech engine unavailable; select or install a model, then retry saved audio");
    const std::lock_guard lock(mutex);
    CHECK(failures == std::vector<std::string>{"Saved audio chunk 1: Speech engine unavailable. "
                                               "Select or install a model, then retry saved "
                                               "audio."});
}

TEST_CASE("an engine released under the meeting blocks its chunks", "[asr_engine]") {
    EngineRig rig;
    rig.speech.service.release_local(fv::speech::Slot::Dictation);
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (rig.speech.service.readiness(fv::speech::Slot::Meeting).state !=
               fv::speech::ReadyState::Offline &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(2ms);
    }
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.engine.drain(3s));
    CHECK(rig.repository->statuses_of(chunk.chunk_id) == std::vector<std::string>{"blocked"});
    CHECK(rig.speech.script->calls().empty());
}

TEST_CASE("a chunk that waited 15 s in the queue is logged once", "[asr_engine]") {
    fv::FakeClock clock;
    EngineRig rig("", {}, clock);
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    REQUIRE(rig.engine.enqueue(chunk));
    clock.advance(16s);
    const LogCapture logs;
    rig.commit_done();
    REQUIRE(rig.engine.drain(5s));
    CHECK(logs.levels("Meeting ASR chunk 1 waited 16.0s (queued behind it: 0)") ==
          std::vector{fv::log::Level::Warning});
}

TEST_CASE("stop cancels the decode and never waits on the speech thread", "[asr_engine]") {
    EngineRig rig;
    fv::meeting::AsrTiming timing;
    timing.stop_join_s = 0.2;
    rig.engine.set_timing(timing);
    rig.speech.script->hold();
    rig.commit_done();
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    REQUIRE(rig.engine.enqueue(chunk));
    REQUIRE(rig.speech.script->wait_entered());
    const LogCapture logs;
    const auto started = std::chrono::steady_clock::now();
    rig.engine.stop();
    CHECK(std::chrono::steady_clock::now() - started < 2s);
    CHECK(logs.levels("Meeting ASR worker did not stop within 10s") ==
          std::vector{fv::log::Level::Warning});
    CHECK_FALSE(rig.engine.enqueue(register_chunk(*rig.repository, rig.folder.path(), 2, 1.0)));
    // The window finishes on the speech thread; its result is dropped and the chunk stays
    // unfinished for recovery.
    rig.speech.script->resume();
    // This stop must see the worker end, however slow a loaded machine is.
    timing.stop_join_s = 30.0;
    rig.engine.set_timing(timing);
    rig.engine.stop();
    CHECK(rig.results().empty());
    // The cut short attempt is refunded: the chunk waits as it did before it was taken.
    const Json row = rig.row(chunk.chunk_id);
    CHECK(row.at("asr_status") == "pending");
    CHECK(row.at("asr_attempts") == 0);
    CHECK(std::ranges::count(rig.repository->statuses_of(chunk.chunk_id), "failed") == 0);
    CHECK(rig.repository->get_pending_chunks("m_test", std::nullopt, {}).size() == 2);
}

TEST_CASE("a stop during a chunk's last attempt does not give the chunk up", "[asr_engine]") {
    std::vector<std::string> failures;
    std::mutex mutex;
    fv::meeting::AsrCallbacks callbacks;
    callbacks.on_chunk_failure = [&](const std::string& message) {
        const std::lock_guard lock(mutex);
        failures.push_back(message);
    };
    EngineRig rig("", callbacks);
    const SpooledChunk chunk = register_chunk(*rig.repository, rig.folder.path(), 1, 0.0);
    // Two attempts were spent before a restart.
    for (int i = 0; i < 2; ++i) {
        rig.repository->set_chunk_status(chunk.chunk_id, "processing", std::nullopt);
        rig.repository->set_chunk_status(chunk.chunk_id, "failed", "boom");
    }
    rig.speech.script->hold();
    rig.commit_done();
    CHECK(rig.engine.requeue_pending() == 1);
    REQUIRE(rig.speech.script->wait_entered());
    std::thread resume([&] {
        std::this_thread::sleep_for(100ms);
        rig.speech.script->resume();
    });
    rig.engine.stop();
    resume.join();
    rig.engine.stop();
    CHECK(rig.results().empty());
    const Json row = rig.row(chunk.chunk_id);
    CHECK(row.at("asr_status") == "pending");
    CHECK(row.at("asr_attempts") == MAX_ATTEMPTS - 1);
    CHECK(rig.repository->get_pending_chunks("m_test", std::nullopt, {}).size() == 1);
    const std::lock_guard lock(mutex);
    CHECK(failures.empty());
}

TEST_CASE("two threads may stop the engine at once", "[asr_engine]") {
    for (int round = 0; round < 20; ++round) {
        EngineRig rig;
        rig.commit_done();
        std::thread other([&] { rig.engine.stop(); });
        rig.engine.stop();
        other.join();
        CHECK(rig.engine.outstanding() == 0);
        CHECK_FALSE(rig.engine.is_available());
    }
}

TEST_CASE("start after stop runs a new worker", "[asr_engine]") {
    EngineRig rig;
    rig.commit_done();
    rig.engine.stop();
    CHECK_FALSE(rig.engine.is_available());
    // A stopped engine loads nothing until it is started again.
    CHECK_FALSE(rig.engine.load_backend({}));
    rig.commit_done();
    REQUIRE(rig.engine.load_backend({}));
    rig.speech.script->set_fallback(segments({{"again", 0.0, 0.2}}));
    REQUIRE(rig.engine.enqueue(register_chunk(*rig.repository, rig.folder.path(), 1, 0.0)));
    REQUIRE(rig.engine.drain(5s));
    CHECK(rig.results().size() == 1);
}
