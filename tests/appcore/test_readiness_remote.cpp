// transcription_readiness_message's Remote branch (_remote_engine_readiness_message in
// services/application_controller.py): a quiet reconnect now, never a reload.
#include <catch2/catch_test_macros.hpp>

#include "appcore/readiness.h"

using fv::appcore::ReadinessInput;
using fv::appcore::transcription_readiness_message;

namespace {

ReadinessInput remote_offline() {
    ReadinessInput input;
    input.has_backend = true;
    input.backend_id = "remote";
    return input;
}

} // namespace

TEST_CASE("a remote engine that could not connect is tried again at once", "[readiness]") {
    auto input = remote_offline();
    input.has_last_error = true;
    input.device_info = "Couldn't reach devbox:47821.";
    input.remote_host = "devbox";
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.message == "Couldn't reach devbox:47821. Trying again...");
    CHECK(decision.request_remote_retry);
    CHECK_FALSE(decision.request_reload);
    CHECK_FALSE(decision.request_model_consent);
}

TEST_CASE("a remote engine never tried or reconnecting says it is connecting", "[readiness]") {
    auto input = remote_offline();
    input.remote_host = "devbox";
    CHECK(transcription_readiness_message(input).message == "Connecting to devbox...");
    input.has_last_error = true;
    input.device_info = "Couldn't reach devbox:47821.";
    input.remote_restoring = true;
    const auto decision = transcription_readiness_message(input);
    CHECK(decision.message == "Connecting to devbox...");
    CHECK(decision.request_remote_retry);
    input.remote_host.clear();
    input.remote_restoring = false;
    input.has_last_error = false;
    CHECK(transcription_readiness_message(input).message == "Connecting to the remote engine...");
}

TEST_CASE("a remote engine still loading or connected asks for no retry", "[readiness]") {
    auto input = remote_offline();
    input.reload_in_flight = true;
    auto decision = transcription_readiness_message(input);
    CHECK(decision.message == "Speech engine is still loading...");
    CHECK_FALSE(decision.request_remote_retry);
    input.reload_in_flight = false;
    input.available = true;
    decision = transcription_readiness_message(input);
    CHECK_FALSE(decision.message);
    CHECK_FALSE(decision.request_remote_retry);
}
