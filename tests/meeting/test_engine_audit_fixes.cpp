// Engine behaviours of meeting/engine.py the a24 audit found missing or different (A24-59,
// A24-68, A24-18, A24-16), ported and pinned here.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "engine_support.h"
#include "meeting/engine_options.h"
#include "meeting/testing/fake_intelligence.h"
#include "meeting_web/dashboard_engine.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::eventually;

namespace {

using std::chrono::seconds;

EngineSetup cloud_setup() {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = true;
    return setup;
}

// A provider whose live meetings come online on `core`.
class LiveProvider final : public fv::meeting::test::OnlineProvider {
public:
    std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
        std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
    std::shared_ptr<fv::meeting_agent::AgentCore>
    create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return core;
    }
    Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
    Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
        return {{"status", "completed"}};
    }
};

} // namespace

TEST_CASE("a failed End shuts the live agent down", "[test_meeting_engine]") {
    // engine.py _finish_failed_end calls _shutdown_agent_core after revoking writes (A24-59):
    // an End that fails before its drain must not leave the live agent running.
    EngineRig rig;
    auto setup = cloud_setup();
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    probe.before_drain([] { throw std::runtime_error("capture stop exploded"); });
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == true);
    REQUIRE(probe.live_intelligence_attached());

    EngineRig::end_and_wait(engine);

    CHECK(rig.events_of("error").back().at("message") == "capture stop exploded");
    CHECK(rig.events_of("ended").back().at("error") == "capture stop exploded");
    CHECK_FALSE(probe.live_intelligence_attached());
    CHECK(provider->core->shutdown_calls() == 1);
}

TEST_CASE("AI insights change only while the meeting is active or paused",
          "[test_meeting_engine]") {
    // engine.py set_cloud_enabled (A24-68): refused once End claimed the meeting, written as a
    // host set_cloud_enabled op (an audit event), and the live card says insights are off.
    EngineRig rig;
    auto setup = cloud_setup();
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    setup.options.end_polish = false;
    setup.options.end_report = false;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("cloud_enabled") == true);

    engine.pause();
    const auto seq = engine.store()->snapshot().at("seq").get<std::int64_t>();
    engine.set_cloud_enabled(false);
    const Json state = engine.store()->snapshot();
    CHECK(state.at("cloud_enabled") == false);
    CHECK(state.at("intelligence_online") == false);
    CHECK(state.at("finalization").at("status") == "disabled");
    CHECK(state.at("finalization").at("message") == "AI insights are off for this meeting.");
    // The op advanced the document's sequence and reached the dashboard as a patch.
    CHECK(state.at("seq").get<std::int64_t>() > seq);
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("cloud_enabled") == false);
    CHECK(provider->core->shutdown_calls() == 1);
    engine.resume();

    EngineRig::end_and_wait(engine);
    try {
        engine.set_cloud_enabled(false);
        FAIL("AI insights were changed after End");
    } catch (const fv::meeting_web::EngineRefusal& refusal) {
        CHECK(std::string(refusal.what()) ==
              "AI insights can only be changed while the meeting is active or paused.");
    }
    try {
        engine.enable_live_intelligence(
            fv::meeting_agent::MeetingAgentRoute{"pi", "fake", "scripted", Json()});
        FAIL("AI insights were turned on after End");
    } catch (const fv::meeting_web::EngineRefusal& refusal) {
        const std::string text = refusal.what();
        CHECK((text == "AI insights can only be changed while the meeting is active or paused." ||
               text == "Approve the current AI insights destination first."));
    }
}

TEST_CASE("agent activity publishes one tick per kind and tool a second", "[test_meeting_engine]") {
    // engine.py _on_agent_activity (A24-18): AGENT_ACTIVITY_MIN_INTERVAL_S = 1.0 on the monotonic
    // clock per (kind, tool); an empty kind reads "update" and an empty timestamp is now.
    fv::FakeClock clock;
    EngineRig rig;
    auto setup = cloud_setup();
    setup.clock = &clock;
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == true);

    provider->core->emit_activity({"thinking", "First", "", "cards", "2026-01-01T00:00:00Z"});
    provider->core->emit_activity({"thinking", "Too soon", "", "cards", "2026-01-01T00:00:00Z"});
    provider->core->emit_activity({"tool", "Other tool", "read_transcript", "cards", "x"});
    clock.advance(std::chrono::milliseconds(999));
    provider->core->emit_activity({"thinking", "Still too soon", "", "cards", "x"});
    clock.advance(std::chrono::milliseconds(1));
    provider->core->emit_activity({"thinking", "A second later", "", "cards", "x"});
    provider->core->emit_activity({"", "No kind", "", "", ""});

    const auto ticks = engine.recent_agent_activity();
    std::vector<std::string> labels;
    for (const auto& tick : ticks) {
        labels.push_back(tick.at("label").get<std::string>());
    }
    CHECK(labels == std::vector<std::string>{"First", "Other tool", "A second later", "No kind"});
    CHECK(ticks.back().at("kind") == "update");
    CHECK_FALSE(ticks.back().at("ts").get<std::string>().empty());
    CHECK(rig.server(0)->messages_of("agent_activity").size() == 4);
}

TEST_CASE("a backup agent is recorded on the meeting before it is tried", "[test_meeting_engine]") {
    // engine.py _switch_to_backup_agent writes the backup to the meeting row before starting it
    // ("Re-runs and reports stay with the agent that wrote the record", A24-16), so a backup that
    // cannot start is still the recorded agent.
    class FailingBackup final : public fv::meeting::IntelligenceProvider {
    public:
        int starts = 0;
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            ++starts;
            throw std::runtime_error(starts == 1 ? "The primary agent could not initialize."
                                                 : "The backup agent could not initialize.");
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    const fv::meeting_agent::MeetingAgentRoute primary{"pi", "fake", "scripted", Json()};
    const fv::meeting_agent::MeetingAgentRoute backup{"codex", "codex", "fake-backup", Json()};
    EngineRig rig;
    auto setup = cloud_setup();
    auto provider = std::make_shared<FailingBackup>();
    setup.intelligence = provider;
    setup.options.llm_provider = primary.provider;
    setup.options.llm_model = primary.model;
    setup.options.agent_approval = fv::meeting::LiveIntelligenceApproval{primary, backup};
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(provider->starts == 2);
    CHECK(engine.store()->snapshot().at("intelligence_online") == false);
    const auto recorded = rig.sql.repo->get_meeting(engine.meeting_id());
    REQUIRE(recorded);
    CHECK(recorded->at("agent_provider") == backup.provider);
    CHECK(recorded->at("agent_model") == backup.model);
    CHECK(recorded->at("agent_endpoint_json").is_null());
}

// from tests/test_meeting_corrections.py::TestEngineHook::
// test_user_note_ops_wake_the_agent_and_other_cards_do_not (the guidance half; the term rules
// prime Whisper's draft prompt, which Parakeet does not take)
TEST_CASE("test_user_note_ops_wake_the_agent_and_other_cards_do_not", "[test_meeting_engine]") {
    EngineRig rig;
    auto setup = cloud_setup();
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(probe.live_intelligence_attached());
    const auto added =
        engine.apply_client_action("host", std::nullopt,
                                   Json{{"op", "add_item"},
                                        {"card", "user_notes"},
                                        {"text", "Correction: Entropic -> Anthropic"},
                                        {"data",
                                         {{"kind", "term_correction"},
                                          {"selected_text", "Entropic"},
                                          {"replacement", "Anthropic"}}}});
    REQUIRE(added.size() == 1);
    REQUIRE(added[0].ok);
    CHECK(probe.guidance_notices() == 1);

    const auto other = engine.apply_client_action(
        "host", std::nullopt,
        Json{{"op", "add_item"}, {"card", "key_points"}, {"text", "Unrelated"}});
    REQUIRE(other.size() == 1);
    CHECK(other[0].ok);
    CHECK(probe.guidance_notices() == 1);

    REQUIRE(added[0].seq);
    engine.undo(*added[0].seq, std::nullopt);
    CHECK(probe.guidance_notices() == 2);
}

// from tests/test_meeting_intent.py::TestIntentWakesTheAgent
TEST_CASE("TestIntentWakesTheAgent", "[test_meeting_intent]") {
    EngineRig rig;
    auto setup = cloud_setup();
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(probe.live_intelligence_attached());
    SECTION("test_a_rewritten_brief_triggers_a_guidance_pass") {
        probe.notify_intelligence_guidance({fv::meeting_state::OpResult{
            .ok = true,
            .op = Json{{"op", "set_meeting_intent"}},
            .effect = Json{{"entity", "intent"},
                           {"intent", {{"text", "Decide the vendor and the budget."}}}}}});
        CHECK(probe.guidance_notices() == 1);
    }
    SECTION("test_an_unrelated_op_does_not_trigger_one") {
        probe.notify_intelligence_guidance({fv::meeting_state::OpResult{
            .ok = true,
            .op = Json{{"op", "set_title"}},
            .effect = Json{{"entity", "title"}, {"text", "Vendor review"}}}});
        CHECK(probe.guidance_notices() == 0);
    }
}

TEST_CASE("review_config is insight_review.py's", "[test_meeting_insight_review]") {
    // review_config(enabled, sensitivity, consent, cloud_enabled): enabled only with the
    // current consent; pending, or unavailable with AI insights off; disabled otherwise.
    const std::string consent = "typesafe-text-v1";
    CHECK(fv::meeting::review_config(
              Json{{"enabled", true}, {"consent", consent}, {"sensitivity", "thorough"}}, true) ==
          Json{{"enabled", true},
               {"consent", consent},
               {"sensitivity", "thorough"},
               {"status", "pending"},
               {"questions", Json::array()},
               {"message", ""}});
    CHECK(fv::meeting::review_config(Json{{"enabled", true}, {"consent", consent}}, false) ==
          Json{{"enabled", true},
               {"consent", consent},
               {"sensitivity", "normal"},
               {"status", "unavailable"},
               {"questions", Json::array()},
               {"message", "AI insights are off for this meeting."}});
    CHECK(fv::meeting::review_config(Json{{"enabled", true}, {"consent", "old-v0"}}, true) ==
          Json{{"enabled", false},
               {"consent", "old-v0"},
               {"sensitivity", "normal"},
               {"status", "disabled"},
               {"questions", Json::array()},
               {"message", ""}});
    CHECK(fv::meeting::review_config(Json::object(), true).at("consent") == "");
}
