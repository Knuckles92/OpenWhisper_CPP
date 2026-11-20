// MeetingEngine with a remote speech route (tests/test_meeting_remote.py): the route the paired
// host answered with is on the meeting's row, with the model it serves, by the time the engine
// asks for its ASR (the row is the first thing a start writes, before capture opens). Connecting to
// the host and pinning its model is src/remote's (tests/remote_host/test_meeting_remote.cpp); the
// engine here is given the pinned route, as MeetingRuntime's engine factory hands it over.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "engine_support.h"
#include "pyport/json.h"
#include "remote/meeting_route.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::FakeAsr;

TEST_CASE("test_engine_persists_actual_remote_route_before_capture", "[test_meeting_remote]") {
    EngineRig rig;
    const Json route{{"fingerprint", std::string(64, 'A')},
                     {"host_name", "devbox"},
                     {"family", "parakeet"},
                     {"model", "parakeet-v3"}};
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    setup.options.asr_model = "parakeet-v3";
    setup.options.asr_language = "de";
    setup.options.asr_remote = route;
    // What the row looked like when the engine asked for its ASR.
    std::optional<Json> row_at_asr;
    std::string language_at_asr;
    setup.asr = [&](const std::string& meeting_id, const std::string& model,
                    const std::string& language) {
        row_at_asr = rig.sql.repo->get_meeting(meeting_id);
        language_at_asr = language;
        return std::make_shared<FakeAsr>(rig.sql.repo, meeting_id, model, language);
    };
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();

    REQUIRE(row_at_asr.has_value());
    CHECK(row_at_asr->at("asr_model") == "parakeet-v3");
    REQUIRE(row_at_asr->at("asr_remote_json").is_string());
    CHECK(fv::py::loads(row_at_asr->at("asr_remote_json").get<std::string>()) == route);
    // The saved route reads back as a remote meeting's.
    CHECK(fv::remote::saved_meeting_route(*row_at_asr) == std::optional<Json>(route));
    CHECK(language_at_asr == "de");
    CHECK_FALSE(rig.mics().empty());
    const Json speech = engine.store()->snapshot().at("speech");
    CHECK(speech.at("source") == "remote");
    CHECK(speech.at("host") == "devbox");
    CHECK(speech.at("model") == "parakeet-v3");
    CHECK(speech.at("connected") == true);
    CHECK(speech.at("message") == "Transcribing on devbox");

    EngineRig::end_and_wait(engine);
}
