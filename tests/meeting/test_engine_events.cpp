// MeetingEngine's own surfaces that OpenWhisper's tests leave to the code (meeting/engine.py
// _heartbeat_loop, _emit, _on_asr_backlog_status, _on_asr_chunk_failure): the heartbeat on the
// meeting strand, listeners that throw, and the ASR's status callbacks reaching the state, the
// listeners and the dashboard.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "engine_support.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::InlineExecutor;

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

EngineSetup transcript_only() {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    return setup;
}

std::string heartbeat_at(EngineRig& rig, const std::string& meeting_id) {
    return rig.sql.repo->get_meeting(meeting_id)->at("app_heartbeat_at").get<std::string>();
}

} // namespace

TEST_CASE("the heartbeat writes the process id every ten seconds on the meeting strand until End",
          "[test_meeting_engine]") {
    // engine.py _heartbeat_loop and HEARTBEAT_INTERVAL_S (A24-06): the row's pid and UTC time,
    // every 10 s while the meeting is live, never after End.
    // Before the rig, whose destructor shuts the engine down.
    fv::FakeClock clock;
    fv::ManualExecutor strand(clock);
    EngineRig rig;
    EngineSetup setup = transcript_only();
    setup.clock = &clock;
    setup.strand = &strand;
    setup.pid = 4242;
    MeetingEngine& engine = rig.make(setup);
    engine.start();
    const std::string id = engine.meeting_id();
    const std::string started = heartbeat_at(rig, id);
    CHECK(rig.sql.repo->get_meeting(id)->at("app_pid") == 4242);
    REQUIRE(strand.pending_timers() == 1);

    strand.advance(milliseconds(9'999));
    CHECK(heartbeat_at(rig, id) == started);
    strand.advance(milliseconds(1));
    const std::string first = heartbeat_at(rig, id);
    CHECK(first != started);
    CHECK(rig.sql.repo->get_meeting(id)->at("app_pid") == 4242);
    // The next one is armed again.
    CHECK(strand.pending_timers() == 1);
    strand.advance(seconds(10));
    const std::string second = heartbeat_at(rig, id);
    CHECK(second != first);

    EngineRig::end_and_wait(engine);
    CHECK(strand.pending_timers() == 0);
    strand.advance(seconds(30));
    CHECK(heartbeat_at(rig, id) == second);
}

TEST_CASE("a listener that throws is logged and the other listeners still hear every event",
          "[test_meeting_engine]") {
    // engine.py _emit: "Meeting listener raised for %s event" is logged and the event goes on.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    InlineExecutor inline_executor;
    // Events come from the start, the speaker model's thread and End: the listeners run there.
    std::mutex mutex;
    int raised = 0;
    std::vector<std::string> heard;
    const fv::Subscription thrower =
        engine.add_listener(inline_executor, [&](const fv::meeting::MeetingEvent&) {
            {
                const std::lock_guard lock(mutex);
                ++raised;
            }
            throw std::runtime_error("listener exploded");
        });
    const fv::Subscription after =
        engine.add_listener(inline_executor, [&](const fv::meeting::MeetingEvent& event) {
            const std::lock_guard lock(mutex);
            heard.push_back(event.kind);
        });

    CHECK_NOTHROW(engine.start());
    REQUIRE(engine.wait_for_processing(seconds(5)));
    EngineRig::end_and_wait(engine);
    REQUIRE(engine.wait_for_end(seconds(10)));
    const std::lock_guard lock(mutex);
    CHECK(raised > 0);
    CHECK(static_cast<std::size_t>(raised) == heard.size());
    // Every event the rig's own listener heard (in an order racing threads may change).
    std::vector<std::string> expected = rig.kinds();
    std::vector<std::string> got = heard;
    std::sort(expected.begin(), expected.end());
    std::sort(got.begin(), got.end());
    CHECK(got == expected);
    CHECK(std::find(heard.begin(), heard.end(), "ended") != heard.end());
}

TEST_CASE("the ASR's backlog and chunk failures reach the state, the listeners and the dashboard",
          "[test_meeting_engine]") {
    // engine.py _on_asr_backlog_status and _on_asr_chunk_failure (A24-11), through the callbacks
    // the engine hands its ASR.
    EngineRig rig;
    MeetingEngine& engine = rig.make(transcript_only());
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto asr = rig.asr(0);
    REQUIRE(asr);
    REQUIRE(asr->callbacks.on_backlog_status);
    REQUIRE(asr->callbacks.on_chunk_failure);

    const std::string behind =
        "Live transcription is behind; audio is saved and captions will catch up.";
    asr->callbacks.on_backlog_status(behind, true);
    Json speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("backlog") == true);
    CHECK(speech.at("message") == behind);
    CHECK(rig.events_of("status").back().at("note") == behind);
    CHECK(rig.events_of("status").back().at("speech").at("backlog") == true);
    const Json shown = rig.server(0)->messages_of("status").back();
    CHECK(shown.at("speech").at("backlog") == true);
    CHECK_FALSE(shown.contains("note"));

    asr->callbacks.on_backlog_status("Live transcription caught up.", false);
    speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("backlog") == false);
    CHECK(speech.at("message") == "Live transcription caught up.");

    const std::string failure = "Saved audio chunk 7: the file is truncated.";
    asr->callbacks.on_chunk_failure(failure);
    speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("chunk_error") == failure);
    CHECK(speech.at("message") == failure);
    CHECK(speech.at("backlog") == false);
    CHECK(rig.events_of("status").back().at("note") == failure);
    const Json error = rig.events_of("error").back();
    CHECK(error.at("code") == "asr_chunk_failed");
    CHECK(error.at("message") == failure +
                                     " After fixing the audio file or speech engine, restart the "
                                     "app and retry this meeting from Recovery.");
}
