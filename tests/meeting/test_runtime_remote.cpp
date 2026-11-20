// tests/test_meeting_runtime.py: the meeting runtime with a paired computer transcribing. The
// route and its transport are src/remote's; here a saved pairing in the settings stands for it,
// the live-audio confirmation is a callback and the engine is the rig's fake.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

namespace {

// Meeting speech on a paired computer named "devbox" (the settings a pairing leaves behind).
void pair_remote_speech(RuntimeRig& rig) {
    const std::string pin(64, 'A');
    rig.settings.update(Json{{"meeting_speech_separate", true},
                             {"meeting_asr_source", "remote"},
                             {"fv_meeting_remote_fingerprint", pin},
                             {"remote_engine_client",
                              {{"host", "127.0.0.1"},
                               {"port", 8750},
                               {"host_name", "devbox"},
                               {"device_id", "me"},
                               {"fingerprint", pin}}}});
}

bool contains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(
        lines, [text](const std::string& line) { return line.find(text) != std::string::npos; });
}

} // namespace

// from
// tests/test_meeting_runtime.py::test_remote_meeting_start_does_not_release_local_dictation_model
TEST_CASE("test_remote_meeting_start_does_not_release_local_dictation_model",
          "[test_meeting_runtime]") {
    // A local start frees the dictation model for the meeting's own
    // (test_start_releases_the_dictation_engine_before_loading_its_own); a meeting transcribed
    // elsewhere loads none, so dictation keeps its model.
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.confirm_live_audio = [](const Json&) {
            return true;
        };
    });
    pair_remote_speech(rig);

    rig.rt->start_meeting(false);
    rig.drain();

    REQUIRE(rig.built.size() == 1);
    REQUIRE(rig.built[0].asr_remote);
    CHECK(rig.built[0].asr_remote->at("host_name") == "devbox");
    CHECK(rig.probe().controller_active());
    CHECK(rig.lease->calls().empty());
    // The paired computer's permission covers the live session, not a second pass over its
    // recording.
    CHECK_FALSE(rig.built[0].end_redecode);
}

// from
// tests/test_meeting_runtime.py::test_remote_meeting_does_not_wait_for_unrelated_local_whisper_load
TEST_CASE("test_remote_meeting_does_not_wait_for_unrelated_local_whisper_load",
          "[test_meeting_runtime]") {
    // Adapted: the unrelated load is the local Parakeet engine's.
    int asked = 0;
    RuntimeRig rig([&asked](MeetingRuntimeDeps& deps) {
        deps.engine_loading_message = [&asked] {
            ++asked;
            return std::optional<std::string>("Still loading");
        };
        deps.confirm_live_audio = [](const Json&) {
            return true;
        };
    });

    // On this computer the meeting waits for the engine.
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(rig.built.empty());
    CHECK(contains(rig.got.statuses, "Still loading"));
    CHECK(asked == 1);

    pair_remote_speech(rig);
    rig.got = Recorded{};
    rig.rt->start_meeting(false);
    rig.drain();

    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].asr_remote.has_value());
    CHECK_FALSE(contains(rig.got.statuses, "Still loading"));
    CHECK(rig.probe().controller_active());
}

// from tests/test_meeting_runtime.py::test_remote_connection_status_reaches_desktop
TEST_CASE("test_remote_connection_status_reaches_desktop", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_engine(std::make_shared<FakeEngine>());
    const Json speech{{"source", "remote"},
                      {"host", "devbox"},
                      {"model", "base"},
                      {"connected", false},
                      {"message", "Host offline; audio saved locally."}};

    probe.on_engine_event("status", Json{{"status", "active"},
                                         {"speech", speech},
                                         {"note", "Host offline; audio saved locally."}});
    rig.drain();

    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back().at("speech") == speech);
    CHECK(rig.got.states.back().at("status") == "active");
    // The engine's sentence is the status line.
    CHECK(rig.got.statuses.back() == "Host offline; audio saved locally.");
}
