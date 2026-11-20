// EndRun's set_finalization sequence on each branch (meeting/end_run.py). The golden replay of
// meeting.end_run covers every scripted scenario; these name the branches the engine relies on.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "end_run_scenario.h"
#include "meeting/end_run.h"
#include "meeting/finalization.h"

using fv::Json;
using namespace fv::meeting;
using fv::meeting::testing::finalizations;
using fv::meeting::testing::run_scenario;

namespace {

std::vector<std::string> statuses(const Json& updates) {
    std::vector<std::string> out;
    for (const auto& update : updates) {
        out.push_back(update.at("status").get<std::string>());
    }
    return out;
}

Json step_statuses(const Json& update) {
    Json out = Json::object();
    for (const auto& step : update.at("steps")) {
        out[step.at("id").get<std::string>()] = step.at("status");
    }
    return out;
}

const Json kStats = {
    {"segments", 32}, {"words", 410}, {"key_points", 4}, {"action_items", 0}, {"decisions", 1}};

} // namespace

TEST_CASE("cloud off without passes is disabled from the start", "[meeting][end_run]") {
    const Json run = run_scenario(Json::object());
    const Json updates = finalizations(run);
    REQUIRE(updates.size() == 2);
    CHECK(updates[0].at("status") == "disabled");
    CHECK(updates[0].at("message") == "AI insights are off for this meeting.");
    CHECK(updates[0].at("emit") == false);
    CHECK_FALSE(updates[0].contains("steps"));
    CHECK(updates[1].at("status") == "disabled");
    CHECK(updates[1].at("emit") == true);
    // Agent writes are revoked even though no cloud pass ran.
    CHECK(run.at("events")[1] == Json::array({"revoke"}));
}

TEST_CASE("cloud off replaces transcript when redecode enabled", "[meeting][end_run]") {
    // The shape of
    // tests/test_meeting_engine.py::test_cloud_off_replaces_transcript_when_redecode_enabled: after
    // a completed re-decode, cloud off still ends `disabled` and passes no steps.
    const Json run = run_scenario({{"will_offline", true}, {"redecode", {{"result", true}}}});
    const Json updates = finalizations(run);
    CHECK(statuses(updates) ==
          std::vector<std::string>{"running", "running", "running", "disabled"});
    CHECK(updates[0].at("emit") == false);
    CHECK(updates[0].at("stage") == "redecode");
    CHECK(updates[0].at("step_details") == "High-accuracy full session Parakeet decode");
    CHECK(updates[1].at("message") == "Re-transcribing meeting…");
    CHECK(updates[2].at("step_details") == "High-accuracy re-decoding complete");
    CHECK(step_statuses(updates[2]) == Json{{"redecode", "completed"}, {"finalize", "pending"}});
    CHECK_FALSE(updates[3].contains("steps"));
    CHECK(updates[3].at("current_step") == 0);
    CHECK(run.at("state").at("offline_ok") == true);
}

TEST_CASE("a failed redecode keeps the live transcript", "[meeting][end_run]") {
    Json run = run_scenario({{"will_offline", true}, {"redecode", {{"result", "raise"}}}});
    Json updates = finalizations(run);
    CHECK(updates[2].at("step_details") == "Re-decoding failed; kept live transcript");
    CHECK(step_statuses(updates[2]).at("redecode") == "failed");

    run = run_scenario({{"will_offline", true},
                        {"redecode", {{"result", false}}},
                        {"redecode_failure", "Kept the live transcript."}});
    updates = finalizations(run);
    CHECK(updates[2].at("step_details") == "Kept the live transcript.");
}

TEST_CASE("redecode progress reports each window", "[meeting][end_run]") {
    const Json run = run_scenario(
        {{"will_offline", true},
         {"redecode", {{"result", true}, {"progress", {{"w1", 1, 3}, {"w2", 2, 3}}}}}});
    const Json updates = finalizations(run);
    CHECK(updates[2].at("message") == "Re-transcribing meeting (window 1/3)…");
    CHECK(updates[2].at("step_details") == "w1");
    CHECK(updates[3].at("message") == "Re-transcribing meeting (window 2/3)…");
}

TEST_CASE("an incomplete meeting with redecode marks finalize and finishes unavailable",
          "[meeting][end_run]") {
    const Json run = run_scenario({{"cloud_enabled", true},
                                   {"complete", false},
                                   {"unfinished", 2},
                                   {"scheduler", true},
                                   {"will_offline", true}});
    const Json updates = finalizations(run);
    const Json& saved = updates[updates.size() - 2];
    CHECK(saved.at("status") == "running");
    CHECK(saved.at("emit") == false);
    CHECK(saved.at("stage") == "finalize");
    CHECK(saved.at("step_details") == "Saved meeting state after re-transcription.");
    const Json& last = updates.back();
    CHECK(last.at("status") == "unavailable");
    CHECK(last.at("stage") == "unavailable");
    CHECK(last.at("message") == std::string(kRecoveryPendingMessage));
    CHECK(last.at("current_step") == 2);
    CHECK(last.at("total_steps") == 2);
    CHECK(step_statuses(last) == Json{{"redecode", "completed"}, {"finalize", "completed"}});
}

TEST_CASE("an incomplete meeting without redecode waits for recovery", "[meeting][end_run]") {
    const Json run =
        run_scenario({{"cloud_enabled", true}, {"complete", false}, {"scheduler", true}});
    const Json updates = finalizations(run);
    CHECK(statuses(updates) == std::vector<std::string>{"unavailable", "unavailable"});
    CHECK(updates[0].at("message") == std::string(kRecoveryPendingMessage));
    CHECK_FALSE(updates[1].contains("steps"));
}

TEST_CASE("offline intelligence reports why", "[meeting][end_run]") {
    const Json run = run_scenario(
        {{"cloud_enabled", true}, {"scheduler", false}, {"offline_message", "No agent."}});
    const Json updates = finalizations(run);
    CHECK(statuses(updates) == std::vector<std::string>{"unavailable", "unavailable"});
    CHECK(updates[1].at("message") == "No agent.");
    CHECK(run.at("state").at("run_cloud") == false);
}

TEST_CASE("cleanup and report off completes without passes", "[meeting][end_run]") {
    const Json run = run_scenario({{"cloud_enabled", true},
                                   {"scheduler", true},
                                   {"want_polish", false},
                                   {"want_report", false}});
    const Json updates = finalizations(run);
    CHECK(updates.back().at("status") == "completed");
    CHECK(updates.back().at("message") == "Post-meeting cleanup and report are off.");
}

TEST_CASE("the cloud passes end with the report headline", "[meeting][end_run]") {
    const Json run = run_scenario({{"cloud_enabled", true},
                                   {"scheduler", true},
                                   {"summary_stats", kStats},
                                   {"polish", {{"result", {{"status", "completed"}}}}},
                                   {"consolidate", {{"result", {{"status", "completed"}}}}}});
    const Json updates = finalizations(run);
    const Json& last = updates.back();
    CHECK(last.at("status") == "completed");
    CHECK(last.at("stage") == "complete");
    CHECK(last.at("message") == "Final insights ready — 32 segments, 4 key points, 1 decisions.");
    CHECK(last.at("summary_stats") == kStats);
    CHECK(step_statuses(last) ==
          Json{{"polish", "completed"}, {"consolidation", "completed"}, {"finalize", "completed"}});
    // Writes are allowed for the passes and revoked after them.
    const Json& events = run.at("events");
    std::vector<std::string> gates;
    for (const auto& event : events) {
        if (event.at(0) != "set_finalization") {
            gates.push_back(event.at(0).get<std::string>());
        }
    }
    CHECK(gates == std::vector<std::string>{"allow", "revoke"});
}

TEST_CASE("a failed polish fails the run with the step's reason", "[meeting][end_run]") {
    const Json run =
        run_scenario({{"cloud_enabled", true},
                      {"scheduler", true},
                      {"polish", {{"result", {{"status", "failed"}, {"message", "Timed out."}}}}},
                      {"consolidate", {{"result", {{"status", "completed"}}}}}});
    const Json updates = finalizations(run);
    const Json& last = updates.back();
    CHECK(last.at("status") == "failed");
    CHECK(last.at("stage") == "failed");
    CHECK(last.at("message") == "Transcript Cleanup failed. The recording and transcript were "
                                "kept. Transcript Cleanup: Timed out.");
}

TEST_CASE("a raising consolidation is final insights failed", "[meeting][end_run]") {
    const Json run = run_scenario({{"cloud_enabled", true},
                                   {"scheduler", true},
                                   {"polish", {{"result", {{"status", "completed"}}}}},
                                   {"consolidate", {{"result", "raise"}}}});
    const Json updates = finalizations(run);
    const Json& last = updates.back();
    CHECK(last.at("status") == "failed");
    CHECK(last.at("message") == "Final insights failed: consolidate failed");
    CHECK(last.at("step_details") == "Final insights failed: consolidate failed");
    CHECK(run.at("events").back() == Json::array({"set_finalization", last}));
}

TEST_CASE("a scheduler without polish counts cleanup as finished", "[meeting][end_run]") {
    const Json run = run_scenario({{"cloud_enabled", true}, {"scheduler", true}});
    const Json updates = finalizations(run);
    bool saw = false;
    for (const auto& update : updates) {
        if (update.at("stage") == "polish" &&
            update.at("step_details") == "Transcript cleanup finished") {
            saw = true;
        }
    }
    CHECK(saw);
}

TEST_CASE("speaker identification ends the run when cloud is off", "[meeting][end_run]") {
    const Json run = run_scenario(
        {{"will_speaker_id", true},
         {"speakers", {{"result", {{"ok", true}, {"applied", 3}}}, {"on_start", true}}}});
    const Json updates = finalizations(run);
    CHECK(updates[1].at("step_details") == "Uploading system audio for speaker labels…");
    CHECK(updates[2].at("step_details") == "Updated 3 speaker labels");
    const Json& last = updates.back();
    CHECK(last.at("status") == "completed");
    CHECK(last.at("message") ==
          "Speaker identification finished. AI insights are off for this meeting.");
    CHECK(step_statuses(last) == Json{{"speaker_id", "completed"}, {"finalize", "completed"}});
}

TEST_CASE("a skipped speaker pass is not a failure", "[meeting][end_run]") {
    const Json run = run_scenario(
        {{"will_speaker_id", true},
         {"speakers",
          {{"result",
            {{"ok", false}, {"skipped", true}, {"error", std::string(kNoConsentReason)}}}}}});
    const Json updates = finalizations(run);
    const Json& last = updates.back();
    CHECK(last.at("status") == "completed");
    CHECK(last.at("message") == std::string(kNoConsentReason));
}

TEST_CASE("a raising speaker pass fails the run", "[meeting][end_run]") {
    const Json run = run_scenario({{"will_speaker_id", true}, {"speakers", {{"result", "raise"}}}});
    const Json updates = finalizations(run);
    const Json& last = updates.back();
    CHECK(last.at("status") == "failed");
    CHECK(last.at("message") == "Speaker identification failed.");
    CHECK(run.at("state").at("speaker_error") == "Speaker identification failed.");
}

TEST_CASE("an unknown step id updates step 1 with the default headline", "[meeting][end_run]") {
    std::vector<FinalizationUpdate> updates;
    EndRun::Effects effects;
    effects.set_finalization = [&](const FinalizationUpdate& update) {
        updates.push_back(update);
        return true;
    };
    effects.core_is_healthy = [] {
        return true;
    };
    EndRun end(std::move(effects), {.meeting_id = "m", .complete = true, .will_offline = true});
    end.plan(false, false, false);
    end.update_step("nope", "running", "detail");
    end.update_step("finalize", "running");
    REQUIRE(updates.size() == 2);
    CHECK(updates[0].current_step == 1);
    CHECK(updates[0].message == "Finalizing meeting state…");
    CHECK(updates[1].current_step == 2);
    // finalize has no running message of its own.
    CHECK(updates[1].message == "Finalizing meeting state…");
    CHECK(end.steps()[1].at("detail") == std::string(*step_detail("finalize")));
}
