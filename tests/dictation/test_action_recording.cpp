#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include "automation_support.h"
#include "base/identity.h"
#include "dictation/automation_port.h"
#include "dictation/messages.h"

// The parts of services/runtime/transcription.py this package ports that OpenWhisper's automation
// tests reach only through the controller: an Action recording's start (_recording_format,
// _action_for_recording, _show_listening, _recording_destination), its History source and Stats
// target (_action_source_name, _entry_stats_counts), and run_action_text's refusal messages,
// which the live API and MCP show as they are.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

Json agent_action(bool enabled = true) {
    return action_json("ag", "Ask my agent", "agent",
                       Json{{"agent", "claude_code"}, {"mode", "ask"}}, "cleanup", enabled);
}

StartOptions action_start(std::string action_id) {
    StartOptions options;
    options.mode = JobMode::Action;
    options.source = "hotkey";
    options.action_id = std::move(action_id);
    return options;
}

bool recorder_started(const AutomationRig& rig) {
    return rig.trace.has("recorder_start");
}

} // namespace

TEST_CASE("an action recording says what it listens for and which agent gets it",
          "[action_recording]") {
    AutomationRig rig;
    rig.with_actions({agent_action()});
    std::vector<std::string> destinations;
    const auto link = rig.runtime->recording_destination_changed.connect(
        rig.ui, [&](std::string destination) { destinations.push_back(std::move(destination)); });

    REQUIRE(rig.runtime->start_recording(action_start("ag")));

    CHECK(rig.last_status() == "Ask my agent \xC2\xB7 Listening...");
    CHECK(rig.overlays().back() == "recording");
    CHECK(destinations == std::vector<std::string>{"Agent \xC2\xB7 Claude Code"});
    const auto job = rig.runtime->current_job();
    REQUIRE(job);
    CHECK(job->mode == JobMode::Action);
    CHECK(job->action_id == "ag");
    CHECK(job->source == "hotkey");
}

TEST_CASE("an action recording's words go to the action, named in History and Stats",
          "[action_recording]") {
    AutomationRig rig;
    rig.with_actions({agent_action()});
    REQUIRE(rig.runtime->start_recording(action_start("ag")));
    rig.runtime->stop_recording();
    rig.run_workers();

    CHECK(rig.pastes() == 0);
    const auto delivery = rig.port.deliveries();
    REQUIRE(delivery.size() == 1);
    CHECK(delivery[0].job->action_id == "ag");
    CHECK(delivery[0].delivery.text == "ASK CLAUDE WHAT TIME IT IS");
    const auto entries = rig.entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].source_name == "Action \xC2\xB7 Ask my agent");
    CHECK(entries[0].entry_kind == "action");
    const auto rows = rig.stats->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entry_kind == "action");
    CHECK(rows[0].target == "Ask my agent");
}

TEST_CASE("a dictation counts no target in Stats", "[action_recording]") {
    AutomationRig rig;
    REQUIRE(rig.runtime->start_recording());
    rig.runtime->stop_recording();
    rig.run_workers();

    const auto rows = rig.stats->rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].entry_kind == "dictation");
    CHECK_FALSE(rows[0].target.has_value());
}

TEST_CASE("an action recording is refused for an action that is gone or turned off",
          "[action_recording]") {
    const std::string id = GENERATE(as<std::string>{}, "", "gone", "ag");
    CAPTURE(id);
    AutomationRig rig;
    rig.with_actions({agent_action(false)});

    CHECK_FALSE(rig.runtime->start_recording(action_start(id)));

    CHECK(rig.last_status() == "That action no longer exists");
    CHECK_FALSE(recorder_started(rig));
    CHECK_FALSE(rig.runtime->has_active_job());
}

TEST_CASE("an action recording takes its action's cleanup profile", "[action_recording]") {
    AutomationRig rig;
    rig.with_actions(
        {action_json("a4", "Formal note", "scripts", Json::object(), "profile:email")});

    // The profile was deleted after the action chose it (an empty list, so no starter profile
    // stands in for it).
    rig.settings.update(Json{{"transcript_cleanup_profiles", Json::array()}});
    CHECK_FALSE(rig.runtime->start_recording(action_start("a4")));
    CHECK(rig.last_status() == "Cleanup profile no longer exists");
    CHECK_FALSE(recorder_started(rig));

    rig.settings.update(Json{
        {"transcript_cleanup_profiles",
         Json::array({Json{
             {"id", "email"}, {"name", "Email"}, {"instructions", "Write it as an email."}}})}});
    REQUIRE(rig.runtime->start_recording(action_start("a4")));
    // An action's listening status names the action, not its profile.
    CHECK(rig.last_status() == "Formal note \xC2\xB7 Listening...");
}

TEST_CASE("run_action_text's refusals carry the messages a program is shown",
          "[action_recording]") {
    AutomationRig rig;
    const std::string product(fv::identity().product);
    const auto refusal = [&](std::string_view id, std::string_view text) {
        try {
            rig.runtime->run_action_text(id, text, "api");
        } catch (const RunRefused& refused) {
            return std::pair<std::string, std::string>{refused.code(), refused.message()};
        }
        return std::pair<std::string, std::string>{"ran", ""};
    };
    using Refusal = std::pair<std::string, std::string>;

    CHECK(refusal("a1", " \n ") == Refusal{"refused", "There are no words to send."});
    CHECK(refusal("gone", "hi") == Refusal{"not_found", "No enabled action has that id."});
    rig.with_actions({agent_action(false)});
    CHECK(refusal("ag", "hi") == Refusal{"not_found", "No enabled action has that id."});

    rig.activity.set_meeting_active(true);
    CHECK(refusal("a1", "hi") == Refusal{"busy", "Meeting Mode is active; end the meeting first."});
    // No words is said first, even during a meeting.
    CHECK(refusal("a1", "") == Refusal{"refused", "There are no words to send."});
    rig.activity.set_meeting_active(false);

    rig.port.taking = false;
    CHECK(refusal("a1", "hi") == Refusal{"busy", product + " isn't taking actions right now."});
    rig.port.taking = true;

    REQUIRE(rig.runtime->claim_job(make_job(JobMode::Dictation)));
    CHECK(refusal("a1", "hi") == Refusal{"busy", product + " is busy with another recording."});
    CHECK(rig.port.deliveries().empty());
}

TEST_CASE("without an automation port nothing is routed and no action records",
          "[action_recording]") {
    AutomationRig rig;
    rig.runtime->set_automation(null_automation_port());

    CHECK_FALSE(rig.runtime->start_recording(action_start("a1")));
    CHECK(rig.last_status() == "That action no longer exists");
    try {
        rig.runtime->run_action_text("a1", "hi", "api");
        FAIL("the run was not refused");
    } catch (const RunRefused& refused) {
        CHECK(refused.code() == RUN_NOT_FOUND);
    }

    // A dictation keeps M1's path: cleaned and pasted.
    rig.transcribe(make_job(JobMode::Dictation));
    rig.run_workers();
    CHECK(rig.stages() == std::vector<std::string>{"ASK CLAUDE WHAT TIME IT IS"});
    CHECK(rig.pastes() == 1);
}
