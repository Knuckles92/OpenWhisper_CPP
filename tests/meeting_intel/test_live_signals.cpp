// tests/test_live_signals_lifecycle.py and the minute-worker, request-size and source-ranking
// tests of tests/test_meeting_fast_features.py: meeting/live_signals.py is LiveHighlights here.
// OpenWhisper's Queue / ManualExecutor is the worker's own thread, made to wait by a judge that
// holds its answer and observed through busy(); LiveSignals._run(minute) is a live check of the
// completed minute, and window_request / window_ops are reached through the judge's request and
// the published pulses. The judge is llm::FakeJudge marked device-local: FioriVoce sends meeting
// text to no other Fast judgments judge, so OpenWhisper's cloud consent has no counterpart and
// `allowed` and the feature switch are one Deps::enabled.

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <future>
#include <limits>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/live_highlights.h"
#include "meeting_state/fast_ops.h"
#include "meeting_state/schema.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/json.h"
#include "rows_repo.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using fv::llm::FakeJudge;
using fv::meeting_intel::LiveHighlights;
using fv::meeting_intel::test::eventually;
using fv::meeting_intel::test::RowsRepo;

Json row(std::string id, double start, std::string text = "The budget is 1000 dollars.",
         double length = 3) {
    return Json{
        {"id", std::move(id)}, {"start_s", start}, {"end_s", start + length}, {"text", text}};
}

std::shared_ptr<fv::meeting_state::MeetingStateStore> store(const std::string& status = "active") {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_test";
    state.cloud_enabled = true;
    state.status = status;
    return std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state));
}

std::string first_passage(const Json& state) {
    return state.at("passages").begin().key();
}

// The lifecycle fixture: one row at 8 s, a judge that finds a number in the first passage.
struct Signals {
    explicit Signals(std::vector<Json> rows = {row("first", 8)})
        : repo(std::make_shared<RowsRepo>(std::move(rows))) {
        judge->set_local(true);
        answer_number();
        worker = std::make_unique<LiveHighlights>(
            state, repo,
            LiveHighlights::Deps{.judge = [judge = judge] { return judge; },
                                 .enabled =
                                     [this] {
                                         return enabled.load();
                                     }});
    }
    ~Signals() { worker->shutdown(); }

    void answer_number() {
        judge->set_handler([](const Json& asked, const Json&) {
            return std::optional(Json{{"number", {{"noul", 0.95}}},
                                      {"number_anchor", {{"choice", first_passage(asked)}}}});
        });
    }
    void idle() const {
        REQUIRE(eventually([this] { return !worker->busy(); }));
    }
    std::vector<std::string> asked_first() const {
        std::vector<std::string> out;
        for (const auto& call : judge->calls()) {
            out.push_back(first_passage(call.state));
        }
        return out;
    }
    Json pulses() const { return state->snapshot().at("live_highlights"); }

    std::shared_ptr<RowsRepo> repo;
    std::shared_ptr<fv::meeting_state::MeetingStateStore> state = store();
    std::shared_ptr<FakeJudge> judge = std::make_shared<FakeJudge>();
    std::atomic<bool> enabled{true};
    std::unique_ptr<LiveHighlights> worker;
};

TEST_CASE("busy worker retains all completed minutes", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_busy_worker_retains_all_completed_minutes
    Signals s;
    s.repo->add(row("second", 75));
    s.repo->add(row("third", 125));
    std::promise<void> entered;
    std::promise<void> release;
    auto holding = entered.get_future();
    auto go = release.get_future().share();
    std::atomic<int> asks{0};
    s.judge->set_handler([&, go](const Json& asked, const Json&) {
        if (++asks == 1) {
            entered.set_value();
            go.wait_for(3s);
        }
        return std::optional(Json{{"number", {{"noul", 0.95}}},
                                  {"number_anchor", {{"choice", first_passage(asked)}}}});
    });
    s.worker->observe({}, 60);
    REQUIRE(holding.wait_for(3s) == std::future_status::ready);
    s.worker->observe({}, 180);
    release.set_value();
    s.idle();
    CHECK((s.asked_first() == std::vector<std::string>{"first", "second", "third"}));
    CHECK(s.pulses().size() == 3);
}

TEST_CASE("silence advances check and duplicate does not call again", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_silence_advances_check_and_duplicate_does_not_call_again
    Signals s;
    s.worker->observe({row("first", 8)}, 0);
    CHECK_FALSE(s.worker->busy());
    s.worker->observe({}, 61);
    s.idle();
    CHECK(s.judge->calls().size() == 1);
    s.worker->observe({row("first", 8)}, 0);
    s.idle();
    CHECK(s.judge->calls().size() == 1);
}

TEST_CASE("late channel rechecks the earlier minute", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_late_channel_rechecks_the_earlier_minute
    Signals s;
    s.worker->observe({}, 120);
    s.idle();
    const auto late = row("late", 30);
    s.repo->add(late);
    s.worker->observe({late}, 0);
    s.idle();
    const auto calls = s.judge->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls.back().state.at("passages").contains("late"));
    CHECK(s.pulses().size() == 1);
}

TEST_CASE("revised evidence is checked again and old pulse removed", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_revised_evidence_is_checked_again_and_old_pulse_removed
    const std::string kind = GENERATE("number", "takeaway");
    CAPTURE(kind);
    Signals s;
    s.judge->set_handler([kind](const Json&, const Json&) {
        return std::optional(
            Json{{kind, {{"noul", 0.95}}}, {kind + "_anchor", {{"choice", "first"}}}});
    });
    s.worker->observe({}, 60);
    s.idle();
    REQUIRE(s.pulses().size() == 1);
    CHECK(s.pulses()[0].at("kind") == kind);
    s.repo->set_text(0, "No budget was mentioned.");
    s.judge->set_handler(
        [kind](const Json&, const Json&) { return std::optional(Json{{kind, {{"noul", 0.1}}}}); });
    s.worker->observe({row("first", 8, "No budget was mentioned.")}, 0);
    s.idle();
    CHECK(s.judge->calls().size() == 2);
    CHECK(s.pulses().empty());
}

TEST_CASE("failed live request can be retried after a late arrival", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_failed_live_request_can_be_retried_after_a_late_arrival
    Signals s;
    s.judge->set_handler({});
    s.worker->observe({}, 60);
    s.idle();
    REQUIRE(s.judge->calls().size() == 1);
    CHECK(s.pulses().empty());
    s.answer_number();
    s.worker->observe({row("first", 8)}, 0);
    s.idle();
    CHECK(s.pulses().size() == 1);
}

TEST_CASE("final check includes short meeting and partial last minute", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_final_check_includes_short_meeting_and_partial_last_minute
    Signals s;
    s.repo->add(row("tail", 139));
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.worker->finalize();
    const auto pulses = s.pulses();
    REQUIRE(pulses.size() == 2);
    CHECK(pulses[0].at("start_s") == 8.0);
    CHECK(pulses[1].at("start_s") == 139.0);
    for (const auto& call : s.judge->calls()) {
        CHECK_FALSE(call.state.contains("known_questions"));
    }
    CHECK(s.state->snapshot().at("questions").empty());
    s.worker->finalize();
    CHECK(s.judge->calls().size() == 2);
}

TEST_CASE("final checks obey total request budget", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_final_checks_obey_total_request_budget
    // A real clock: the judge spends longer than the whole budget on the newest minute.
    Signals s;
    s.repo->add(row("second", 75));
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler([](const Json& asked, const Json&) {
        std::this_thread::sleep_for(200ms);
        return std::optional(Json{{"number", {{"noul", 0.95}}},
                                  {"number_anchor", {{"choice", first_passage(asked)}}}});
    });
    s.worker->finalize(0.15);
    const auto calls = s.judge->calls();
    REQUIRE(calls.size() == 1);
    REQUIRE(calls[0].timeout_s);
    CHECK(*calls[0].timeout_s <= 0.15);
    CHECK(*calls[0].timeout_s > 0.1);
}

TEST_CASE("final results are discarded if consent is revoked", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_final_results_are_discarded_if_consent_is_revoked
    // Both switches of the Python test (cloud consent, the feature) are Deps::enabled here.
    Signals s;
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler([&](const Json& asked, const Json&) {
        s.enabled = false;
        return std::optional(Json{{"number", {{"noul", 0.95}}},
                                  {"number_anchor", {{"choice", first_passage(asked)}}}});
    });
    s.worker->finalize();
    CHECK(s.judge->calls().size() == 1);
    CHECK(s.pulses().empty());
}

TEST_CASE("only final system pulses can write after end", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_only_final_system_pulses_can_write_after_end
    auto state = store("ended");
    const std::string actor(fv::meeting_state::LIVE_SIGNALS_ACTOR);
    Json op{{"op", "publish_highlights"}, {"pulses", Json::array()}, {"minute", 0}};
    CHECK_FALSE(state->apply("system", actor, Json::array({op}))[0].ok);
    op["final"] = true;
    CHECK_FALSE(state->apply("host", std::string("me"), Json::array({op}))[0].ok);
    CHECK(state->apply("system", actor, Json::array({op}))[0].ok);
    // A device-local judge needs no AI insights consent.
    REQUIRE(state->update_runtime_fields(Json{{"cloud_enabled", false}}));
    CHECK(state->apply("system", actor, Json::array({op}))[0].ok);
}

TEST_CASE("final pulses persist and broadcast", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_final_pulses_persist_and_broadcast
    const auto [kind, text] =
        GENERATE(std::pair<std::string, std::string>{"number", "The budget is 1000 dollars."},
                 std::pair<std::string, std::string>{
                     "takeaway", "We learned that early customer feedback prevents rework."});
    CAPTURE(kind);
    auto repo = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
    repo->create_meeting(Json{{"id", "m_final"},
                              {"title", "Test"},
                              {"status", "ended"},
                              {"started_at", "2026-09-19T20:00:00Z"},
                              {"host_token", "host"},
                              {"guest_token", "guest"},
                              {"cloud_enabled", true},
                              {"spool_dir", "unused"}});
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_final";
    segment.meeting_id = "m_final";
    segment.channel = "mic";
    segment.start_s = 8;
    segment.end_s = 12;
    segment.text = text;
    repo->add_segments({segment});
    fv::meeting_state::MeetingState document;
    document.meeting_id = "m_final";
    document.status = "ended";
    document.cloud_enabled = true;
    auto target = std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(document), repo);
    std::vector<fv::meeting_state::OpResult> events;
    auto subscription = target->subscribe([&](std::int64_t, const auto& results) {
        events.insert(events.end(), results.begin(), results.end());
    });
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->push(Json{{kind, {{"noul", 0.95}}},
                     {kind + "_anchor",
                      {{"choice", "sg_final"},
                       {"confidence", 0.7},
                       {"probabilities", {{"sg_final", 0.9}, {"none", 0.1}}}}}});
    LiveHighlights worker(target, repo,
                          {.judge = [judge] { return judge; },
                           .enabled =
                               [] {
                                   return true;
                               }});
    worker.finalize();
    const auto saved =
        fv::py::loads(repo->get_meeting("m_final")->at("state_json").get<std::string>());
    REQUIRE(saved.at("live_highlights").size() == 1);
    const auto& pulse = saved.at("live_highlights")[0];
    CHECK(pulse.at("segment_id") == "sg_final");
    CHECK(pulse.at("kind") == kind);
    CHECK(pulse.at("text") == text);
    REQUIRE(events.size() == 1);
    CHECK(events[0].effect.at("entity") == "live_highlights");
    const auto& assessment = pulse.at("assessment");
    CHECK(assessment.at("scores") == Json{{kind, 0.95}});
    CHECK(assessment.at("source_confidence") == 0.7);
    CHECK(assessment.at("source_probability") == 0.9);
    CHECK(assessment.at("source_rank") == 1);
    const auto& options = assessment.at("source_options");
    REQUIRE(options.size() == 2);
    CHECK(options[0].at("segment_id") == "sg_final");
    CHECK(options[0].at("probability") == 0.9);
    CHECK(options[0].at("start_s") == 8.0);
    CHECK(options[0].at("text") == text);
    CHECK(options[0].size() == 4);
    CHECK(options[1] == Json{{"segment_id", nullptr}, {"probability", 0.1}});
    CHECK(events[0].effect.at("pulses") == saved.at("live_highlights"));
    const auto restored = fv::meeting_state::MeetingState::from_json(saved).to_json();
    CHECK(restored.at("live_highlights") == saved.at("live_highlights"));
}

TEST_CASE("final check does not send in ineligible state", "[live_signals]") {
    // from tests/test_live_signals_lifecycle.py::test_final_check_does_not_send_in_ineligible_state
    const std::string status = GENERATE("active", "canceled", "needs_recovery");
    CAPTURE(status);
    Signals s;
    REQUIRE(s.state->update_runtime_fields(Json{{"status", status}}));
    s.worker->finalize();
    CHECK(s.judge->calls().empty());
}

TEST_CASE("shutdown during request discards result and pending windows", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_shutdown_during_request_discards_result_and_pending_windows
    // The judge cannot stop its own worker's thread, so a helper thread shuts the worker down
    // while the judgment is held, and the judge then answers.
    Signals s;
    s.repo->add(row("second", 75));
    std::thread stopper;
    s.judge->set_handler([&](const Json& asked, const Json&) {
        stopper = std::thread([&] { s.worker->shutdown(); });
        std::this_thread::sleep_for(100ms);
        return std::optional(Json{{"number", {{"noul", 0.95}}},
                                  {"number_anchor", {{"choice", first_passage(asked)}}}});
    });
    s.worker->observe({}, 120);
    REQUIRE(eventually([&] { return !s.judge->calls().empty(); }));
    s.idle();
    stopper.join();
    CHECK(s.judge->calls().size() == 1);
    CHECK(s.pulses().empty());
    CHECK_FALSE(s.worker->busy());
}

TEST_CASE("clearer definitions preserve threshold and real evidence", "[live_signals]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_clearer_definitions_preserve_threshold_and_real_evidence
    const std::string kind = GENERATE("number", "takeaway");
    const auto [probability, anchor, expected] =
        GENERATE(table<double, std::string, std::size_t>({{0.67, "first", 0},
                                                          {0.79, "first", 0},
                                                          {0.80, "first", 1},
                                                          {0.94, "first", 1},
                                                          {0.99, "missing", 0},
                                                          {0.99, "none", 0}}));
    CAPTURE(kind, probability, anchor);
    Signals s;
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler([=](const Json&, const Json&) {
        return std::optional(Json{{kind, {{"noul", probability}}},
                                  {kind + "_anchor", {{"choice", anchor}, {"confidence", 1.0}}}});
    });
    s.worker->finalize();
    CHECK(s.pulses().size() == expected);
}

// --- tests/test_meeting_fast_features.py ----------------------------------------------------

// The fast-features Repo: one decision row at 10 s.
Json fast_row(std::string id = "sg_one", std::string text = "We agree to ship the beta on Friday.",
              double start = 10, double end = 15) {
    return Json{
        {"id", std::move(id)}, {"text", std::move(text)}, {"start_s", start}, {"end_s", end}};
}

TEST_CASE("minute worker is nonblocking bounded and deduplicates", "[live_signals]") {
    // from
    // tests/test_meeting_fast_features.py::test_minute_worker_is_nonblocking_bounded_and_deduplicates
    Signals s({fast_row()});
    std::promise<void> entered;
    std::promise<void> release;
    auto holding = entered.get_future();
    auto go = release.get_future().share();
    std::atomic<int> asks{0};
    s.judge->set_handler([&, go](const Json&, const Json&) {
        if (++asks == 1) {
            entered.set_value();
            go.wait_for(3s);
        }
        return std::optional(Json{{"decision", {{"noul", 0.0}}}});
    });
    s.worker->observe({fast_row("sg_one", "We agree to ship the beta on Friday.", 10, 59)}, 0);
    CHECK_FALSE(s.worker->busy());
    s.worker->observe({fast_row("sg_one", "We agree to ship the beta on Friday.", 10, 60)}, 0);
    REQUIRE(holding.wait_for(3s) == std::future_status::ready);
    s.worker->observe({fast_row("sg_one", "We agree to ship the beta on Friday.", 10, 120)}, 0);
    release.set_value();
    s.idle();
    s.worker->observe({fast_row("sg_one", "We agree to ship the beta on Friday.", 10, 60)}, 0);
    s.idle();
    CHECK(s.judge->calls().size() == 1);
    s.worker->shutdown();
    s.worker->observe({fast_row("sg_one", "We agree to ship the beta on Friday.", 10, 180)}, 0);
    CHECK_FALSE(s.worker->busy());
    CHECK(s.judge->calls().size() == 1);
}

TEST_CASE("minute consent revoked in flight discards answers", "[live_signals]") {
    // from
    // tests/test_meeting_fast_features.py::test_minute_consent_revoked_in_flight_discards_answers
    Signals s({fast_row()});
    s.judge->set_handler([&](const Json&, const Json&) {
        s.enabled = false;
        return std::optional(
            Json{{"decision", {{"noul", 0.99}}}, {"decision_anchor", {{"choice", "sg_one"}}}});
    });
    s.worker->observe({}, 60);
    s.idle();
    CHECK(s.judge->calls().size() == 1);
    CHECK(s.pulses().empty());
}

TEST_CASE("minute disabled never calls remote", "[live_signals]") {
    // from tests/test_meeting_fast_features.py::test_minute_disabled_never_calls_remote
    // Both parametrizations (feature off, consent off) are Deps::enabled false; a judge that is
    // not on this computer is never asked either.
    Signals off({fast_row()});
    off.enabled = false;
    off.worker->observe({}, 60);
    off.idle();
    CHECK(off.judge->calls().empty());
    Signals hosted({fast_row()});
    hosted.judge->set_local(false);
    hosted.worker->observe({}, 60);
    hosted.idle();
    CHECK(hosted.judge->calls().empty());
}

TEST_CASE("minute discards source changed during request", "[live_signals]") {
    // from tests/test_meeting_fast_features.py::test_minute_discards_source_changed_during_request
    Signals s({fast_row()});
    s.judge->set_handler([&](const Json&, const Json&) {
        s.repo->set_text(0, "No decision was made.");
        return std::optional(
            Json{{"decision", {{"noul", 0.99}}}, {"decision_anchor", {{"choice", "sg_one"}}}});
    });
    s.worker->observe({}, 60);
    s.idle();
    CHECK(s.judge->calls().size() == 1);
    CHECK(s.pulses().empty());
}

TEST_CASE("request budget with dense minute and full inbox", "[live_signals]") {
    // from
    // tests/test_meeting_fast_features.py::test_request_budget_with_dense_minute_and_full_inbox
    std::vector<Json> rows;
    std::string filler;
    for (int i = 0; i < 5; ++i) {
        filler += "A useful sentence. ";
    }
    for (int i = 0; i < 30; ++i) {
        char id[32];
        std::snprintf(id, sizeof id, "sg_%012d", i);
        rows.push_back(fast_row(id, "Who owns the next release? " + filler, i * 3, i * 3 + 2));
    }
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    auto repo = std::make_shared<RowsRepo>(rows);
    auto state = store();
    for (int i = 0; i < 7; ++i) {
        REQUIRE(state
                    ->apply("host", std::string("me"),
                            Json::array({Json{{"op", "ask_question"},
                                              {"text", "Who owns the product launch and when "
                                                       "will it happen? (" +
                                                           std::to_string(i) + ")"}}}))[0]
                    .ok);
    }
    LiveHighlights worker(state, repo,
                          {.judge = [judge] { return judge; },
                           .enabled = [] { return true; },
                           .radar_enabled =
                               [] {
                                   return true;
                               }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].state.at("known_questions").size() == 7);
    CHECK(calls[0].state.at("passages").size() == 16);
    CHECK(calls[0].questions.contains("candidate_0"));
    const auto request =
        Json{{"state", calls[0].state}, {"questions", calls[0].questions}, {"model", "jev-1.13.0"}};
    // services.typesafe.MAX_STATE_CHARS
    CHECK(fv::py::dumps(request).size() < 24'000);
}

TEST_CASE("source ranking uses complete distribution and handles ties", "[live_signals]") {
    // from
    // tests/test_meeting_fast_features.py::test_source_ranking_uses_complete_distribution_and_handles_ties
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const auto [probabilities, rank] =
        GENERATE_COPY(table<Json, int>({{Json{{"sg_one", 0.5}, {"none", 0.5}}, 1},
                                        {Json{{"sg_one", 0.4}, {"none", 0.6}}, 2},
                                        {Json{{"sg_one", 0.9}}, 0},
                                        {Json{{"sg_one", 0.9}, {"none", 0.1}, {"unasked", 0}}, 0},
                                        {Json{{"sg_one", nan}, {"none", 0.1}}, 0},
                                        {Json{{"sg_one", true}, {"none", 0}}, 0}}));
    CAPTURE(probabilities.dump());
    Signals s({fast_row("sg_one", "We agree to ship the beta on Friday.", 130, 135)});
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler([probabilities](const Json&, const Json&) {
        return std::optional(
            Json{{"number", {{"noul", 0.8}}},
                 {"number_anchor",
                  {{"choice", "sg_one"}, {"confidence", 0}, {"probabilities", probabilities}}}});
    });
    s.worker->finalize();
    REQUIRE(s.pulses().size() == 1);
    const auto assessment = s.pulses()[0].at("assessment");
    CHECK(assessment.at("source_confidence") == 0);
    CHECK(assessment.at("scores") == Json{{"number", 0.8}});
    CHECK(assessment.at("window_start_s") == 120);
    CHECK(assessment.at("window_end_s") == 180);
    if (rank == 0) {
        CHECK_FALSE(assessment.contains("source_rank"));
        CHECK_FALSE(assessment.contains("source_probability"));
        CHECK_FALSE(assessment.contains("source_option_count"));
        CHECK_FALSE(assessment.contains("source_options"));
    } else {
        CHECK(assessment.at("source_rank") == rank);
        Json seen = Json::object();
        for (const auto& option : assessment.at("source_options")) {
            seen[option.at("segment_id").is_null() ? "none"
                                                   : option.at("segment_id").get<std::string>()] =
                option.at("probability");
        }
        CHECK(seen == probabilities);
    }
}

TEST_CASE("source options save every evaluated passage in full", "[live_signals]") {
    // from
    // tests/test_meeting_fast_features.py::test_source_options_save_every_evaluated_passage_in_full
    std::vector<Json> rows;
    for (int i = 0; i < 17; ++i) {
        rows.push_back(
            fast_row("sg_" + std::to_string(i), "Passage " + std::to_string(i), i * 3, i * 3 + 2));
    }
    std::string long_text;
    for (int i = 0; i < 10; ++i) {
        long_text += "The budget is one thousand dollars. ";
    }
    rows[0]["text"] = long_text;
    Signals s(rows);
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler([](const Json& asked, const Json&) {
        Json probabilities = Json::object();
        for (const auto& [id, passage] : asked.at("passages").items()) {
            probabilities[id] = id == "sg_0" ? 0.99 : 0.000625;
        }
        probabilities["none"] = 0.000625;
        return std::optional(
            Json{{"number", {{"noul", 0.94}}},
                 {"number_anchor", {{"choice", "sg_0"}, {"probabilities", probabilities}}}});
    });
    s.worker->finalize();
    const auto passages = s.judge->calls().at(0).state.at("passages");
    CHECK_FALSE(passages.contains("sg_16"));
    REQUIRE(s.pulses().size() == 1);
    const auto pulse = s.pulses()[0];
    const auto& options = pulse.at("assessment").at("source_options");
    CHECK(options.size() == 17);
    CHECK(pulse.at("assessment").at("source_option_count") == 17);
    // The whole passage, stripped as the request carried it.
    CHECK(options[0].at("text") == long_text.substr(0, long_text.size() - 1));
    CHECK(options[0].at("text").get<std::string>().size() >
          pulse.at("text").get<std::string>().size());
    std::set<std::string> ids;
    for (const auto& option : options) {
        ids.insert(option.at("segment_id").is_null() ? "<none>"
                                                     : option.at("segment_id").get<std::string>());
    }
    std::set<std::string> expected{"<none>"};
    for (const auto& [id, passage] : passages.items()) {
        expected.insert(id);
    }
    CHECK(ids == expected);
}

// --- tests/test_text_judge.py ---------------------------------------------------------------

TEST_CASE("final highlight pass goes newest first with the judges budget", "[live_signals]") {
    // from
    // tests/test_text_judge.py::test_final_highlight_pass_goes_newest_first_with_the_judges_budget
    std::vector<Json> rows;
    for (int m = 0; m < 3; ++m) {
        rows.push_back(fast_row("sg_" + std::to_string(m),
                                "Minute " + std::to_string(m) + " words here.", m * 60 + 5,
                                m * 60 + 9));
    }
    Signals s(rows);
    REQUIRE(s.state->update_runtime_fields(Json{{"status", "ended"}}));
    s.judge->set_handler({}); // every judgment fails, so each minute is asked once
    s.worker->finalize(30.0);
    const auto calls = s.judge->calls();
    CHECK((s.asked_first() == std::vector<std::string>{"sg_2", "sg_1", "sg_0"}));
    for (const auto& call : calls) {
        REQUIRE(call.timeout_s);
        CHECK(*call.timeout_s > 20.0);
        CHECK(*call.timeout_s <= 30.0);
    }
}

// --- tests/test_typesafe_meeting_runtime.py ---------------------------------------------------

TEST_CASE("a judge whose construction failed is looked up again", "[live_signals]") {
    // from tests/test_typesafe_meeting_runtime.py::test_judge_retries_initialization_failure
    // The judge is looked up per judgment, so a failed construction is retried next time.
    auto repo = std::make_shared<RowsRepo>(std::vector<Json>{row("first", 8)});
    auto state = store();
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json& asked, const Json&) {
        return std::optional(Json{{"number", {{"noul", 0.95}}},
                                  {"number_anchor", {{"choice", first_passage(asked)}}}});
    });
    std::atomic<int> lookups{0};
    LiveHighlights worker(state, repo,
                          {.judge = [&]() -> std::shared_ptr<fv::llm::Judge> {
                               if (++lookups == 1) {
                                   throw std::runtime_error("temporary failure");
                               }
                               return judge;
                           },
                           .enabled =
                               [] {
                                   return true;
                               }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    CHECK(lookups.load() == 1);
    CHECK(state->snapshot().at("live_highlights").empty());
    worker.observe({row("first", 8)}, 0);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    CHECK(lookups.load() == 2);
    CHECK(state->snapshot().at("live_highlights").size() == 1);
}

} // namespace

TEST_CASE("the question radar needs a whole question word at the start", "[live_signals]") {
    // live_signals._QUESTION_START is (?i)^(who|...|are)\b (audit A29-19): "Issues", "Canada"
    // and "Doing" start no question, "Is" and "can" do.
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    auto repo = std::make_shared<RowsRepo>(std::vector<Json>{
        fast_row("sg_000000000001",
                 "Issues remain with the vendor contract. Canada joins the launch next week. "
                 "Doing the review takes a day.",
                 0, 4),
        fast_row("sg_000000000002", "Is the vendor contract signed yet. can we ship on Monday", 5,
                 9)});
    auto state = store();
    LiveHighlights worker(state, repo,
                          {.judge = [judge] { return judge; },
                           .enabled = [] { return true; },
                           .radar_enabled =
                               [] {
                                   return true;
                               }});
    worker.observe({}, 60);
    REQUIRE(eventually([&] { return !worker.busy(); }));
    worker.shutdown();
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 1);
    const Json candidates = calls[0].state.value("candidates", Json::object());
    std::vector<std::string> texts;
    for (const auto& [key, candidate] : candidates.items()) {
        texts.push_back(candidate.at("text").get<std::string>());
    }
    CHECK(texts ==
          std::vector<std::string>{"Is the vendor contract signed yet.", "can we ship on Monday"});
}
