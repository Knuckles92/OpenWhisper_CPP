// services/meeting_rerun.py: what a re-run of a past meeting's steps resolves.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>

#include "base/testing/fake_clock.h"
#include "meeting/finalization.h"
#include "meeting/rerun_options.h"

using fv::Json;
using namespace fv::meeting;

namespace {

const fv::FakeClock& before_shutdown() {
    static const fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 11 / 16},
                                     "UTC");
    return clock;
}

} // namespace

TEST_CASE("the meeting's recorded model wins and settings fill the rest", "[meeting][rerun]") {
    const Json settings = {{"meeting_language", "de"},
                           {"meeting_redecode_coverage_guard", true},
                           {"meeting_audio_upload_consent_given", true},
                           {"meeting_speaker_id_backend", "off"}};
    const RerunOptions options =
        rerun_options({{"asr_model", "orukeet-v0.1"}}, settings, before_shutdown());
    CHECK(options.timeout_s == 900.0);
    CHECK(options.asr_model_name == "orukeet-v0.1");
    CHECK(options.language == "de");
    CHECK(options.speaker_id_backend == "off");
    CHECK(options.speaker_audio_consent);
    CHECK(options.redecode_coverage_guard);
}

TEST_CASE("a meeting without a recorded model uses the meeting speech model", "[meeting][rerun]") {
    const RerunOptions options = rerun_options(Json(), Json::object(), before_shutdown());
    CHECK(options.asr_model_name == "parakeet-v3");
    CHECK(options.language == "auto");
    CHECK(options.speaker_id_backend == "local");
    CHECK_FALSE(options.speaker_audio_consent);
}

TEST_CASE("a remote meeting takes its route's language", "[meeting][rerun]") {
    const SavedRemoteRoute remote = [](const Json& meeting) -> std::optional<Json> {
        if (!meeting.contains("route")) {
            return std::nullopt;
        }
        return std::optional<Json>(meeting.at("route"));
    };
    const Json settings = {{"meeting_language", "de"}};
    CHECK(rerun_options({{"route", {{"language", "fr"}}}}, settings, before_shutdown(), remote)
              .language == "fr");
    CHECK(
        rerun_options({{"route", Json::object()}}, settings, before_shutdown(), remote).language ==
        "auto");
    CHECK(rerun_options(Json::object(), settings, before_shutdown(), remote).language == "de");
}

TEST_CASE("a stored openai backend never reaches the key", "[meeting][rerun]") {
    // FioriVoce reads "openai" as on-device on every date, so the gate refuses before any key.
    const SpeakerPassGate gate = resolve_speaker_pass(
        {{"meeting_speaker_id_backend", "openai"}, {"meeting_audio_upload_consent_given", true}},
        before_shutdown());
    CHECK(gate == SpeakerPassGate{false, std::string(kNotOpenaiReason), false});
    CHECK(
        rerun_options(Json::object(), {{"meeting_speaker_id_backend", "openai"}}, before_shutdown())
            .speaker_id_backend == "local");
}
