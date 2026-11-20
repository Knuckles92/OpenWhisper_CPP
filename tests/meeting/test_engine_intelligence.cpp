// MeetingEngine with meeting intelligence over in-process fakes (tests/test_meeting_engine.py):
// the live agent's start, health, backup and write gate, and End's cleanup and report passes. The
// live cases run the app's provider (MeetingIntelligence) over a factory of scripted cores, so
// the tool host and the cancel wiring under test are the ones a meeting uses.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "engine_support.h"
#include "intelligence_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting/meeting_intelligence.h"
#include "meeting/testing/fake_intelligence.h"
#include "meeting_agent/route.h"
#include "meeting_agent/testing/fake_agent_core.h"

using fv::Json;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::Event;
using fv::meeting::test::mic_segment;
using fv::meeting::test::OwnedCore;
using fv::meeting_agent::MeetingAgentRoute;

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;
using Core = fv::meeting_agent::testing::FakeAgentCore;

constexpr std::string_view kUnusable =
    "The meeting-intelligence backend is unavailable (no API key or backend); continuing "
    "transcript-only.";
constexpr std::string_view kKeyRejected =
    "The AI provider rejected the API key (401). Check it in Settings → Meeting Mode → "
    "Intelligence.";

// Python's `fakes` for the agent: MeetingIntelligence over a factory the test scripts. Every
// destination is approved, as a meeting started with AI insights has it. Declared before the
// EngineRig, whose engines call into it until they are gone.
struct Live {
    Live()
        : route(fv::meeting_agent::resolve_meeting_agent_route(settings)),
          provider(std::make_shared<fv::meeting::MeetingIntelligence>(
              [this](std::string_view kind) { return make(kind); }, [this] { return settings; },
              [](const MeetingAgentRoute&) { return true; })) {}

    std::unique_ptr<fv::meeting_agent::AgentCore> make(std::string_view kind) {
        std::function<std::unique_ptr<fv::meeting_agent::AgentCore>(std::string_view)> scripted;
        {
            const std::lock_guard lock(mutex);
            kinds_.emplace_back(kind);
            scripted = factory;
        }
        if (scripted) {
            return scripted(kind);
        }
        return std::make_unique<OwnedCore>(kind == backup.kind ? backup_core : core);
    }
    // The core kinds the factory was asked for, in order.
    std::vector<std::string> kinds() const {
        const std::lock_guard lock(mutex);
        return kinds_;
    }

    // An opted-in meeting whose destination (and, when asked, backup agent) the host approved.
    EngineSetup setup(bool with_backup = false) const {
        EngineSetup out{.options = EngineRig::default_options()};
        out.options.cloud_enabled = true;
        out.options.agent_core_kind = route.kind;
        out.options.llm_provider = route.provider;
        out.options.llm_model = route.model;
        out.options.agent_approval = fv::meeting::LiveIntelligenceApproval{
            route, with_backup ? std::optional(backup) : std::nullopt};
        out.intelligence = provider;
        return out;
    }

    const Json settings = Json::object();
    const MeetingAgentRoute route;
    const MeetingAgentRoute backup{"codex", "codex", "gpt-x", Json()};
    std::shared_ptr<Core> core = std::make_shared<Core>();
    std::shared_ptr<Core> backup_core = std::make_shared<Core>();
    // Replaces the default (the primary `core`, or `backup_core` for the backup's kind).
    std::function<std::unique_ptr<fv::meeting_agent::AgentCore>(std::string_view)> factory;
    std::shared_ptr<fv::meeting::MeetingIntelligence> provider;

private:
    mutable std::mutex mutex;
    std::vector<std::string> kinds_;
};

EngineSetup setup_with(bool cloud_enabled) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = cloud_enabled;
    return setup;
}

Json step(const Json& finalization, std::string_view id) {
    for (const Json& entry : finalization.value("steps", Json::array())) {
        if (entry.value("id", std::string()) == id) {
            return entry;
        }
    }
    return Json();
}

std::vector<std::string> step_ids(const Json& finalization) {
    std::vector<std::string> ids;
    for (const Json& entry : finalization.value("steps", Json::array())) {
        ids.push_back(entry.value("id", std::string()));
    }
    return ids;
}

std::set<std::string> segment_ids(EngineRig& rig, const std::string& meeting_id) {
    std::set<std::string> ids;
    for (const Json& row : rig.sql.repo->get_segments(meeting_id, -1.0, std::nullopt)) {
        ids.insert(row.at("id").get<std::string>());
    }
    return ids;
}

bool any_online(const std::vector<Json>& intelligence_events) {
    return std::ranges::any_of(intelligence_events,
                               [](const Json& event) { return event.value("online", false); });
}

// A failed pass, as a provider that answers with `error`.
fv::meeting_agent::AgentResult failed(std::string error) {
    fv::meeting_agent::AgentResult result;
    result.error = std::move(error);
    return result;
}

// Moves the meeting clock past the warm-up and each retry backoff, waking the scheduler, until
// `done` holds; false when it never did.
template <class F>
bool drive(MeetingEngine& engine, fv::FakeClock& clock, F done,
           std::chrono::milliseconds timeout = seconds(5)) {
    const auto until = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < until) {
        // Longer than the longest backoff (300 s after the third consecutive failure).
        clock.advance(seconds(320));
        MeetingEngineProbe(engine).notify_intelligence_segments(1);
        std::this_thread::sleep_for(milliseconds(5));
        if (done()) {
            return true;
        }
    }
    return done();
}

bool online(MeetingEngine& engine) {
    return engine.store()->snapshot().at("intelligence_online").get<bool>();
}

Json finalization_of(MeetingEngine& engine) {
    return engine.store()->snapshot().at("finalization");
}

} // namespace

// ---- the live agent's start ----

TEST_CASE("test_end_cancels_initializing_agent_without_late_online_status",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_end_cancels_initializing_agent_without_late_online_status. Python's End shuts the
    // initializing core down, which unblocks it; here End cancels the start's token, which
    // cancels the core, and the provider shuts down the core it no longer hands over. The
    // cleanup and report passes are on, as in Python: the agent never came online, so End runs
    // neither and the card says why (engine.py _end_worker, end_run.py EndRun.plan).
    Live live;
    Event entered;
    Event release;
    live.factory = [&](std::string_view) {
        return std::make_unique<OwnedCore>(
            live.core,
            [&] {
                entered.set();
                release.wait(seconds(5));
            },
            [&] { release.set(); });
    };
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.processing_inline = false;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(entered.wait());

    engine.end();
    REQUIRE(engine.wait_for_end(seconds(5)));
    REQUIRE(engine.wait_for_processing(seconds(5)));

    CHECK(live.core->shutdown_calls() >= 1);
    CHECK(live.core->cancel_calls() >= 1);
    // Never adopted: no scheduler ran a pass on it, and End built no core for a final pass.
    CHECK(live.core->passes().empty());
    CHECK(live.kinds().size() == 1);
    CHECK_FALSE(engine.agent_writes_allowed());
    CHECK_FALSE(online(engine));
    CHECK_FALSE(any_online(rig.events_of("intelligence")));
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("status") == "ended");
    // A cancelled start leaves no failure behind, so the sentence stands alone.
    const Json finalization = finalization_of(engine);
    CHECK(finalization.at("status") == "unavailable");
    CHECK(finalization.at("message") ==
          "Meeting intelligence is offline; final cloud insights could not run.");
    CHECK(step(finalization, "polish").is_null());
    CHECK(step(finalization, "consolidation").is_null());
}

TEST_CASE("test_revoked_initializer_cannot_adopt_or_overwrite_new_consent",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDeferredProcessingStartup::
    // test_revoked_initializer_cannot_adopt_or_overwrite_new_consent. Turning AI insights back
    // on is enable_live_intelligence with a fresh approval of the destination here
    // (set_cloud_enabled(true) is refused); it does not wait for the revoked start.
    const bool initialization_raises = GENERATE(false, true);
    const bool reenable = GENERATE(false, true);
    CAPTURE(initialization_raises, reenable);
    Live live;
    Event entered;
    Event release;
    auto revoked = std::make_shared<Core>();
    auto fresh = std::make_shared<Core>();
    std::atomic<int> created{0};
    live.factory = [&](std::string_view) -> std::unique_ptr<fv::meeting_agent::AgentCore> {
        if (created++ == 0) {
            return std::make_unique<OwnedCore>(revoked, [&] {
                entered.set();
                release.wait(seconds(5));
                if (initialization_raises) {
                    throw std::runtime_error("initialization interrupted");
                }
            });
        }
        return std::make_unique<OwnedCore>(fresh);
    };
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.processing_inline = false;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(entered.wait());

    engine.set_cloud_enabled(false);
    CHECK(finalization_of(engine).at("status") == "disabled");
    CHECK(finalization_of(engine).at("message") == "AI insights are off for this meeting.");
    if (reenable) {
        engine.enable_live_intelligence(live.route);
        CHECK(engine.store()->snapshot().at("cloud_enabled") == true);
    }
    release.set();
    REQUIRE(engine.wait_for_processing(seconds(5)));

    for (const Json& event : rig.events_of("intelligence")) {
        CHECK(event.value("error", std::string()).empty());
    }
    // The revoked start's tool host stays shut, whatever happened since.
    const auto tools = revoked->tools();
    REQUIRE(tools);
    const auto denied =
        tools->apply_agent_ops(Json::array({Json{{"op", "set_rolling_summary"},
                                                 {"text", "Late revoked write"},
                                                 {"evidence", Json::array({"sg_audio"})}}}));
    REQUIRE(denied.size() == 1);
    CHECK_FALSE(denied.front().ok);
    CHECK(denied.front().reason == "agent_writes_revoked");
    CHECK(revoked->passes().empty());
    CHECK(revoked->shutdown_calls() >= 1);
    if (reenable) {
        CHECK(created.load() == 2);
        CHECK(online(engine));
        CHECK(fresh->initialize_calls() == 1);
        CHECK(fresh->shutdown_calls() == 0);
        CHECK(finalization_of(engine).at("status") == "pending");
        // The fresh agent's host is open.
        REQUIRE(fresh->tools());
        const auto landed = fresh->tools()->apply_agent_ops(
            Json::array({Json{{"op", "set_rolling_summary"}, {"text", "Fresh consent"}}}));
        REQUIRE(landed.size() == 1);
        CHECK(landed.front().reason != "agent_writes_revoked");
    } else {
        CHECK(created.load() == 1);
        CHECK_FALSE(online(engine));
        CHECK(finalization_of(engine).at("status") == "disabled");
    }
}

TEST_CASE("test_unhealthy_core_reports_offline_and_meeting_still_starts", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_unhealthy_core_reports_offline_and_meeting_still_starts
    Live live;
    live.core->set_healthy(false);
    EngineRig rig;
    MeetingEngine& engine = rig.make(live.setup());

    const Json result = engine.start();

    CHECK_FALSE(result.at("meeting_id").get<std::string>().empty());
    // The meeting itself is unaffected: transcript-only, still active.
    CHECK(engine.is_active());
    REQUIRE(rig.asr(0));
    CHECK(static_cast<bool>(rig.asr(0)->segments_callback()));
    CHECK_FALSE(online(engine));
    const auto intel = rig.events_of("intelligence");
    REQUIRE_FALSE(intel.empty());
    CHECK(intel.back().at("online") == false);
    // A reason is reported, not silence.
    CHECK(intel.back().at("error") == std::string(kUnusable));
    const auto statuses = rig.events_of("status");
    REQUIRE_FALSE(statuses.empty());
    CHECK(statuses.back().at("intelligence_online") == false);
    CHECK(std::ranges::any_of(statuses, [](const Json& status) {
        return status.value("note", std::string()) == kUnusable;
    }));
    CHECK(finalization_of(engine).at("status") == "unavailable");
    CHECK(finalization_of(engine).at("message") == std::string(kUnusable));
    // The unusable core is released so a later retry rebuilds it, and no scheduler drives it.
    CHECK(live.core->shutdown_calls() == 1);
    CHECK(live.kinds().size() == 1);
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    MeetingEngineProbe(engine).notify_intelligence_segments(1);
    CHECK(live.core->passes().empty());
}

TEST_CASE("test_healthy_core_reports_online", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::test_healthy_core_reports_online
    // (and the agent-core half of test_start_persists_and_passes_llm_endpoint: the core is
    // configured with the approved destination).
    fv::FakeClock clock;
    Live live;
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();

    CHECK(online(engine));
    const auto intel = rig.events_of("intelligence");
    REQUIRE_FALSE(intel.empty());
    CHECK(intel.back().at("online") == true);
    CHECK(rig.events_of("status").back().at("intelligence_online") == true);
    CHECK(finalization_of(engine).at("status") == "pending");
    const auto config = live.core->config();
    REQUIRE(config);
    CHECK(config->meeting_id == engine.meeting_id());
    CHECK(config->provider == live.route.provider);
    CHECK(config->model == live.route.model);
    CHECK(config->endpoint == live.route.endpoint);
    // An installed agent runs under this charter alone, so every core is handed it.
    CHECK(config->system_prompt.find("meeting-intelligence copilot") != std::string::npos);
    // The scheduler started: committed transcript reaches the core once the warm-up passed.
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    REQUIRE(drive(engine, clock, [&] { return !live.core->passes().empty(); }));
    CHECK(live.core->passes().front().payload.new_segments.front().at("id") == "sg_1");
}

TEST_CASE("test_missing_pi_keeps_recording_without_starting_an_api_fallback",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_missing_pi_keeps_recording_without_starting_an_api_fallback. Pi is a packaged
    // sidecar there; the built-in agent runs in process here, so what carries over is a core
    // that cannot be made: the meeting records, one attempt is made and nothing else is tried.
    const bool factory_throws = GENERATE(true, false);
    CAPTURE(factory_throws);
    Live live;
    live.factory = [&](std::string_view) -> std::unique_ptr<fv::meeting_agent::AgentCore> {
        if (factory_throws) {
            throw std::runtime_error("The built-in meeting agent could not be loaded.");
        }
        return nullptr;
    };
    const std::string reason = factory_throws ? "The built-in meeting agent could not be loaded."
                                              : "The meeting agent is not available.";
    EngineRig rig;
    MeetingEngine& engine = rig.make(live.setup());

    const Json result = engine.start();

    CHECK(result.at("meeting_id") == engine.meeting_id());
    CHECK(engine.store()->snapshot().at("status") == "active");
    CHECK_FALSE(online(engine));
    REQUIRE(rig.asr(0));
    REQUIRE(rig.server(0));
    CHECK_FALSE(rig.server(0)->stopped.load());
    CHECK(live.kinds() == std::vector<std::string>{live.route.kind});
    const auto intel = rig.events_of("intelligence");
    REQUIRE_FALSE(intel.empty());
    CHECK(intel.back().at("online") == false);
    CHECK(intel.back().at("error") == reason);
    CHECK(finalization_of(engine).at("status") == "unavailable");
    CHECK(finalization_of(engine).at("message") ==
          "Meeting intelligence failed to start: " + reason);
    CHECK(MeetingEngineProbe(engine).intelligence_offline_message() ==
          "Meeting intelligence is offline; final cloud insights could not run. " + reason);
}

// ---- the scheduler's health signal ----

TEST_CASE("test_scheduler_health_signal_is_wired_and_propagates", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_scheduler_health_signal_is_wired_and_propagates. Python calls the scheduler's
    // on_health by hand; here the real scheduler reports it after three failed checkpoints and
    // again when one succeeds.
    fv::FakeClock clock;
    Live live;
    std::atomic<bool> failing{true};
    live.core->set_hook([&](const auto&, auto*) {
        if (failing.load()) {
            return failed("upstream timed out");
        }
        fv::meeting_agent::AgentResult ok;
        ok.ok = true;
        return ok;
    });
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(online(engine));
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    const std::size_t before = rig.events_of("intelligence").size();

    REQUIRE(drive(engine, clock, [&] { return !online(engine); }));

    CHECK(live.core->passes().size() >= 3);
    auto intel = rig.events_of("intelligence");
    REQUIRE(intel.size() == before + 1);
    CHECK(intel.back().at("online") == false);
    CHECK(intel.back().at("error") == "Checkpoints failed repeatedly; continuing transcript-only. "
                                      "Last error: upstream timed out");
    CHECK(finalization_of(engine).at("status") == "unavailable");
    CHECK(finalization_of(engine).at("message") == intel.back().at("error"));
    // Dashboard clients learn about it too.
    const auto broadcasts = rig.server(0)->messages_of("status");
    CHECK(std::ranges::any_of(broadcasts, [](const Json& message) {
        return message.value("intelligence_online", true) == false &&
               message.at("finalization").at("status") == "unavailable";
    }));
    // No backup was approved: the same core keeps trying.
    CHECK(live.kinds().size() == 1);

    failing = false;
    REQUIRE(drive(engine, clock, [&] { return online(engine); }));
    intel = rig.events_of("intelligence");
    CHECK(intel.back().at("online") == true);
    CHECK(finalization_of(engine).at("status") == "pending");
    CHECK(rig.server(0)->messages_of("status").back().at("intelligence_online") == true);
}

TEST_CASE("test_rejected_key_is_named_in_the_offline_message", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestIntelligenceHealth::
    // test_rejected_key_is_named_in_the_offline_message
    fv::FakeClock clock;
    Live live;
    std::atomic<bool> failing{true};
    live.core->set_hook([&](const auto&, auto*) {
        if (failing.load()) {
            return failed(R"(401: {"message":"User not found."})");
        }
        fv::meeting_agent::AgentResult ok;
        ok.ok = true;
        return ok;
    });
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    engine.start();
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});

    // Until the offline event is out too: the engine saves intelligence_online before it emits
    // the event, so under load the store can be ahead of the recorded events.
    const auto offline_event = [&] {
        const auto events = rig.events_of("intelligence");
        return !events.empty() && events.back().at("online") == false;
    };
    REQUIRE(drive(engine, clock, [&] { return !online(engine) && offline_event(); }));

    const std::string error = rig.events_of("intelligence").back().at("error").get<std::string>();
    CHECK(error == "Checkpoints failed repeatedly; continuing transcript-only. " +
                       std::string(kKeyRejected));
    // The provider's own words stay out of it.
    CHECK(error.find("User not found") == std::string::npos);
    CHECK(probe.intelligence_offline_message() ==
          "Meeting intelligence is offline; final cloud insights could not run. " +
              std::string(kKeyRejected));

    failing = false;
    REQUIRE(drive(engine, clock, [&] { return online(engine); }));
    CHECK(probe.intelligence_offline_message() ==
          "Meeting intelligence is offline; final cloud insights could not run.");
}

TEST_CASE("End runs the final passes when checkpoints failed and the live core is healthy",
          "[test_meeting_engine]") {
    // engine.py _end_worker and end_run.py EndRun.plan: failed checkpoints take the dashboard
    // offline, but the scheduler and its core stay, so End still runs cleanup and the report
    // while the core reports itself healthy.
    fv::FakeClock clock;
    Live live;
    live.core->set_hook([](const fv::meeting_agent::CheckpointPayload& payload, auto*) {
        if (!payload.is_polish && !payload.is_consolidation) {
            return failed("upstream timed out");
        }
        fv::meeting_agent::AgentResult ok;
        ok.ok = true;
        return ok;
    });
    EngineRig rig;
    EngineSetup setup = live.setup();
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    REQUIRE(drive(engine, clock, [&] { return !online(engine); }));

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(step_ids(finalization) ==
          std::vector<std::string>{"polish", "consolidation", "finalize"});
    CHECK(step(finalization, "polish").at("status") == "completed");
    CHECK(step(finalization, "consolidation").at("status") == "completed");
    CHECK(finalization.at("status") == "completed");
    // The live core and one core for each final pass.
    CHECK(live.kinds().size() == 3);
}

TEST_CASE("End reports intelligence offline when the backup could not take over",
          "[test_meeting_engine]") {
    // engine.py _switch_to_backup_agent stops the failing scheduler before the backup starts;
    // when the backup cannot come up no scheduler is left, so End runs no final pass and the
    // card names why insights left the first agent.
    fv::FakeClock clock;
    Live live;
    live.core->set_hook([](const auto&, auto*) { return failed("boom"); });
    live.backup_core->set_healthy(false);
    EngineRig rig;
    EngineSetup setup = live.setup(true);
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    REQUIRE(drive(engine, clock, [&] {
        return live.kinds().size() == 2 &&
               finalization_of(engine).at("message").get<std::string>().find(
                   "could not take over") != std::string::npos;
    }));
    const std::size_t passes = live.core->passes().size();

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(finalization.at("status") == "unavailable");
    CHECK(finalization.at("message") ==
          "Meeting intelligence is offline; final cloud insights could not run. Last error: boom");
    CHECK(step(finalization, "polish").is_null());
    CHECK(step(finalization, "consolidation").is_null());
    // No core was built for a final pass, and no pass reached either agent.
    CHECK(live.kinds() == std::vector<std::string>{live.route.kind, live.backup.kind});
    CHECK(live.core->passes().size() == passes);
    CHECK(live.backup_core->passes().empty());
}

// ---- the backup agent ----

TEST_CASE("test_backup_only_gets_transcript_since_insights_were_turned_back_on",
          "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestBackupAgent::
    // test_backup_only_gets_transcript_since_insights_were_turned_back_on. Python seeds the
    // backup's scheduler with the transcript so far; here both agents read one durable delivery
    // ledger, whose cursors a fresh approval moved to the end of what was said until then.
    fv::FakeClock clock;
    Live live;
    live.core->set_hook([](const auto&, auto*) { return failed("boom"); });
    EngineRig rig;
    EngineSetup setup = live.setup(true);
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_before", 0.0, 2.0, "Before the switch")});

    engine.set_cloud_enabled(false);
    rig.sql.repo->add_segments({mic_segment(id, "sg_off", 3.0, 4.0, "While insights were off")});
    engine.enable_live_intelligence(live.route, live.backup);
    REQUIRE(online(engine));
    rig.sql.repo->add_segments({mic_segment(id, "sg_after", 5.0, 8.0, "After the switch")});

    REQUIRE(drive(engine, clock, [&] { return !live.backup_core->passes().empty(); }));
    REQUIRE(fv::meeting::test::eventually([&] { return online(engine); }));

    CHECK(live.kinds() ==
          std::vector<std::string>{live.route.kind, live.route.kind, live.backup.kind});
    std::vector<std::string> delivered;
    const auto backup_passes = live.backup_core->passes();
    for (const Json& row : backup_passes.front().payload.new_segments) {
        delivered.push_back(row.at("id").get<std::string>());
    }
    CHECK(delivered == std::vector<std::string>{"sg_after"});
    CHECK(finalization_of(engine).at("message") ==
          "Your backup agent took over AI insights. Last error: boom");
    CHECK(rig.sql.repo->get_meeting(id)->at("agent_provider") == "codex");
    CHECK(rig.sql.repo->get_meeting(id)->at("agent_model") == "gpt-x");
    CHECK(rig.sql.repo->get_meeting(id)->at("agent_endpoint_json").is_null());
}

TEST_CASE("test_backup_is_tried_once", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestBackupAgent::test_backup_is_tried_once
    fv::FakeClock clock;
    Live live;
    live.core->set_hook([](const auto&, auto*) { return failed("boom"); });
    live.backup_core->set_hook([](const auto&, auto*) { return failed("boom again"); });
    EngineRig rig;
    EngineSetup setup = live.setup(true);
    setup.clock = &clock;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});

    // The backup takes over, then fails its own three checkpoints.
    REQUIRE(drive(engine, clock,
                  [&] { return live.backup_core->passes().size() >= 3 && !online(engine); }));
    // Nothing takes over from the backup, however long its checkpoints keep failing.
    const std::size_t passes = live.backup_core->passes().size();
    drive(
        engine, clock, [&] { return live.backup_core->passes().size() >= passes + 2; }, seconds(2));

    CHECK(live.kinds() == std::vector<std::string>{live.route.kind, live.backup.kind});
    CHECK(live.core->shutdown_calls() == 1);
    CHECK_FALSE(online(engine));
    CHECK(finalization_of(engine).at("status") == "unavailable");
    CHECK(finalization_of(engine).at("message") ==
          "Checkpoints failed repeatedly; continuing transcript-only. Last error: boom again");
}

TEST_CASE("test_backup_never_starts_once_the_meeting_is_ending", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestBackupAgent::
    // test_backup_never_starts_once_the_meeting_is_ending. Python marks the engine as ending and
    // reports the failure by hand; here the third failing checkpoint is held until End has
    // claimed the meeting, so the scheduler's offline report lands on an ending meeting.
    fv::FakeClock clock;
    Live live;
    Event third;
    Event release;
    std::atomic<int> calls{0};
    live.core->set_hook([&](const auto&, auto*) {
        if (++calls == 3) {
            third.set();
            release.wait(seconds(5));
        }
        return failed("boom");
    });
    EngineRig rig;
    EngineSetup setup = live.setup(true);
    setup.clock = &clock;
    // End then builds no core of its own.
    setup.options.end_polish = false;
    setup.options.end_report = false;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    rig.sql.repo->add_segments({mic_segment(engine.meeting_id(), "sg_1", 0.0, 2.0, "Hello")});
    REQUIRE(drive(engine, clock, [&] { return third.is_set(); }));

    engine.end();
    release.set();
    REQUIRE(engine.wait_for_end(seconds(10)));

    CHECK(live.kinds() == std::vector<std::string>{live.route.kind});
    CHECK(live.backup_core->initialize_calls() == 0);
    CHECK(rig.sql.repo->get_meeting(engine.meeting_id())->at("agent_provider") ==
          live.route.provider);
}

// ---- the agent's write gate ----

TEST_CASE("test_live_writes_follow_the_engine_gate", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestAgentToolHost::test_live_writes_follow_the_engine_gate
    Live live;
    EngineRig rig;
    MeetingEngine& engine = rig.make(live.setup());
    engine.start();
    rig.sql.repo->add_segments(
        {mic_segment(engine.meeting_id(), "sg_said", 0.0, 1.0, "Ship it Friday")});
    const auto tools = live.core->tools();
    REQUIRE(tools);
    const auto topic = [](const char* text) {
        return Json::array(
            {Json{{"op", "set_topic"}, {"text", text}, {"evidence", Json::array({"sg_said"})}}});
    };

    const auto landed = tools->apply_agent_ops(topic("Launch"));
    REQUIRE(landed.size() == 1);
    CHECK(landed.front().ok);
    CHECK(engine.store()->snapshot().at("topic").at("current") == "Launch");
    CHECK(tools->segment_exists("sg_said"));
    CHECK_FALSE(tools->segment_exists("sg_never"));

    engine.revoke_agent_writes();
    const auto refused = tools->apply_agent_ops(topic("Too late"));
    REQUIRE(refused.size() == 1);
    CHECK(refused.front().reason == "agent_writes_revoked");
    CHECK(tools->ask_question("Why?", Json::array({"sg_said"})).reason == "agent_writes_revoked");
    CHECK(engine.store()->snapshot().at("topic").at("current") == "Launch");
}

// ---- End's cleanup and report passes ----

TEST_CASE("test_cloud_on_polishes_draft_without_offline_replace", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_cloud_on_polishes_draft_without_offline_replace
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    EngineSetup setup = setup_with(true);
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_draft", 0.0, 1.0, "draft")});
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_final", 0.0, 2.0, "final")});

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(rig.asr(0)->offline_passes.load() == 0);
    CHECK(intelligence->polished() == std::vector<std::string>{id});
    CHECK(intelligence->consolidated() == std::vector<std::string>{id});
    const auto ids = segment_ids(rig, id);
    CHECK(ids.contains("sg_draft"));
    CHECK_FALSE(ids.contains("sg_final"));
    CHECK(finalization.at("status") == "completed");
    CHECK(step_ids(finalization) ==
          std::vector<std::string>{"polish", "consolidation", "finalize"});
}

TEST_CASE("End runs cleanup and the report through the app's provider and repairs the record",
          "[test_meeting_engine]") {
    // engine.py _end_worker with scheduler.py run_final_polish, run_consolidation and
    // _repair_after_consolidation: here each final pass is a throwaway core of the provider, the
    // live one having been shut down when capture stopped.
    Live live;
    live.core->set_hook([](const fv::meeting_agent::CheckpointPayload& payload,
                           fv::meeting_agent::AgentToolHost* tools) {
        fv::meeting_agent::AgentResult result;
        result.ok = true;
        if (payload.is_consolidation && tools != nullptr) {
            result.op_results =
                tools->apply_agent_ops(Json::array({Json{{"op", "add_item"},
                                                         {"card", "key_points"},
                                                         {"text", "Budget review ships Friday"},
                                                         {"evidence", Json::array({"sg_1"})}}}));
        }
        return result;
    });
    EngineRig rig;
    MeetingEngine& engine = rig.make(live.setup());
    engine.start();
    REQUIRE(online(engine));
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments(
        {mic_segment(id, "sg_1", 0.0, 2.0, "We should ship the budget review on Friday.")});

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(finalization.at("status") == "completed");
    CHECK(step_ids(finalization) ==
          std::vector<std::string>{"polish", "consolidation", "finalize"});
    for (const Json& entry : finalization.at("steps")) {
        CHECK(entry.at("status") == "completed");
    }
    // One core for the live meeting, one for each final pass, every one shut down.
    CHECK(live.kinds().size() == 3);
    CHECK(live.core->shutdown_calls() == 3);
    const auto passes = live.core->passes();
    REQUIRE(passes.size() == 2);
    CHECK(passes[0].payload.is_polish);
    CHECK(passes[1].payload.is_consolidation);
    CHECK(passes[1].payload.new_segments.front().at("id") == "sg_1");
    // The agent's key point landed, and the repair gave the record its story beat.
    const Json cards = engine.store()->snapshot().at("cards");
    REQUIRE(cards.at("key_points").size() == 1);
    CHECK(cards.at("key_points")[0].at("author_type") == "agent");
    std::vector<Json> timeline;
    for (const Json& item : cards.at("timeline")) {
        if (item.value("status", std::string()) != "removed") {
            timeline.push_back(item);
        }
    }
    REQUIRE(timeline.size() == 1);
    CHECK(timeline[0].at("author_type") == "system");
    // Writes after End are refused.
    CHECK_FALSE(engine.agent_writes_allowed());
    REQUIRE(live.core->tools());
    const auto late = live.core->tools()->apply_agent_ops(
        Json::array({Json{{"op", "set_rolling_summary"}, {"text", "Too late"}}}));
    REQUIRE(late.size() == 1);
    CHECK(late.front().reason == "agent_writes_revoked");
}

TEST_CASE("test_offline_redecode_runs_polish_then_consolidation", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::
    // test_offline_redecode_runs_polish_then_consolidation
    class Ordered final : public fv::meeting::test::OnlineProvider {
    public:
        Json polish(const fv::meeting::RerunContext& context, const fv::CancelToken&) override {
            const std::lock_guard lock(mutex);
            order.emplace_back("polish");
            for (const Json& row : context.segments) {
                polished.push_back(row.at("id").get<std::string>());
            }
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext& context,
                         const fv::CancelToken&) override {
            const std::lock_guard lock(mutex);
            order.emplace_back("consolidation");
            for (const Json& row : context.segments) {
                consolidated.push_back(row.at("id").get<std::string>());
            }
            return {{"status", "completed"}};
        }
        std::mutex mutex;
        std::vector<std::string> order;
        std::vector<std::string> polished;
        std::vector<std::string> consolidated;
    };
    EngineRig rig;
    auto intelligence = std::make_shared<Ordered>();
    EngineSetup setup = setup_with(true);
    setup.options.end_redecode = true;
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    const std::string id = engine.meeting_id();
    rig.sql.repo->add_segments({mic_segment(id, "sg_draft", 0.0, 1.0, "draft")});
    rig.asr(0)->set_offline_segments({mic_segment(id, "sg_final", 0.0, 2.0, "final")});

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(rig.asr(0)->offline_passes.load() == 1);
    CHECK(intelligence->order == std::vector<std::string>{"polish", "consolidation"});
    // Both passes worked on the re-decoded transcript, not on the draft it replaced.
    CHECK(intelligence->polished == std::vector<std::string>{"sg_final"});
    CHECK(intelligence->consolidated == std::vector<std::string>{"sg_final"});
    CHECK(segment_ids(rig, id) == std::set<std::string>{"sg_final"});
    CHECK(finalization.at("status") == "completed");
    CHECK(step_ids(finalization) ==
          std::vector<std::string>{"redecode", "polish", "consolidation", "finalize"});
    for (const Json& entry : finalization.at("steps")) {
        CHECK(entry.at("status") == "completed");
    }
}

TEST_CASE("test_end_skips_polish_when_disabled", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_skips_polish_when_disabled
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    EngineSetup setup = setup_with(true);
    setup.options.end_polish = false;
    setup.options.end_report = true;
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(intelligence->polished().empty());
    CHECK(intelligence->consolidated() == std::vector<std::string>{engine.meeting_id()});
    CHECK(step(finalization, "polish").is_null());
    CHECK(step(finalization, "consolidation").at("status") == "completed");
    CHECK(finalization.at("status") == "completed");
}

TEST_CASE("test_end_skips_report_when_disabled", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_skips_report_when_disabled
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    EngineSetup setup = setup_with(true);
    setup.options.end_polish = true;
    setup.options.end_report = false;
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(intelligence->polished() == std::vector<std::string>{engine.meeting_id()});
    CHECK(intelligence->consolidated().empty());
    CHECK(step(finalization, "consolidation").is_null());
    CHECK(finalization.at("status") == "completed");
    CHECK(finalization.at("message") == "Transcript cleanup is ready.");
}

TEST_CASE("test_end_skips_llm_when_both_disabled", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestEndLifecycle::test_end_skips_llm_when_both_disabled
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    EngineSetup setup = setup_with(true);
    setup.options.end_polish = false;
    setup.options.end_report = false;
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();

    const Json finalization = EngineRig::end_and_wait(engine);

    CHECK(intelligence->polished().empty());
    CHECK(intelligence->consolidated().empty());
    CHECK(finalization.at("status") == "completed");
    CHECK(finalization.at("message") == "Post-meeting cleanup and report are off.");
    CHECK_FALSE(engine.agent_writes_allowed());
}

TEST_CASE("test_demo_mode_end_runs_polish_and_report", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::TestDemoMeeting::test_demo_mode_end_runs_polish_and_report
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    EngineSetup setup = setup_with(true);
    setup.options.demo_mode = true;
    setup.intelligence = intelligence;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    const std::string id = engine.meeting_id();

    const Json finalization = EngineRig::end_and_wait(engine);

    REQUIRE_FALSE(rig.events_of("ended").empty());
    CHECK(rig.events_of("ended").back().at("status") == "ended");
    CHECK(rig.sql.repo->get_meeting(id)->at("status") == "ended");
    CHECK(intelligence->polished() == std::vector<std::string>{id});
    CHECK(intelligence->consolidated() == std::vector<std::string>{id});
    // The passes were handed the seeded demo transcript.
    CHECK(intelligence->last_context().segments.size() >= 10);
    CHECK(finalization.at("status") == "completed");
    CHECK(rig.asr_count() == 0);
}

TEST_CASE("test_final_highlights_run_after_transcript_edits", "[test_meeting_engine]") {
    // from tests/test_meeting_engine.py::test_final_highlights_run_after_transcript_edits. The
    // hook stands where the final highlights run, as Python's replaced _finalize_highlights.
    const bool end_report = GENERATE(true, false);
    CAPTURE(end_report);
    EngineRig rig;
    auto intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    EngineSetup setup = setup_with(true);
    setup.options.end_report = end_report;
    setup.intelligence = intelligence;
    setup.settings = [] {
        return Json{{"typesafe_enabled", true}, {"typesafe_highlights_enabled", true}};
    };
    setup.local_fast_judge = [judge] {
        return judge;
    };
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    std::atomic<int> runs{0};
    std::atomic<int> sound{0};
    probe.final_highlights([&] {
        ++runs;
        const bool after_everything = probe.asr() == nullptr && !probe.fast_features_running() &&
                                      engine.store()->snapshot().at("status") == "ended" &&
                                      intelligence->polished().size() == 1 &&
                                      intelligence->consolidated().size() == (end_report ? 1u : 0u);
        sound += after_everything ? 1 : 0;
    });
    engine.start();
    // The live highlights worker runs during the meeting and is gone by the final pass.
    REQUIRE(probe.fast_features_running());

    EngineRig::end_and_wait(engine);

    CHECK(runs.load() == 1);
    CHECK(sound.load() == 1);
}
