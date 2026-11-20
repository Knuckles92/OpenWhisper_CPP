// transcription_readiness_message (Parakeet branch) from services/application_controller.py,
// replayed from the golden vectors of services.application_controller.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "appcore/readiness.h"
#include "base/json.h"
#include "golden/golden.h"

using fv::Json;
using fv::appcore::ReadinessInput;
using fv::appcore::transcription_readiness_message;

TEST_CASE("readiness replays the golden vectors", "[golden][readiness]") {
    fv::golden::for_each(
        "services.application_controller", "transcription_readiness_message",
        [](const fv::golden::Case& c) {
            const Json flags = c.arg<Json>(0);
            const Json expected = c.result<Json>();
            ReadinessInput input;
            input.has_backend = flags.at("has_backend").get<bool>();
            input.available = flags.at("available").get<bool>();
            input.reload_in_flight = flags.at("reload_in_flight").get<bool>();
            input.model_missing = flags.at("model_missing").get<bool>();
            input.closed_by_cancel = flags.at("closed_by_cancel").get<bool>();
            input.has_last_error = flags.at("has_last_error").get<bool>();
            input.device_info = expected.at("device_info").get<std::string>();

            const auto decision = transcription_readiness_message(input);
            CHECK(decision.message ==
                  fv::golden::decode<std::optional<std::string>>(expected.at("message")));
            CHECK(decision.request_reload == expected.at("request_reload").get<bool>());
            CHECK(decision.request_model_consent ==
                  expected.at("request_model_consent").get<bool>());
        });
}

TEST_CASE("a loaded engine is ready and asks for nothing", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.available = true;
    const auto decision = transcription_readiness_message(input);
    CHECK_FALSE(decision.message);
    CHECK_FALSE(decision.request_reload);
    CHECK_FALSE(decision.request_model_consent);
}

TEST_CASE("no engine selected", "[readiness]") {
    CHECK(transcription_readiness_message(ReadinessInput{}).message ==
          "No transcription engine is selected");
}

TEST_CASE("an engine still loading says so and asks for nothing", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.reload_in_flight = true;
    input.model_missing = true;
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.message == "Speech engine is still loading...");
    CHECK_FALSE(decision.request_model_consent);
}

TEST_CASE("a missing model asks for consent and shows the engine's sentence", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.model_missing = true;
    input.device_info = "Download Parakeet TDT 0.6B v3 in Downloads.";
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.request_model_consent);
    CHECK_FALSE(decision.request_reload);
    CHECK(decision.message == "Download Parakeet TDT 0.6B v3 in Downloads.");
}

// from
// tests/test_controller_thread_hops.py::test_a_hotkey_readiness_check_really_reloads_the_engine
// (the decision half: closed by a Cancel, or never loaded)
TEST_CASE("an engine closed by cancel or never loaded is reloaded", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.device_info = "Parakeet is not loaded";
    SECTION("never loaded") {}
    SECTION("closed by a cancel") {
        input.closed_by_cancel = true;
        input.has_last_error = true;
        input.device_info = "Parakeet stopped.";
    }
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.request_reload);
    CHECK(decision.message == "Reloading speech engine...");
}

TEST_CASE("a load that failed shows its error and is not retried", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.has_last_error = true;
    input.device_info = "Install Parakeet's GPU runtime in Downloads.";
    const auto decision = transcription_readiness_message(input);
    CHECK_FALSE(decision.request_reload);
    CHECK(decision.message == "Install Parakeet's GPU runtime in Downloads.");
}

TEST_CASE("readiness of an engine that is not a speech engine replays the golden vectors",
          "[golden][readiness]") {
    fv::golden::for_each(
        "services.application_controller", "transcription_readiness_message.other_engine",
        [](const fv::golden::Case& c) {
            ReadinessInput input;
            input.has_backend = true;
            input.available = c.arg<bool>(0);
            input.backend_id = c.arg<std::string>(1);
            const Json expected = c.result<Json>();
            const auto decision = transcription_readiness_message(input);
            CHECK(decision.message ==
                  fv::golden::decode<std::optional<std::string>>(expected.at("message")));
            CHECK(decision.request_reload == expected.at("request_reload").get<bool>());
            CHECK(decision.request_model_consent ==
                  expected.at("request_model_consent").get<bool>());
        });
}

TEST_CASE("an unavailable API engine asks for a valid key and starts nothing", "[readiness]") {
    ReadinessInput input;
    input.has_backend = true;
    input.backend_id = "api";
    // The speech-engine states do not apply to it.
    input.reload_in_flight = true;
    input.model_missing = true;
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.message == "The selected API transcription engine needs a valid API key");
    CHECK_FALSE(decision.request_reload);
    CHECK_FALSE(decision.request_model_consent);

    input.backend_id = "something_else";
    CHECK(transcription_readiness_message(input).message ==
          "The selected transcription engine is unavailable");
    input.available = true;
    CHECK_FALSE(transcription_readiness_message(input).message);
    input.has_backend = false;
    CHECK(transcription_readiness_message(input).message == "No transcription engine is selected");
}

TEST_CASE("an engine named by default is the speech engine", "[readiness]") {
    CHECK(ReadinessInput{}.backend_id == "parakeet");
    // The M1 members keep their order: the engine's id was added after them.
    const ReadinessInput positional{true, false, false, true, false, false, "Get the model."};
    CHECK(positional.has_backend);
    CHECK(positional.model_missing);
    CHECK(positional.device_info == "Get the model.");
    CHECK(positional.backend_id == "parakeet");
    CHECK(transcription_readiness_message(positional).request_model_consent);
}
