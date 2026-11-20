// tests/test_meeting_runtime.py: retrying post-meeting steps from the desktop, and the retry
// publication tests (generations and claims: progress is published only while the claim is
// current, an old retry's completion never finishes a new one, the claim is held through
// publication).
//
// The UI actor and the worker threads are ManualExecutors, so where Python waits on a barrier
// in another thread, these tests check the same state from inside the step that Python blocks
// in.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/utf8.h"
#include "meeting/refinalize.h"
#include "meeting/runtime_messages.h"
#include "meeting_state/stored.h"
#include "pyport/json.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

Json retry_result(const std::string& message = "First retry completed") {
    return Json{{"ok", true},
                {"finalization", Json{{"status", "completed"}, {"message", message}}}};
}

// Python's retry_publication fixture: an ended meeting whose engine is kept, whose
// publish_finalization queues a status event as the live engine does.
struct Publication {
    explicit Publication(bool with_engine = true) {
        rig.make_meeting(id, Json{{"asr_model", "mock"}});
        engine = std::make_shared<FakeEngine>(id);
        engine->active = false;
        auto probe = rig.probe();
        engine->on_publish = [this](const Json& snapshot) {
            rig.probe().queue(engine, "status",
                              Json{{"status", "ended"}, {"finalization", snapshot}});
        };
        if (with_engine) {
            probe.set_engine(engine);
        }
        probe.set_card(id);
    }

    int spawned_retries() {
        return static_cast<int>(
            std::ranges::count(rig.spawned_names(), std::string("meeting-retry-finalization")));
    }
    std::vector<Json> finalization_states() {
        std::vector<Json> states;
        for (const Json& state : rig.got.states) {
            if (state.contains("finalization") && state["finalization"].is_object()) {
                states.push_back(state["finalization"]);
            }
        }
        return states;
    }

    RuntimeRig rig;
    std::string id = "m_retry_order";
    std::shared_ptr<FakeEngine> engine;
};

void speaker_retry_meeting(RuntimeRig& rig, const std::string& meeting_id = "m_speakers") {
    fv::meeting_state::MeetingState state;
    state.meeting_id = meeting_id;
    state.status = "ended";
    state.cloud_enabled = false;
    state.finalization.status = "failed";
    state.finalization.message = "Speaker Identification failed.";
    state.finalization.steps = Json::array({Json{{"id", "speaker_id"},
                                                 {"name", "Speaker Identification"},
                                                 {"status", "failed"},
                                                 {"detail", "api down"}},
                                            Json{{"id", "finalize"},
                                                 {"name", "State Finalization"},
                                                 {"status", "completed"},
                                                 {"detail", "Done"}}});
    state.finalization.total_steps = 2;
    rig.make_meeting(meeting_id, Json{{"title", "Sync"},
                                      {"spool_dir", fv::path_to_utf8(rig.root.path() / "spool")},
                                      {"state_json", fv::py::dumps(state.to_json())},
                                      {"state_seq", 0}});
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_lb";
    segment.meeting_id = meeting_id;
    segment.channel = "loopback";
    segment.start_s = 0.0;
    segment.end_s = 2.0;
    segment.text = "hello";
    rig.repo->add_segments({segment});
}

} // namespace

TEST_CASE("a restarted runtime requires a fresh saved-meeting approval for every retry",
          "[test_meeting_runtime]") {
    class InlineUi final : public fv::Executor {
    public:
        void post(fv::Task task) override { task(); }
        fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task) override { return {}; }
        bool is_current() const override { return true; }
    } ui;
    bool approve = false;
    bool change_destination = false;
    unsigned prompts = 0;
    std::function<void()> change;
    RuntimeRig rig([&](MeetingRuntimeDeps& deps) {
        deps.ui = &ui;
        deps.confirm_saved_insights = [&](const Json& meeting,
                                          const fv::meeting_agent::MeetingAgentRoute& route,
                                          bool report) {
            ++prompts;
            CHECK_FALSE(report);
            CHECK(meeting.at("id") == "m_saved");
            CHECK(meeting.at("title") == "Review notes");
            CHECK(route.provider == "codex");
            CHECK(route.model == "fixture-model");
            if (change_destination) {
                change();
            }
            return approve;
        };
    });
    rig.make_ended_meeting(
        "m_saved", false, "failed", "2026-08-19T18:00:00",
        {{"title", "Review notes"}, {"agent_provider", "codex"}, {"agent_model", "fixture-model"}});
    unsigned runs = 0;
    rig.rerun = [&](const std::string& id, const RerunRequest& request) {
        ++runs;
        CHECK(id == "m_saved");
        REQUIRE(request.approved_route);
        CHECK(request.approved_route->provider == "codex");
        REQUIRE(request.saved_destination_allowed);
        CHECK(request.saved_destination_allowed());
        return retry_result();
    };
    rig.restart();
    rig.drain();
    CHECK(prompts == 0);
    CHECK(runs == 0);
    rig.rt->retry_step("consolidation", "m_saved");
    rig.drain();
    CHECK(prompts == 1);
    CHECK(runs == 0);
    CHECK_FALSE(rig.rt->is_finalizing());

    approve = true;
    rig.rt->retry_step("consolidation", "m_saved");
    rig.drain();
    CHECK(prompts == 2);
    CHECK(runs == 1);
    CHECK_FALSE(rig.rt->is_finalizing());

    change_destination = true;
    change = [&] {
        rig.repo->update_meeting("m_saved", Json{{"agent_model", "changed-during-confirmation"}});
    };
    rig.rt->retry_step("consolidation", "m_saved");
    rig.drain();
    CHECK(prompts == 3);
    CHECK(runs == 1);
    CHECK_FALSE(rig.rt->is_finalizing());
}

TEST_CASE("saved report destinations ask for fresh consent before starting their writer",
          "[test_meeting_runtime]") {
    class InlineUi final : public fv::Executor {
    public:
        void post(fv::Task task) override { task(); }
        fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task) override { return {}; }
        bool is_current() const override { return true; }
    } ui;
    const std::string destination = GENERATE(std::string("openai"), std::string("codex"));
    bool approve = false;
    unsigned prompts = 0;
    unsigned reports = 0;
    RuntimeRig rig([&](MeetingRuntimeDeps& deps) {
        deps.ui = &ui;
        deps.confirm_saved_insights = [&](const Json& meeting,
                                          const fv::meeting_agent::MeetingAgentRoute& route,
                                          bool report) {
            ++prompts;
            CHECK(report);
            CHECK(meeting.at("id") == "m_saved_report");
            CHECK(route.provider == destination);
            return approve;
        };
        deps.start_saved_report = [&](std::shared_ptr<fv::meeting_state::MeetingStateStore> store,
                                      const fv::meeting_agent::MeetingAgentRoute& route,
                                      std::string_view text, fv::Executor&, const fv::CancelToken&,
                                      std::function<bool()> allowed) {
            ++reports;
            CHECK(store->meeting_id() == "m_saved_report");
            CHECK(route.provider == destination);
            CHECK(text == "Write a budget brief.");
            CHECK(allowed());
            return Json{{"ok", true}, {"report_id", "rep_fixture"}};
        };
    });
    rig.make_ended_meeting(
        "m_saved_report", false, "completed", "2026-08-19T18:00:00",
        {{"title", "Budget"}, {"agent_provider", destination}, {"agent_model", "fixture-model"}});
    const auto meeting = rig.repo->get_meeting("m_saved_report");
    REQUIRE(meeting);
    const auto store = fv::meeting_state::open_store(rig.repo, *meeting, true, Json::object());
    const auto passes = rig.probe().post_meeting_passes();
    CHECK_THROWS_AS(passes.report(store, *meeting, "Write a budget brief.", ui, {}),
                    fv::meeting_web::HttpError);
    CHECK(prompts == 1);
    CHECK(reports == 0);

    approve = true;
    CHECK(passes.report(store, *meeting, "Write a budget brief.", ui, {}).at("ok") == true);
    CHECK(prompts == 2);
    CHECK(reports == 1);
    fv::CancelSource stop;
    stop.cancel();
    CHECK_THROWS_AS(passes.report(store, *meeting, "Write a budget brief.", ui, stop.token()),
                    fv::meeting_web::HttpError);
    CHECK(prompts == 2);
    CHECK(reports == 1);
}

// from tests/test_meeting_runtime.py::test_retry_insights_blocked_when_active
TEST_CASE("test_retry_insights_blocked_when_active", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_card");
    rig.probe().set_card("m_card");
    rig.probe().set_controller_active(true);
    rig.rt->retry_insights();
    rig.drain();
    CHECK_FALSE(rig.rt->is_finalizing());
}

// from tests/test_meeting_runtime.py::test_retry_insights_runs_and_updates_finalization
TEST_CASE("test_retry_insights_runs_and_updates_finalization", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_test_123", Json{{"asr_model", "base"}});
    auto engine = std::make_shared<FakeEngine>("m_test_123");
    engine->active = false;
    rig.probe().set_engine(engine);
    std::vector<std::string> from_steps;
    rig.rerun = [&from_steps](const std::string&, const RerunRequest& request) {
        from_steps.push_back(request.from_step);
        return Json{
            {"ok", true},
            {"applied", 5},
            {"error", nullptr},
            {"finalization", Json{{"status", "completed"},
                                  {"message", "Final insights are ready."},
                                  {"steps", Json::array({Json{{"id", "consolidation"},
                                                              {"name", "Summary & Action Items"},
                                                              {"status", "completed"}}})}}}};
    };

    rig.rt->retry_insights();
    rig.drain();

    CHECK(from_steps == std::vector<std::string>{"failed"});
    const Json finalization = rig.probe().finalization().value();
    CHECK(finalization["status"] == "completed");
    CHECK_THAT(finalization["message"].get<std::string>(), ContainsSubstring("ready"));
    const auto published = engine->published();
    REQUIRE_FALSE(published.empty());
    CHECK(published.back()["status"] == "completed");
    CHECK(published.back()["message"] == "Final insights are ready.");
    CHECK(engine->allow_writes == 1);
    CHECK(engine->revoke_writes == 1);
}

// from tests/test_meeting_runtime.py::test_retry_waits_for_a_run_already_holding_the_meeting
TEST_CASE("test_retry_waits_for_a_run_already_holding_the_meeting", "[test_meeting_runtime]") {
    // A dashboard re-run in progress is neither duplicated nor overwritten.
    RuntimeRig rig;
    auto engine = std::make_shared<FakeEngine>("m_busy");
    engine->active = false;
    rig.probe().set_engine(engine);
    FinalizationClaim dashboard("m_busy");

    rig.rt->retry_insights();
    rig.drain();

    CHECK_FALSE(rig.rt->is_finalizing());
    CHECK(rig.got.statuses ==
          std::vector<std::string>{"Post-meeting steps are already running for this meeting."});
    CHECK(engine->published().empty());
}

// from tests/test_meeting_runtime.py::test_retry_after_engine_teardown_uses_card_meeting
TEST_CASE("test_retry_after_engine_teardown_uses_card_meeting", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_card", Json{{"asr_model", "base"}, {"started_at", "2026-11-17T08:00:00"}});
    rig.make_meeting("m_other", Json{{"started_at", "2026-11-17T09:30:00"}});
    rig.probe().set_card("m_card");
    std::vector<std::pair<std::string, std::string>> called;
    rig.rerun = [&called](const std::string& meeting_id, const RerunRequest& request) {
        called.emplace_back(meeting_id, request.from_step);
        return Json{{"ok", true},
                    {"applied", 1},
                    {"error", nullptr},
                    {"finalization", Json{{"status", "completed"},
                                          {"message", "Final insights are ready."},
                                          {"steps", Json::array()}}}};
    };
    rig.rt->retry_step("polish");
    rig.drain();
    CHECK(called == std::vector<std::pair<std::string, std::string>>{{"m_card", "polish"}});
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-retry-lookup")) == 0);
}

// from tests/test_meeting_runtime.py::test_retry_without_card_uses_list_meetings
TEST_CASE("test_retry_without_card_uses_list_meetings", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_latest", Json{{"asr_model", "base"}});
    std::vector<std::string> called;
    rig.rerun = [&called](const std::string& meeting_id, const RerunRequest&) {
        called.push_back(meeting_id);
        return Json{{"ok", true},
                    {"finalization", Json{{"status", "completed"}, {"message", "ready"}}}};
    };
    rig.rt->retry_insights();
    rig.drain();
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-retry-lookup")) == 1);
    CHECK(called == std::vector<std::string>{"m_latest"});
}

// from tests/test_meeting_runtime.py::test_retry_finalization_from_worker_uses_signal
TEST_CASE("test_retry_finalization_from_worker_uses_signal", "[test_meeting_runtime]") {
    // Retry must not rebuild widgets on the finalization worker.
    RuntimeRig rig;
    rig.make_meeting("m_card", Json{{"asr_model", "base"}});
    rig.probe().set_card("m_card");
    rig.rerun = [](const std::string&, const RerunRequest&) {
        return Json{{"ok", true},
                    {"applied", 1},
                    {"error", nullptr},
                    {"finalization", Json{{"status", "completed"},
                                          {"message", "Final insights are ready."},
                                          {"steps", Json::array()}}}};
    };
    rig.rt->retry_step("polish");
    rig.workers.run_all();
    CHECK_FALSE(rig.rt->is_finalizing());
    CHECK(rig.got.refreshes == 0);
    rig.ui.run_all();
    CHECK(rig.got.refreshes == 1);
}

// from tests/test_meeting_runtime.py::test_desktop_speaker_retry_never_uploads_when_ineligible
TEST_CASE("test_desktop_speaker_retry_never_uploads_when_ineligible", "[test_meeting_runtime]") {
    const Json saved = GENERATE(Json{{"meeting_speaker_id_backend", "openai"},
                                     {"meeting_audio_upload_consent_given", false}},
                                Json{{"meeting_speaker_id_backend", "local"},
                                     {"meeting_audio_upload_consent_given", true}});
    RuntimeRig rig;
    rig.settings.update(saved);
    speaker_retry_meeting(rig);
    rig.probe().set_card("m_speakers");
    rig.rerun = [](const std::string& meeting_id, const RerunRequest& request) {
        return rerun_finalization(meeting_id, request);
    };

    // The Re-run speakers button refuses before starting anything.
    rig.rt->retry_speakers();
    rig.drain();
    CHECK_FALSE(rig.rt->is_finalizing());
    REQUIRE_FALSE(rig.got.statuses.empty());
    const std::string last = rig.got.statuses.back();
    CHECK((last == "Audio-upload consent has not been given." ||
           last == "Speaker identification is not set to OpenAI."));

    // Retrying the failed speaker step reaches the pipeline, which skips it: nothing uploads.
    rig.rt->retry_step("failed");
    rig.drain();
    const Json finalization = rig.probe().finalization().value();
    Json speaker_step;
    for (const Json& step : finalization["steps"]) {
        if (step["id"] == "speaker_id") {
            speaker_step = step;
        }
    }
    CHECK(speaker_step["status"] == "completed");
    CHECK(finalization["status"] != "failed");
}

// from tests/test_meeting_runtime.py::test_retry_holds_busy_until_terminal_publication
TEST_CASE("test_retry_holds_busy_until_terminal_publication", "[test_meeting_runtime]") {
    const bool with_engine = GENERATE(false, true);
    const bool terminal_progress = GENERATE(false, true);
    Publication p(with_engine);
    auto probe = p.rig.probe();
    int terminal_count = 0;
    bool entered = false;
    probe.set_publish_hook([&](const std::string& meeting_id, const Json& finalization) {
        if (finalization.value("status", "") != "completed") {
            return;
        }
        ++terminal_count;
        if (terminal_count != (terminal_progress ? 2 : 1)) {
            return;
        }
        entered = true;
        // The terminal publication is under way: the run still owns the meeting.
        p.rig.ui.run_all();
        CHECK(is_running(meeting_id));
        CHECK(p.rig.rt->is_finalizing());
        CHECK(p.rig.rt->meeting_busy(meeting_id));
        p.rig.rt->retry_insights();
        CHECK(p.spawned_retries() == 1);
    });
    p.rig.rerun = [&](const std::string& meeting_id, const RerunRequest& request) {
        return run_claimed(meeting_id, request.claim, [&] {
            if (terminal_progress) {
                request.progress(retry_result()["finalization"]);
            }
            return retry_result();
        });
    };

    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();

    CHECK(entered);
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK_FALSE(p.rig.rt->meeting_busy(p.id));
    REQUIRE_FALSE(p.finalization_states().empty());
    CHECK(p.finalization_states().back()["status"] == "completed");
    if (with_engine) {
        CHECK(p.engine->published().back()["status"] == "completed");
    }
}

// from tests/test_meeting_runtime.py::test_queued_old_retry_completion_cannot_finish_new_retry
TEST_CASE("test_queued_old_retry_completion_cannot_finish_new_retry", "[test_meeting_runtime]") {
    const bool with_engine = GENERATE(false, true);
    const bool terminal_progress = GENERATE(false, true);
    Publication p(with_engine);
    std::vector<std::function<void(const Json&)>> callbacks;
    bool second_ran = false;
    p.rig.rerun = [&](const std::string& meeting_id, const RerunRequest& request) {
        return run_claimed(meeting_id, request.claim, [&] {
            callbacks.push_back(request.progress);
            const auto run = callbacks.size();
            if (run == 1 && terminal_progress) {
                request.progress(retry_result()["finalization"]);
            }
            if (run == 2) {
                second_ran = true;
                // The first worker's events, queued until now, reach the UI actor while the
                // second retry runs: none of them may finish it.
                const auto before = p.finalization_states().size();
                const auto notes_before = p.rig.got.statuses.size();
                p.rig.ui.run_all();
                const auto states = p.finalization_states();
                for (auto i = before; i < states.size(); ++i) {
                    CHECK(states[i]["status"] == "running");
                }
                for (auto i = notes_before; i < p.rig.got.statuses.size(); ++i) {
                    CHECK(p.rig.got.statuses[i].find("completed") == std::string::npos);
                }
                CHECK(p.rig.rt->is_finalizing());
                CHECK(p.rig.probe().finalization().value()["status"] == "running");
                CHECK(is_running(meeting_id));
                REQUIRE_FALSE(states.empty());
                CHECK(states.back()["status"] == "running");

                // A late callback of the first run changes nothing.
                const auto published = p.engine->published().size();
                const auto count = p.rig.got.states.size();
                callbacks[0](retry_result("Late first retry callback")["finalization"]);
                p.rig.ui.run_all();
                CHECK(p.engine->published().size() == published);
                CHECK(p.rig.got.states.size() == count);
                CHECK(p.rig.rt->is_finalizing());
            }
            return retry_result("Retry " + std::to_string(run) + " completed");
        });
    };

    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.workers.run_all();
    CHECK_FALSE(p.rig.rt->is_finalizing());
    // Leave the first worker's events queued until the second retry starts.
    p.rig.rt->retry_insights();
    p.rig.drain();

    CHECK(second_ran);
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK(p.rig.probe().finalization().value()["message"] == "Retry 2 completed");
    CHECK(p.finalization_states().back()["message"] == "Retry 2 completed");
}

// from tests/test_meeting_runtime.py::test_retry_failure_releases_ownership_after_publishing
TEST_CASE("test_retry_failure_releases_ownership_after_publishing", "[test_meeting_runtime]") {
    Publication p;
    p.rig.rerun = [](const std::string&, const RerunRequest&) -> Json {
        throw std::runtime_error("synthetic retry failure");
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK_FALSE(p.rig.rt->meeting_busy(p.id));
    CHECK(p.finalization_states().back()["status"] == "failed");
    CHECK_THAT(p.finalization_states().back()["message"].get<std::string>(),
               ContainsSubstring("synthetic retry failure"));

    p.rig.rerun = [](const std::string&, const RerunRequest&) {
        return retry_result();
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK(p.finalization_states().back()["status"] == "completed");
}

// from tests/test_meeting_runtime.py::test_retry_keeps_shared_claim_through_publication
TEST_CASE("test_retry_keeps_shared_claim_through_publication", "[test_meeting_runtime]") {
    const bool with_engine = GENERATE(false, true);
    Publication p(with_engine);
    const auto pipeline = [](const std::string& meeting_id, FinalizationClaim* claim) {
        return run_claimed(meeting_id, claim, [] { return retry_result(); });
    };
    bool entered = false;
    p.rig.probe().set_publish_hook([&](const std::string& meeting_id, const Json& finalization) {
        if (finalization.value("status", "") != "completed") {
            return;
        }
        entered = true;
        CHECK_THROWS_AS(pipeline(meeting_id, nullptr), FinalizationBusyError);
        CHECK(p.rig.rt->meeting_busy(meeting_id));
    });
    p.rig.rerun = [&](const std::string& meeting_id, const RerunRequest& request) {
        return pipeline(meeting_id, request.claim);
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    CHECK(entered);
    CHECK_FALSE(is_running(p.id));
    CHECK(pipeline(p.id, nullptr)["ok"] == true);
}

// from tests/test_meeting_runtime.py::test_new_dashboard_claim_discards_queued_desktop_completion
TEST_CASE("test_new_dashboard_claim_discards_queued_desktop_completion", "[test_meeting_runtime]") {
    const bool with_engine = GENERATE(false, true);
    Publication p(with_engine);
    std::vector<std::function<void(const Json&)>> callbacks;
    p.rig.rerun = [&](const std::string& meeting_id, const RerunRequest& request) {
        return run_claimed(meeting_id, request.claim, [&] {
            callbacks.push_back(request.progress);
            return retry_result();
        });
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.workers.run_all();
    FinalizationClaim dashboard(p.id);
    const auto states_before = p.rig.got.states.size();
    const auto publications_before = p.engine->published().size();
    p.rig.ui.run_all();
    callbacks.at(0)(retry_result("Late desktop callback")["finalization"]);
    p.rig.ui.run_all();
    CHECK(p.rig.got.states.size() == states_before);
    CHECK(p.engine->published().size() == publications_before);
    CHECK(p.rig.rt->meeting_busy(p.id));
    dashboard.release();
}

// from tests/test_meeting_runtime.py::test_failed_retry_publication_releases_shared_claim
TEST_CASE("test_failed_retry_publication_releases_shared_claim", "[test_meeting_runtime]") {
    Publication p;
    p.rig.probe().set_publish_hook([](const std::string&, const Json& finalization) {
        if (finalization.value("status", "") == "completed") {
            throw std::runtime_error("synthetic publication failure");
        }
    });
    p.rig.rerun = [](const std::string&, const RerunRequest&) {
        return retry_result();
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK_FALSE(p.rig.rt->meeting_busy(p.id));
    CHECK_FALSE(is_running(p.id));
    CHECK(p.finalization_states().back()["status"] == "completed");
    FinalizationClaim claim(p.id);
    claim.release();
}

// from tests/test_meeting_runtime.py::test_lost_retry_admission_does_not_queue_an_unowned_snapshot
TEST_CASE("test_lost_retry_admission_does_not_queue_an_unowned_snapshot",
          "[test_meeting_runtime]") {
    Publication p;
    bool reached = false;
    p.rig.rerun = [&reached](const std::string&, const RerunRequest&) {
        reached = true; // a run that lost admission must not reach the pipeline
        return retry_result();
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    // Another dashboard wins admission between the claim and the worker.
    auto competing = std::make_unique<FinalizationClaim>(p.id);
    p.rig.workers.run_all();
    competing.reset();
    FinalizationClaim newer(p.id);
    p.rig.ui.run_all();
    CHECK(p.engine->published().empty());
    CHECK(p.engine->allow_writes == 0);
    CHECK(p.engine->revoke_writes == 0);
    CHECK(std::ranges::none_of(p.rig.got.states,
                               [](const Json& state) { return state.contains("finalization"); }));
    CHECK(p.rig.rt->meeting_busy(p.id));
    CHECK(p.rig.got.statuses.back() == messages::kRetryBusy);
    CHECK_FALSE(reached);
    newer.release();
}

// from tests/test_meeting_runtime.py::test_progress_after_retry_returns_cannot_reopen_finished_run
TEST_CASE("test_progress_after_retry_returns_cannot_reopen_finished_run",
          "[test_meeting_runtime]") {
    Publication p;
    std::vector<std::function<void(const Json&)>> callbacks;
    p.rig.rerun = [&](const std::string&, const RerunRequest& request) {
        callbacks.push_back(request.progress);
        return retry_result();
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    const auto publications_before = p.engine->published().size();
    callbacks.at(0)(Json{{"status", "running"}, {"message", "Late progress"}});
    p.rig.ui.run_all();
    CHECK(p.engine->published().size() == publications_before);
    CHECK_FALSE(p.rig.rt->is_finalizing());
    CHECK(p.finalization_states().back()["status"] == "completed");
}

TEST_CASE("a retry says it is retrying and a failure without words is the bare sentence",
          "[test_meeting_runtime]") {
    // services/runtime/meeting.py retry_finalization (audit A04-82): the status line and the
    // running finalization read "Retrying post-meeting steps…"; a failed result with no message
    // or error reads RETRY_FAILED_MESSAGE.
    Publication p;
    p.rig.rerun = [](const std::string&, const RerunRequest&) {
        return Json{{"ok", false}};
    };
    on_ui(p.rig.ui, [&] { p.rig.rt->retry_insights(); });
    p.rig.drain();
    CHECK(std::ranges::find(p.rig.got.statuses, std::string(messages::kRetrying)) !=
          p.rig.got.statuses.end());
    const auto states = p.finalization_states();
    REQUIRE_FALSE(states.empty());
    CHECK(std::ranges::any_of(states, [](const Json& state) {
        return state["status"] == "running" && state["message"] == "Retrying post-meeting steps…";
    }));
    CHECK(states.back()["status"] == "failed");
    CHECK(states.back()["message"] == "Post-meeting retry failed.");
}
