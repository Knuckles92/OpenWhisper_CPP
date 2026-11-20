// MeetingEngine with a remote speech route (tests/test_meeting_engine.py): the paired host's
// connection status in the meeting's speech state. The route itself (the paired computer, its
// confirmation before capture) is src/remote's; the engine's ASR here is the rig's fake.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "engine_support.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;

TEST_CASE("test_remote_connection_update_keeps_backlog_and_chunk_error", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_remote_connection_update_keeps_backlog_and_chunk_error. Python calls
    // _on_asr_connection_status on a local meeting; the engine here hands that callback to the
    // ASR of a remote meeting only, so the meeting has a route and the fake ASR reports through
    // the callback it was given.
    EngineRig rig;
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    setup.options.asr_model = "parakeet-tdt";
    setup.options.asr_remote = Json{{"host_name", "devbox"}, {"model", "parakeet-tdt"}};
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(rig.asr(0));
    REQUIRE(static_cast<bool>(rig.asr(0)->callbacks.on_connection_status));
    const Json started = engine.store()->snapshot().at("speech");
    CHECK(started.at("source") == "remote");
    CHECK(started.at("host") == "devbox");
    CHECK(started.at("connected") == true);

    probe.on_asr_backlog_status("Captions are catching up", true);
    probe.on_asr_chunk_failure("Saved chunk cannot be read");
    rig.asr(0)->callbacks.on_connection_status("Host offline; audio saved locally.", false);

    Json speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("connected") == false);
    CHECK(speech.at("message") == "Host offline; audio saved locally.");
    CHECK(speech.at("backlog") == true);
    CHECK(speech.at("chunk_error") == "Saved chunk cannot be read");

    rig.asr(0)->callbacks.on_connection_status("Remote link restored", true);

    speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("backlog") == true);
    CHECK(speech.at("chunk_error") == "Saved chunk cannot be read");
    CHECK(speech.at("connected") == true);
    CHECK(speech.at("message") == "Remote link restored");
    CHECK(speech.at("source") == "remote");
    CHECK(speech.at("host") == "devbox");
    CHECK(speech.at("model") == "parakeet-tdt");
    // The desktop and the dashboard hear it with the sentence as the note.
    const Json status = rig.events_of("status").back();
    CHECK(status.at("note") == "Remote link restored");
    CHECK(status.at("speech") == speech);
    CHECK(rig.server(0)->messages_of("status").back().at("speech") == speech);

    EngineRig::end_and_wait(engine);
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
}

TEST_CASE("a local meeting's ASR is given no connection status callback", "[test_meeting_engine]") {
    // engine.py _start_asr passes on_connection_status with a remote route only.
    EngineRig rig;
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(rig.asr(0));
    CHECK_FALSE(static_cast<bool>(rig.asr(0)->callbacks.on_connection_status));
    CHECK(engine.store()->snapshot().at("speech").empty());
}
