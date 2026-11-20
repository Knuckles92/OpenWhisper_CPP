// tests/test_meeting_runtime.py: the consent round trip of a start with AI insights, the brief
// across it, the AI insights toggle of a running meeting, and what the card says about a speaker
// re-run.
//
// OpenWhisper stores one consent and asks only while it is missing. OpenWhisper_CPP asks before
// every meeting whose text would leave for a hosted model or an installed agent, and the answer
// names the destination: the gate (set_consent_gate) is consulted on each start with AI insights,
// and a running meeting's insights turn on only with an approval in hand.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "meeting/runtime_messages.h"
#include "meeting_state/schema.h"
#include "runtime_support.h"
#include "settings/keys.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;
using fv::meeting_agent::MeetingAgentRoute;

namespace {

// The consent gate as the Meeting tab plugs it in: asked with the cloud choice on the UI actor,
// it takes the start over and answers later (OpenWhisper's meeting_consent_requested signal and
// on_consent_result).
struct Gate {
    explicit Gate(MeetingRuntime& runtime) {
        runtime.set_consent_gate([this](bool cloud, std::function<void(bool)> reply) {
            asked.push_back(cloud);
            if (!takes_over) {
                return false;
            }
            answer = std::move(reply);
            return true;
        });
    }
    bool pending() const { return static_cast<bool>(answer); }
    // The user's answer, given on the UI actor.
    void reply(RuntimeRig& rig, bool granted) {
        REQUIRE(pending());
        auto give = std::exchange(answer, nullptr);
        on_ui(rig.ui, [&] { give(granted); });
        rig.drain();
    }

    std::vector<bool> asked;
    bool takes_over = true;
    std::function<void(bool)> answer;
};

std::size_t starts_spawned(RuntimeRig& rig) {
    const auto names = rig.spawned_names();
    return static_cast<std::size_t>(std::ranges::count(names, std::string("meeting-start")));
}

// Ends the rig's running meeting as the engine reports it.
void end_meeting(RuntimeRig& rig, std::size_t index) {
    rig.engines.at(index)->active = false;
    rig.engines.at(index)->emit("ended", Json{{"status", "ended"}});
    rig.drain();
}

// A started meeting whose engine has a state store and somewhere its insights could go.
struct Running {
    Running() {
        rig.make_engine = [this](const MeetingEngineOptions&) {
            auto engine = std::make_shared<FakeEngine>("m_live");
            fv::meeting_state::MeetingState state;
            state.meeting_id = "m_live";
            state.title = "Budget sync";
            engine->set_store(
                std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state)));
            engine->set_destination(route);
            return engine;
        };
        rig.rt->start_meeting(false);
        rig.drain();
        REQUIRE(rig.engines.size() == 1);
        engine = rig.engines[0];
        rig.got = Recorded{};
    }

    const MeetingAgentRoute route{"pi", "openrouter", "test/model", Json{{"profile_id", "x"}}};
    std::vector<std::string> prompts;
    bool approve = false;
    RuntimeRig rig{[this](MeetingRuntimeDeps& deps) {
        deps.confirm_browser_insights = [this](const std::string& title,
                                               const MeetingAgentRoute& asked) {
            prompts.push_back(title + " -> " + asked.provider + "/" + asked.model);
            return approve;
        };
    }};
    std::shared_ptr<FakeEngine> engine;
};

} // namespace

// from tests/test_meeting_runtime.py::test_cloud_start_without_consent_emits_and_does_not_launch
TEST_CASE("test_cloud_start_without_consent_emits_and_does_not_launch", "[test_meeting_runtime]") {
    RuntimeRig rig;
    Gate gate(*rig.rt);

    rig.rt->start_meeting(true);
    rig.drain();

    CHECK(gate.asked == std::vector<bool>{true});
    CHECK(gate.pending());
    CHECK(rig.built.empty());
    CHECK(starts_spawned(rig) == 0);
    CHECK(rig.rt->is_claimed());
    CHECK_FALSE(rig.probe().controller_active());
    // Nothing was announced: no "starting" state, no status line.
    CHECK(rig.got.states.empty());
    CHECK(rig.got.statuses.empty());
}

// from tests/test_meeting_runtime.py::test_brief_survives_the_consent_round_trip
TEST_CASE("test_brief_survives_the_consent_round_trip", "[test_meeting_runtime]") {
    // The consent dialog suspends the start; the typed brief (and the system audio choice made
    // in setup) must outlast it.
    RuntimeRig rig;
    Gate gate(*rig.rt);
    const std::string brief = "Flag anything about the Q3 budget.";

    rig.rt->start_meeting(true, "disabled", brief);
    rig.drain();
    gate.reply(rig, true);

    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].intent == brief);
    CHECK(rig.built[0].system_audio_policy == "disabled");
    CHECK(rig.built[0].cloud_enabled);
}

// from tests/test_meeting_runtime.py::test_a_later_meeting_does_not_inherit_the_previous_brief
TEST_CASE("test_a_later_meeting_does_not_inherit_the_previous_brief", "[test_meeting_runtime]") {
    // Each start rewrites the pending brief, including back to empty.
    RuntimeRig rig;
    rig.rt->start_meeting(false, "auto", "Yesterday's brief.");
    rig.drain();
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].intent == "Yesterday's brief.");
    end_meeting(rig, 0);

    rig.rt->start_meeting(false);
    rig.drain();

    REQUIRE(rig.built.size() == 2);
    CHECK(rig.built[1].intent.empty());
}

// from tests/test_meeting_runtime.py::test_demo_start_without_consent_uses_start_demo_kind
TEST_CASE("test_demo_start_without_consent_uses_start_demo_kind", "[test_meeting_runtime]") {
    // Python remembers the pending kind ("start_demo"); here the answer's continuation carries
    // it, so the kind shows in what the answer launches.
    RuntimeRig rig;
    Gate gate(*rig.rt);

    rig.rt->start_demo_meeting(true);
    rig.drain();

    CHECK(gate.asked == std::vector<bool>{true});
    CHECK(rig.rt->is_claimed());
    CHECK(rig.built.empty());

    gate.reply(rig, true);
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].demo_mode);
    CHECK(rig.got.statuses.front() == messages::kStartingDemo);
}

// from
// tests/test_meeting_runtime.py::test_consent_result_launches_with_the_answered_cloud_choice
TEST_CASE("test_consent_result_launches_with_the_answered_cloud_choice", "[test_meeting_runtime]") {
    struct Case {
        const char* id;
        bool demo;
        bool granted;
    };
    const Case c = GENERATE(Case{"declined-starts-transcript-only", false, false},
                            Case{"granted", false, true}, Case{"granted-demo", true, true});
    CAPTURE(c.id);
    RuntimeRig rig;
    Gate gate(*rig.rt);
    if (c.demo) {
        rig.rt->start_demo_meeting(true);
    } else {
        rig.rt->start_meeting(true);
    }
    rig.drain();
    REQUIRE(gate.pending());

    gate.reply(rig, c.granted);

    // A declined consent still starts the meeting, transcript-only.
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].cloud_enabled == c.granted);
    CHECK(rig.built[0].demo_mode == c.demo);
    CHECK(rig.built[0].system_audio_policy == "auto");
    CHECK(starts_spawned(rig) == 1);
    CHECK_FALSE(gate.pending());
    // The answered choice is the one remembered for the next start.
    CHECK(fv::keys::meeting_cloud_last_enabled.read(*rig.settings.snapshot()) == c.granted);
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.front().at("status") == "starting");
    CHECK(rig.got.states.front().at("cloud_enabled") == c.granted);
    CHECK(rig.probe().controller_active());
}

// from tests/test_meeting_runtime.py::test_remembered_cloud_on_prompts_when_consent_missing
TEST_CASE("test_remembered_cloud_on_prompts_when_consent_missing", "[test_meeting_runtime]") {
    RuntimeRig rig;
    Gate gate(*rig.rt);
    rig.settings.update(Json{{"meeting_cloud_last_enabled", true}});

    rig.rt->start_meeting();
    rig.drain();

    CHECK(gate.asked == std::vector<bool>{true});
    CHECK(gate.pending());
    CHECK(rig.built.empty());
    CHECK(starts_spawned(rig) == 0);
}

// from tests/test_meeting_runtime.py::test_second_start_during_consent_is_blocked
TEST_CASE("test_second_start_during_consent_is_blocked", "[test_meeting_runtime]") {
    RuntimeRig rig;
    Gate gate(*rig.rt);
    rig.rt->start_meeting(true);
    rig.drain();
    REQUIRE(gate.pending());

    rig.rt->start_meeting(false);
    rig.drain();

    CHECK(rig.built.empty());
    CHECK(rig.got.statuses == std::vector<std::string>{std::string(messages::kAlreadyInProgress)});
    // The first start's question is still the one open.
    CHECK(gate.asked == std::vector<bool>{true});
    CHECK(gate.pending());

    gate.reply(rig, true);
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].cloud_enabled);
}

TEST_CASE("every start with AI insights asks again and carries only its own approval",
          "[meeting][runtime]") {
    // OpenWhisper_CPP's rule where OpenWhisper's
    // test_start_with_consent_already_given_launches_cloud launches without a prompt: consent is
    // per meeting and destination, so an earlier grant answers nothing.
    RuntimeRig rig;
    Gate gate(*rig.rt);
    const MeetingAgentRoute route{"pi", "openrouter", "test/model", Json{{"profile_id", "x"}}};
    const MeetingAgentRoute backup{"codex", "codex", "gpt-x", Json()};
    // Outside a claimed start an approval belongs to no meeting.
    rig.rt->set_start_intelligence_approval({route, backup});

    rig.rt->start_meeting(true);
    rig.drain();
    REQUIRE(gate.pending());
    // The dialog's answer names what was approved (the Meeting tab's askIntelligenceConsent).
    rig.rt->set_start_intelligence_approval({route, backup});
    gate.reply(rig, true);
    REQUIRE(rig.built.size() == 1);
    REQUIRE(rig.built[0].agent_approval);
    CHECK(rig.built[0].agent_approval->primary == route);
    CHECK(rig.built[0].agent_approval->backup == backup);
    CHECK(rig.built[0].agent_core_kind == "pi");
    CHECK(rig.built[0].llm_provider == "openrouter");
    CHECK(rig.built[0].llm_model == "test/model");
    CHECK(rig.built[0].llm_endpoint == Json{{"profile_id", "x"}});
    end_meeting(rig, 0);

    // The remembered choice is on now, and the next meeting is asked about all the same.
    rig.rt->start_meeting();
    rig.drain();
    CHECK(gate.asked == std::vector<bool>{true, true});
    REQUIRE(gate.pending());
    CHECK(rig.built.size() == 1);
    // Declined: transcript-only, and the first meeting's approval is not carried over.
    gate.reply(rig, false);
    REQUIRE(rig.built.size() == 2);
    CHECK_FALSE(rig.built[1].cloud_enabled);
    CHECK_FALSE(rig.built[1].agent_approval);
    end_meeting(rig, 1);

    // Granted without naming a destination: the engine is handed no approval to act on.
    rig.rt->start_meeting(true);
    rig.drain();
    gate.reply(rig, true);
    REQUIRE(rig.built.size() == 3);
    CHECK(rig.built[2].cloud_enabled);
    CHECK_FALSE(rig.built[2].agent_approval);
}

// from tests/test_meeting_runtime.py::test_toggle_cloud_with_consent_applies
TEST_CASE("test_toggle_cloud_with_consent_applies", "[test_meeting_runtime]") {
    // OpenWhisper's toggle_cloud(True) with consent stored; here the tab calls the runtime with
    // the approval its dialog just collected.
    Running live;
    auto& rig = live.rig;
    const LiveIntelligenceApproval approval{live.route, std::nullopt};

    rig.rt->set_live_intelligence("m_live", approval);
    rig.drain();

    const auto requests = live.engine->live_requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0]);
    CHECK(requests[0]->primary == live.route);
    CHECK(rig.got.states == std::vector<Json>{Json{{"cloud_enabled", true}}});
    CHECK(rig.got.statuses.empty());

    // Off needs no approval, and a refusal is shown with the state the engine is left in.
    rig.got = Recorded{};
    rig.rt->set_live_intelligence("m_live", std::nullopt);
    rig.drain();
    CHECK(rig.got.states == std::vector<Json>{Json{{"cloud_enabled", false}}});
    live.engine->on_live_intelligence = [](const std::optional<LiveIntelligenceApproval>&) {
        throw std::runtime_error("Approve the current AI insights destination first.");
    };
    rig.got = Recorded{};
    rig.rt->set_live_intelligence("m_live", approval);
    rig.drain();
    // _apply_cloud's error, with the state the engine is left in.
    CHECK(rig.got.statuses.empty());
    CHECK(rig.got.errors == std::vector<std::string>{"Could not change AI insights: Approve the "
                                                     "current AI insights destination first."});
    CHECK(rig.got.states == std::vector<Json>{Json{{"cloud_enabled", false}}});

    // Another meeting's id reaches no engine.
    rig.got = Recorded{};
    rig.rt->set_live_intelligence("m_other", approval);
    rig.drain();
    CHECK(live.engine->live_requests().size() == 3);
    CHECK(rig.got.states.empty());
}

// from tests/test_meeting_runtime.py::test_toggle_cloud_without_consent_reprompts
TEST_CASE("test_toggle_cloud_without_consent_reprompts", "[test_meeting_runtime]") {
    // OpenWhisper's toggle_cloud(True) re-prompts when no consent is stored and changes nothing
    // meanwhile. Here no consent is ever stored: the Meeting tab asks before it calls the
    // runtime (ui_meeting's presenter), and the dashboard's switch asks the desktop through the
    // runtime, which is the round trip under test.
    Running live;
    auto& rig = live.rig;
    std::vector<std::optional<std::string>> outcomes;
    const auto finish = [&outcomes](std::optional<std::string> refusal) {
        outcomes.push_back(std::move(refusal));
    };

    rig.probe().request_browser_insights("m_live", fv::CancelToken(), finish);
    rig.drain();

    CHECK(live.prompts == std::vector<std::string>{"Budget sync -> openrouter/test/model"});
    REQUIRE(outcomes.size() == 1);
    CHECK(outcomes[0] == "AI insights were not approved on the desktop.");
    CHECK(live.engine->live_requests().empty());
    CHECK(rig.got.states.empty());
    CHECK(live.engine->store()->snapshot().at("cloud_enabled") == false);

    // Approved: the engine is handed exactly the destination the prompt named.
    live.approve = true;
    rig.probe().request_browser_insights("m_live", fv::CancelToken(), finish);
    rig.drain();
    CHECK(live.prompts.size() == 2);
    REQUIRE(outcomes.size() == 2);
    CHECK_FALSE(outcomes[1]);
    const auto requests = live.engine->live_requests();
    REQUIRE(requests.size() == 1);
    REQUIRE(requests[0]);
    CHECK(requests[0]->primary == live.route);
    CHECK_FALSE(requests[0]->backup);
    CHECK(rig.got.states == std::vector<Json>{Json{{"cloud_enabled", true}}});

    // Already on: nothing to ask. A request for a meeting that is not the live one, or whose
    // permit was withdrawn, is refused without a prompt.
    rig.probe().request_browser_insights("m_live", fv::CancelToken(), finish);
    rig.probe().request_browser_insights("m_other", fv::CancelToken(), finish);
    fv::CancelSource withdrawn;
    withdrawn.cancel();
    rig.probe().request_browser_insights("m_live", withdrawn.token(), finish);
    rig.drain();
    CHECK(live.prompts.size() == 2);
    REQUIRE(outcomes.size() == 5);
    CHECK_FALSE(outcomes[2]);
    CHECK(outcomes[3] == "This meeting is no longer active.");
    CHECK(outcomes[4] == "This meeting is no longer active.");
    CHECK(live.engine->live_requests().size() == 1);
}

TEST_CASE("the card never offers a speaker re-run from saved settings", "[meeting][runtime]") {
    // Where OpenWhisper's test_content_summary_names_what_blocks_a_speaker_rerun offers the
    // re-run and names the missing consent: OpenWhisper_CPP uploads a saved meeting's audio only
    // from the explicit Past Meetings action, approved for that meeting each time, so a saved
    // "openai" backend reads as on-device labels and no saved consent unlocks anything.
    RuntimeRig rig;
    const Json not_offered{
        {"speaker_rerun_offered", false},
        {"speaker_rerun_reason", "Speaker identification is not set to OpenAI."}};

    rig.settings.update(Json{{"meeting_speaker_id_backend", "openai"},
                             {"meeting_audio_upload_consent_given", false}});
    CHECK(rig.probe().speaker_rerun_fields() == not_offered);

    rig.settings.update(Json{{"meeting_audio_upload_consent_given", true}});
    CHECK(rig.probe().speaker_rerun_fields() == not_offered);

    // The half that carries over unchanged: on-device labels offer no re-run.
    rig.settings.update(Json{{"meeting_speaker_id_backend", "local"}});
    CHECK(rig.probe().speaker_rerun_fields().at("speaker_rerun_offered") == false);
    // The explicit action is offered only with a way to run it.
    CHECK_FALSE(rig.rt->can_offer_speaker_upload());
}
