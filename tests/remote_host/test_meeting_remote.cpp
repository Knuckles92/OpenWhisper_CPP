// Meeting audio through an actual paired TLS host, with fake speech inference
// (tests/test_meeting_remote.py): the meeting's ASR worker (meeting_capture's MeetingAsrEngine)
// over a SpeechService whose Meeting slot is the paired computer, as MeetingRuntime builds it for
// a meeting with a remote route. Each case carries its OpenWhisper function's name.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <future>
#include <mutex>

#include "../meeting_capture/asr_test_support.h"
#include "../meeting_capture/capture_test_support.h"
#include "base/testing/manual_executor.h"
#include "host_rig.h"
#include "meeting_capture/asr_engine.h"
#include "meeting_capture/capture_block.h"
#include "paired_backend.h"

using namespace fv;
using namespace fv::remote_host;
using namespace fv::remote_host::rig;
using Catch::Approx;
using Catch::Matchers::ContainsSubstring;
using fv::meeting::MeetingAsrEngine;
using fv::meeting::test::RecordingRepository;
using fv::meeting::test::register_chunk;
using fv::meeting_state::SpooledChunk;
using fv::meeting_state::TranscriptSegment;
using fv::speech::ReadyState;

namespace {

// A meeting whose speech is the paired computer's: the route pins the host, the SpeechService's
// Meeting slot is a RemoteSpeechBackend of its own (MeetingRuntime's default engine factory).
struct RemoteMeeting {
    explicit RemoteMeeting(std::string language = "", meeting::AsrCallbacks callbacks = {})
        : route(std::make_shared<Json>(
              Json{{"fingerprint", paired.pairing->fingerprint}, {"host_name", "devbox"}})),
          speech(
              [this](const speech::BackendConfig&) {
                  return paired.make(nullptr, system_clock(), route);
              },
              notify),
          asr(speech, repository, "m_remote", std::move(language), system_clock(),
              std::move(callbacks)) {
        repository->create_meeting(Json{{"id", "m_remote"},
                                        {"title", "Remote meeting"},
                                        {"status", "active"},
                                        {"started_at", "2026-09-27T12:00:00Z"},
                                        {"host_token", "host"},
                                        {"guest_token", "guest"},
                                        {"spool_dir", path_to_utf8(folder.path())}});
    }

    // Selects the Meeting slot and waits until the load has an answer.
    ReadyState connect() {
        speech.select(speech::Slot::Meeting, {.backend = "remote"});
        wait_for(
            [&] { return speech.readiness(speech::Slot::Meeting).state != ReadyState::Loading; },
            10s);
        return speech.readiness(speech::Slot::Meeting).state;
    }

    // Commits as the meeting engine does: the segments stored and the chunk done, in one step.
    void commit_chunks() {
        asr.start([this](const SpooledChunk& chunk, std::vector<TranscriptSegment> segments) {
            (void)repository->commit_chunk_transcription("m_remote", chunk.chunk_id, segments);
        });
    }

    SpooledChunk chunk(std::string channel, int index, double start_s) {
        return register_chunk(*repository, folder.path(), index, start_s, "m_remote",
                              std::move(channel));
    }

    PairedBackend paired;
    meeting::test::TempDir folder;
    std::shared_ptr<RecordingRepository> repository = std::make_shared<RecordingRepository>();
    std::shared_ptr<Json> route;
    InlineExecutor notify;
    speech::SpeechService speech;
    MeetingAsrEngine asr;
};

} // namespace

TEST_CASE("test_live_mic_and_system_audio_use_remote_model_and_meeting_timestamps") {
    RemoteMeeting meeting("de");
    REQUIRE(meeting.connect() == ReadyState::Ready);
    REQUIRE(meeting.asr.load_backend({}));
    REQUIRE(meeting.asr.is_available());
    meeting.commit_chunks();
    REQUIRE(meeting.asr.enqueue(meeting.chunk("mic", 1, 10.0)));
    REQUIRE(meeting.asr.enqueue(meeting.chunk("loopback", 2, 20.0)));
    REQUIRE(meeting.asr.drain(5s));

    const auto segments = meeting.repository->get_segments("m_remote", -1.0, std::nullopt);
    REQUIRE(segments.size() == 2);
    // The host's times are within each chunk; the meeting's start where the chunk started.
    CHECK(segments[0].at("channel") == "mic");
    CHECK(segments[0].at("start_s").get<double>() == Approx(10.0));
    CHECK(segments[0].at("end_s").get<double>() == Approx(10.5));
    CHECK(segments[0].at("text") == "heard 8000");
    CHECK(segments[1].at("channel") == "loopback");
    CHECK(segments[1].at("start_s").get<double>() == Approx(20.0));
    CHECK(segments[1].at("end_s").get<double>() == Approx(20.5));
    const auto calls = meeting.paired.host.engine->calls();
    REQUIRE(calls.size() == 2);
    for (const auto& call : calls) {
        CHECK(call.language == std::optional<std::string>("de"));
    }
    CHECK(meeting.repository->count_unfinished_chunks("m_remote") == 0);
    // The route now names the model that decoded the meeting.
    CHECK(meeting.route->at("model") == "parakeet-v3");
    CHECK(meeting.route->at("family") == "parakeet");
}

TEST_CASE("test_remote_outage_keeps_chunks_pending_and_stop_is_prompt") {
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> status;
    meeting::AsrCallbacks callbacks;
    callbacks.on_connection_status = [&](const std::string& message, bool connected) {
        std::lock_guard lock(mutex);
        status.emplace_back(message, connected);
    };
    RemoteMeeting meeting("", callbacks);
    REQUIRE(meeting.connect() == ReadyState::Ready);
    REQUIRE(meeting.asr.load_backend({}));
    meeting.paired.stop_host();
    meeting.asr.start([](const SpooledChunk&, std::vector<TranscriptSegment>) {
        FAIL("No transcript while offline");
    });
    const auto pending = meeting.chunk("mic", 1, 0.0);
    REQUIRE(meeting.asr.enqueue(pending));
    REQUIRE(wait_for([&] {
        std::lock_guard lock(mutex);
        return !status.empty();
    }));
    {
        std::lock_guard lock(mutex);
        CHECK_FALSE(status.back().second);
        CHECK_THAT(status.back().first, ContainsSubstring("Couldn't reach"));
        CHECK_THAT(status.back().first,
                   ContainsSubstring("Audio is saved locally; retrying the remote connection."));
    }
    // The outage spent none of the chunk's decode attempts.
    const auto rows = meeting.repository->get_pending_chunks("m_remote", std::nullopt, {});
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].at("asr_attempts") == 0);

    // Stop does not sit out the retry delay.
    auto stopped = std::async(std::launch::async, [&] { meeting.asr.stop(); });
    CHECK(stopped.wait_for(2s) == std::future_status::ready);
    CHECK(meeting.repository->count_unfinished_chunks("m_remote") == 1);
    CHECK(meeting.paired.host.engine->calls().empty());
}

TEST_CASE("test_live_worker_resumes_pending_audio_after_host_returns") {
    std::mutex mutex;
    std::vector<bool> status;
    std::vector<std::string> messages;
    meeting::AsrCallbacks callbacks;
    // What the meeting engine sets for a remote route (asr/engine.py _check_remote_ready).
    callbacks.connected_message = "Transcribing on devbox";
    callbacks.on_connection_status = [&](const std::string& message, bool connected) {
        std::lock_guard lock(mutex);
        status.push_back(connected);
        messages.push_back(message);
    };
    RemoteMeeting meeting("", callbacks);
    REQUIRE(meeting.connect() == ReadyState::Ready);
    REQUIRE(meeting.asr.load_backend({}));
    meeting.paired.stop_host();
    meeting.commit_chunks();
    REQUIRE(meeting.asr.enqueue(meeting.chunk("mic", 1, 10.0)));
    REQUIRE(meeting.asr.enqueue(meeting.chunk("loopback", 2, 20.0)));
    REQUIRE(wait_for([&] {
        std::lock_guard lock(mutex);
        return std::ranges::find(status, false) != status.end();
    }));
    CHECK(meeting.repository->count_unfinished_chunks("m_remote") == 2);
    meeting.paired.start_host();
    // The worker's first pause after an outage is 5 s (its backoff is not a test seam here).
    REQUIRE(meeting.asr.drain(15s));
    CHECK(meeting.repository->count_unfinished_chunks("m_remote") == 0);
    {
        std::lock_guard lock(mutex);
        REQUIRE_FALSE(status.empty());
        CHECK(status.back());
        CHECK(messages.back() == "Transcribing on devbox");
    }
    CHECK(meeting.paired.host.engine->calls().size() == 2);
    meeting.asr.stop();
}

TEST_CASE("test_remote_preview_emits_text_for_host_model") {
    // The Parakeet case; a Nemotron host (the test's other parameter) streams, which neither
    // OpenWhisper_CPP's host engine nor its meeting route has.
    RemoteMeeting meeting;
    REQUIRE(meeting.connect() == ReadyState::Ready);
    REQUIRE(meeting.asr.load_backend({}));
    std::mutex mutex;
    std::vector<Json> events;
    meeting.asr.start_preview([&](const Json& preview) {
        std::lock_guard lock(mutex);
        events.push_back(preview);
    });
    const std::vector<std::int16_t> frames(16000, 1000);
    meeting.asr.feed_preview(
        meeting::CaptureBlock{"mic", frames.data(), frames.size(), 16000, 100.0}, 0.0);
    REQUIRE(wait_for(
        [&] {
            std::lock_guard lock(mutex);
            return !events.empty();
        },
        10s));
    {
        std::lock_guard lock(mutex);
        CHECK(events[0].at("channel") == "mic");
        CHECK_FALSE(events[0].at("text").get<std::string>().empty());
    }
    const auto calls = meeting.paired.host.engine->calls();
    REQUIRE_FALSE(calls.empty());
    CHECK(calls[0].op == "transcribe");
    meeting.asr.stop();
}

TEST_CASE("test_unavailable_remote_aborts_before_recording") {
    // The part of it that is the route's: a host with nothing loaded leaves the meeting's slot
    // unusable with the host's reason, which MeetingRuntime's engine factory throws before it
    // builds the engine that would open capture.
    RemoteMeeting meeting;
    meeting.paired.host.engine->set_available(false);
    CHECK(meeting.connect() == ReadyState::Offline);
    const auto readiness = meeting.speech.readiness(speech::Slot::Meeting);
    CHECK_THAT(readiness.message, ContainsSubstring("not loaded"));
    CHECK(readiness.message == "devbox: not loaded");
    CHECK_FALSE(meeting.asr.load_backend({}));
    CHECK_FALSE(meeting.asr.is_available());
    CHECK(meeting.paired.host.engine->calls().empty());
    CHECK_FALSE(meeting.route->contains("model"));
}
