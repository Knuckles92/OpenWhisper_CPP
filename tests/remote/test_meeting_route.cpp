#include "engine/error.h"
#include "models/meeting_speech.h"
#include "remote/meeting_route.h"
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <string>
using fv::Json;

TEST_CASE("saved meeting routes distinguish missing, self-hosted and damaged records") {
    CHECK_FALSE(fv::remote::saved_meeting_route(Json::object()));
    const Json route = {{"fingerprint", "AA"}, {"family", "parakeet"}, {"model", "parakeet-v3"}};
    const Json row = {{"asr_remote_json", route.dump()}};
    CHECK(fv::remote::saved_meeting_route(row) == route);
    CHECK_FALSE(fv::remote::saved_meeting_route(row, "aa"));
    for (const auto& raw : {"{", "[]", "{}", "null"}) {
        CHECK_THROWS_AS(fv::remote::saved_meeting_route(Json{{"asr_remote_json", raw}}),
                        fv::EngineError);
    }
}
TEST_CASE("a paired meeting host must offer the pinned meeting-capable Parakeet model") {
    const Json route = {{"fingerprint", "AA"}, {"host_name", "Desk"}};
    auto ready =
        Json{{"engine", {{"available", true}, {"family", "parakeet"}, {"model", "parakeet-v3"}}}};
    const auto pinned = fv::remote::checked_meeting_engine(route, ready);
    CHECK(pinned.at("model") == "parakeet-v3");
    ready["engine"]["model"] = "unrecognized";
    CHECK_THROWS_AS(fv::remote::checked_meeting_engine(pinned, ready), fv::EngineError);
    ready["engine"]["model"] = "parakeet-v3";
    ready["engine"]["available"] = false;
    CHECK_THROWS_AS(fv::remote::checked_meeting_engine(pinned, ready), fv::EngineError);
    CHECK_THROWS_AS(fv::remote::meeting_route(Json::object()), fv::EngineError);
}

TEST_CASE("test_a_separate_speech_source_defaults_local_and_does_not_follow_dictation",
          "[test_meeting_remote]") {
    using fv::models::resolve_meeting_asr_source;
    CHECK(resolve_meeting_asr_source(Json{{"selected_model", "parakeet"}}) == "local");
    CHECK(resolve_meeting_asr_source(
              Json{{"meeting_speech_separate", true}, {"selected_model", "remote"}}) == "local");
    CHECK(resolve_meeting_asr_source(
              Json{{"meeting_speech_separate", true}, {"meeting_asr_source", "bad"}}) == "local");
    CHECK(resolve_meeting_asr_source(Json{{"meeting_speech_separate", true},
                                          {"meeting_asr_source", "remote"}}) == "remote");
    // Dictation through the paired computer never sends a meeting there by itself.
    CHECK(resolve_meeting_asr_source(Json{{"selected_model", "remote"}}) == "local");
}

TEST_CASE("test_unpaired_remote_has_actionable_error", "[test_meeting_remote]") {
    CHECK_THROWS_WITH(fv::remote::meeting_route(Json::object()),
                      "Pair a computer in Settings → Dictation → Remote engine first.");
    // Paired, with meetings sent there by a saved choice of that very host: the route names it.
    const std::string pin(64, 'A');
    Json settings{{"remote_engine_client",
                   {{"host", "127.0.0.1"},
                    {"port", 47821},
                    {"host_name", "devbox"},
                    {"device_id", "me"},
                    {"fingerprint", pin}}}};
    // a25-remote-meeting-consent: a pairing alone is not a choice to send meeting audio.
    CHECK_THROWS_WITH(fv::remote::meeting_route(settings),
                      "Choose this paired host in Meeting Voice before sending meeting audio.");
    settings["meeting_speech_separate"] = true;
    settings["meeting_asr_source"] = "remote";
    settings["fv_meeting_remote_fingerprint"] = pin;
    CHECK(fv::remote::meeting_route(settings) ==
          Json{{"fingerprint", pin}, {"host_name", "devbox"}});
}
