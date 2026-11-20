// tests/test_meeting_custom_report.py (TestCorpus, TestGeneration, TestLifecycle) and the two
// report tests of tests/test_installed_agent_core.py. Python replaces services.text_generation's
// generate with scripted turns; here the text endpoint is an SseProvider on 127.0.0.1 serving
// the canned Chat Completions replies of tests/fixtures/llm/reports/, so the whole request path
// runs. The installed agent is a fake run_agent_task, as Python patches it.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "agents/drivers.h"
#include "meeting_intel/history_source.h"
#include "meeting_state/custom_report_ops.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/text.h"
#include "report_support.h"

#if defined(_WIN32)
#include <windows.h>
#endif

using namespace fv::meeting_intel;
using namespace fv::meeting_intel::test;
using fv::Json;
using fv::testing::SseReply;

namespace {

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

std::vector<SseReply> replies(const Json& bodies, std::size_t repeat = 1) {
    std::vector<SseReply> out;
    for (std::size_t i = 0; i < repeat; ++i) {
        for (const auto& body : bodies) {
            out.push_back(SseReply::json(200, body));
        }
    }
    return out;
}

std::vector<SseReply> fixture(std::string_view name, std::size_t repeat = 1) {
    return replies(report_fixture(name), repeat);
}

std::vector<Json> tool_messages(const Json& body) {
    std::vector<Json> out;
    for (const auto& message : body.at("messages")) {
        if (message.value("role", "") == "tool") {
            out.push_back(message);
        }
    }
    return out;
}

std::set<std::string> tool_names(const Json& body) {
    std::set<std::string> names;
    for (const auto& tool : body.at("tools")) {
        names.insert(tool.at("function").at("name").get<std::string>());
    }
    return names;
}

Json brief_result() {
    return Json{{"markdown", "# Brief\n\nbody"},
                {"title", "Brief"},
                {"sources", Json{{"transcript_lines", 4}}}};
}

// Waits until `ready` holds, up to five seconds.
template <class Ready> bool eventually(Ready ready) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < until) {
        if (ready()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return ready();
}

} // namespace

// ---- TestCorpus ----

TEST_CASE("TestCorpus::test_lines_carry_the_clock_and_the_speaker",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_lines_carry_the_clock_and_the_speaker
    const TranscriptCorpus corpus(report_segments(), Json::object());
    CHECK(corpus.lines()[0].starts_with("[0:00] Me: Lets talk"));
    CHECK(corpus.lines()[1].starts_with("[0:06] Others: We cannot"));
    CHECK(corpus.duration_s() == 28.0);
    CHECK(corpus.size() == 4);
}

TEST_CASE("TestCorpus::test_named_participants_replace_the_channel_label",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_named_participants_replace_the_channel_label
    Json row = report_segments()[0];
    row["speaker_participant_id"] = "p_1";
    const Json participants = {{"p_1", {{"display_name", "Priya"}}}};
    CHECK(contains(TranscriptCorpus({row}, participants).lines()[0], "Priya:"));
}

TEST_CASE("TestCorpus::test_empty_lines_are_dropped", "[test_meeting_custom_report]") {
    // from tests/test_meeting_custom_report.py::TestCorpus::test_empty_lines_are_dropped
    std::vector<Json> rows = report_segments();
    rows.push_back(report_segment(9, 40, "   "));
    CHECK(TranscriptCorpus(rows, Json::object()).size() == report_segments().size());
}

TEST_CASE("TestCorpus::test_search_ranks_by_overlap_and_returns_time_order",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_search_ranks_by_overlap_and_returns_time_order
    const TranscriptCorpus corpus(report_segments(), Json::object());
    const std::string hits = corpus.search("renewal terms");
    REQUIRE(contains(hits, "[0:00]"));
    REQUIRE(contains(hits, "[0:24]"));
    CHECK(hits.find("[0:00]") < hits.find("[0:24]"));
    CHECK(contains(corpus.search("submarine"), "No transcript lines match"));
    CHECK(corpus.search("  ") == "Provide search terms.");
}

TEST_CASE("TestCorpus::test_a_query_of_only_stop_words_asks_for_real_terms",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_a_query_of_only_stop_words_asks_for_real_terms
    CHECK(TranscriptCorpus(report_segments(), Json::object()).search("the and") ==
          "Provide search terms.");
}

TEST_CASE("TestCorpus::test_window_reads_a_stretch_and_reports_an_empty_one",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_window_reads_a_stretch_and_reports_an_empty_one
    const TranscriptCorpus corpus(report_segments(), Json::object());
    const std::string window = corpus.window(6, 15);
    CHECK(contains(window, "twelve percent"));
    CHECK_FALSE(contains(window, "Q3"));
    // Reversed bounds are a typo, not an error.
    CHECK(corpus.window(15, 6) == window);
    CHECK(contains(corpus.window(500, 600), "No transcript between"));
}

TEST_CASE("TestCorpus::test_a_long_transcript_keeps_its_edges_and_marks_the_gap",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_a_long_transcript_keeps_its_edges_and_marks_the_gap
    std::vector<Json> rows;
    for (int i = 0; i < 600; ++i) {
        rows.push_back(report_segment(i, i * 5.0, std::format("line number {} about budget", i)));
    }
    const TranscriptCorpus corpus(rows, Json::object());
    const std::string edges = corpus.render_edges(400);
    CHECK(contains(edges, "line number 0 "));
    CHECK(contains(edges, "line number 599 "));
    CHECK(contains(edges, "lines from"));
    CHECK(contains(edges, "search_transcript"));
    CHECK(fv::py::len(edges) < corpus.total_chars());
}

TEST_CASE("TestCorpus::test_the_digest_carries_qualifiers_the_report_must_not_drop",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_the_digest_carries_qualifiers_the_report_must_not_drop
    auto state = ended_state();
    state.rolling_summary = "The team held pricing at twelve percent.";
    state.topic.current = "Vendor renewal";
    Json snapshot = state.to_json();
    snapshot["participants"] = Json{{"p_1", {{"id", "p_1"}, {"display_name", "Priya"}}}};
    snapshot["cards"]["action_items"] =
        Json::array({Json{{"id", "it_1"},
                          {"card", "action_items"},
                          {"text", "Draft the renewal note"},
                          {"status", "proposed"},
                          {"evidence", Json::array()},
                          {"data", {{"owner_participant_id", "p_1"}, {"deadline", "Friday"}}},
                          {"review", {{"state", "provisional"}}}}});
    snapshot["cards"]["decisions"] = Json::array({Json{{"id", "it_2"},
                                                       {"card", "decisions"},
                                                       {"text", "Removed decision"},
                                                       {"status", "removed"},
                                                       {"data", Json::object()},
                                                       {"evidence", Json::array()}}});
    snapshot["questions"] =
        Json::array({Json{{"id", "q_1"}, {"text", "What is the term length?"}, {"status", "open"}},
                     Json{{"id", "q_2"},
                          {"text", "Who signs?"},
                          {"status", "resolved"},
                          {"answer", "Legal does."}}});
    const std::string digest = build_digest(report_meeting(), snapshot,
                                            TranscriptCorpus(report_segments(), Json::object()));
    CHECK(contains(digest, "Vendor renewal"));
    CHECK(contains(digest, "Priya"));
    CHECK(contains(digest, "owner: Priya"));
    CHECK(contains(digest, "due: Friday"));
    CHECK(contains(digest, "provisional"));
    CHECK(contains(digest, "What is the term length?"));
    CHECK(contains(digest, "Legal does."));
    CHECK_FALSE(contains(digest, "Removed decision"));
}

TEST_CASE("TestCorpus::test_the_prompt_pastes_a_short_transcript_whole",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_the_prompt_pastes_a_short_transcript_whole
    const std::string prompt = build_corpus_prompt(
        report_meeting(), ended_state().to_json(),
        TranscriptCorpus(report_segments(), Json::object()), "every commitment we made");
    CHECK(contains(prompt, "every commitment we made"));
    CHECK(contains(prompt, "FULL TRANSCRIPT (complete)"));
    CHECK(contains(prompt, "twelve percent discount"));
    CHECK(contains(prompt, "The whole transcript is above."));
}

TEST_CASE("TestCorpus::test_the_prompt_points_a_long_meeting_at_the_read_tools",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_the_prompt_points_a_long_meeting_at_the_read_tools
    std::vector<Json> rows;
    for (int i = 0; i < 4000; ++i) {
        rows.push_back(
            report_segment(i, i * 5.0, std::format("line number {} about the budget review", i)));
    }
    const std::string prompt =
        build_corpus_prompt(report_meeting(), ended_state().to_json(),
                            TranscriptCorpus(rows, Json::object()), "a brief");
    CHECK(contains(fv::py::lower(prompt), "use search_transcript and read_transcript"));
    CHECK_FALSE(contains(prompt, "FULL TRANSCRIPT (complete)"));
}

TEST_CASE("TestCorpus::test_an_empty_meeting_says_so_instead_of_inviting_invention",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_an_empty_meeting_says_so_instead_of_inviting_invention
    const std::string prompt = build_corpus_prompt(report_meeting(), ended_state().to_json(),
                                                   TranscriptCorpus({}, Json::object()), "a brief");
    CHECK(contains(fv::py::lower(prompt), "no transcript text"));
}

TEST_CASE("TestCorpus::test_the_title_comes_from_the_document_then_the_request",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestCorpus::test_the_title_comes_from_the_document_then_the_request
    const auto [markdown, expected] = GENERATE(table<std::string, std::string>({
        {"# Pricing brief\n\nbody", "Pricing brief"},
        {"\n\n#  Spaced  \n", "Spaced"},
        {"no heading here", "summarize the pricing"},
    }));
    CHECK(report_title(markdown, "summarize the pricing") == expected);
}

// ---- TestGeneration ----

TEST_CASE("TestGeneration::test_one_turn_becomes_the_report", "[test_meeting_custom_report]") {
    // from tests/test_meeting_custom_report.py::TestGeneration::test_one_turn_becomes_the_report
    ReportHarness harness(fixture("one_turn"));
    const Json result = harness.run();
    CHECK(result.at("markdown") == "# Commitments\n\n- Hold at 12%");
    CHECK(result.at("title") == "Commitments");
    CHECK(result.at("usage").at("total_tokens") == 15);
    CHECK(result.at("sources").at("transcript_lines") == 4);
    CHECK(result.at("sources").at("transcript_complete") == true);
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 1);
    CHECK(contains(bodies[0].at("messages").at(1).at("content").get<std::string>(),
                   "every commitment"));
}

TEST_CASE("TestGeneration::test_read_tools_run_and_their_results_come_back",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_read_tools_run_and_their_results_come_back
    ReportHarness harness(fixture("read_then_write"));
    const Json result = harness.run();
    CHECK(result.at("markdown").get<std::string>().starts_with("# Brief"));
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 2);
    const auto tools = tool_messages(bodies[1]);
    REQUIRE(tools.size() == 2);
    CHECK(contains(tools[0].at("content").get<std::string>(), "twelve percent discount"));
    CHECK(contains(tools[1].at("content").get<std::string>(), "[0:06]"));
    CHECK(result.at("sources").at("tools_used") ==
          Json{{"read_transcript", 1}, {"search_transcript", 1}});
}

TEST_CASE("TestGeneration::test_a_failing_tool_does_not_sink_the_report",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_a_failing_tool_does_not_sink_the_report
    ReportHarness harness(fixture("failing_tools"));
    const Json result = harness.run();
    CHECK(result.at("markdown").get<std::string>().starts_with("# Brief"));
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 2);
    std::vector<std::string> contents;
    for (const auto& message : tool_messages(bodies[1])) {
        contents.push_back(message.at("content").get<std::string>());
    }
    CHECK(std::ranges::any_of(contents, [](const auto& t) { return contains(t, "Unknown tool"); }));
    CHECK(std::ranges::any_of(contents,
                              [](const auto& t) { return contains(t, "Could not parse"); }));
}

TEST_CASE("TestGeneration::test_the_last_round_takes_the_tools_away_and_asks_for_the_report",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_the_last_round_takes_the_tools_away_and_asks_for_the_report
    ReportHarness harness(fixture("keeps_reading", kReportMaxToolRounds));
    const Json result = harness.run();
    CHECK(result.at("markdown").get<std::string>().starts_with("# Partial"));
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == static_cast<std::size_t>(kReportMaxToolRounds));
    CHECK(bodies.front().contains("tools"));
    CHECK_FALSE(bodies.back().contains("tools"));
    CHECK(contains(bodies.back().at("messages").back().at("content").get<std::string>(),
                   "Stop reading and write the report now"));
}

TEST_CASE("TestGeneration::test_a_wrapping_code_fence_is_stripped",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_a_wrapping_code_fence_is_stripped
    ReportHarness harness(replies(Json::array({chat_reply("```markdown\n# Brief\n\nbody\n```")})));
    CHECK(harness.run().at("markdown") == "# Brief\n\nbody");
}

TEST_CASE("TestGeneration::test_an_empty_answer_is_reported_not_stored",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_an_empty_answer_is_reported_not_stored
    ReportHarness harness(replies(Json::array({chat_reply("   ")})));
    CHECK(contains(unavailable([&] { harness.run(); }), "came back empty"));
}

TEST_CASE("TestGeneration::test_a_transport_failure_becomes_a_readable_message",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_a_transport_failure_becomes_a_readable_message
    // Python's generate raises RuntimeError; here the endpoint refuses the request (a status the
    // client does not retry), which is not a TextGenerationError either.
    ReportHarness harness({SseReply::json(401, Json{{"error", {{"message", "bad key"}}}})});
    CHECK(contains(unavailable([&] { harness.run("a brief"); }), "could not be reached"));
}

TEST_CASE("TestGeneration::test_the_request_is_validated_before_any_model_call",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_the_request_is_validated_before_any_model_call
    const std::string request =
        GENERATE(std::string(), std::string("   "),
                 std::string(fv::meeting_state::MAX_REQUEST_CHARS + 1, 'x'));
    ReportHarness harness(fixture("one_turn"));
    CHECK_THROWS_AS(harness.run(request), ReportUnavailable);
    CHECK(harness.provider.requests().empty());
}

TEST_CASE("TestGeneration::test_past_meeting_recall_is_offered_only_when_it_is_enabled",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestGeneration::test_past_meeting_recall_is_offered_only_when_it_is_enabled
    {
        ReportHarness harness(fixture("one_turn"), true);
        harness.consent(false, false);
        const Json result = harness.run();
        CHECK(tool_names(harness.bodies().at(0)) ==
              std::set<std::string>{"search_transcript", "read_transcript"});
        CHECK(result.at("sources").at("past_meetings") == false);
    }
    {
        ReportHarness harness(fixture("one_turn"), true);
        harness.consent(true, true);
        const Json result = harness.run();
        const auto names = tool_names(harness.bodies().at(0));
        CHECK(names.contains("search_past_meetings"));
        CHECK(names.contains("search_context_files"));
        CHECK(result.at("sources").at("past_meetings") == true);
    }
}

// ---- TestLifecycle ----

TEST_CASE("TestLifecycle::test_a_successful_run_publishes_through_the_store",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestLifecycle::test_a_successful_run_publishes_through_the_store
    StoreFixture fixture_store;
    ReportHarness harness;
    InlineExecutor inline_worker;
    Json captured;
    harness.deps.worker = &inline_worker;
    harness.deps.generator = [&](const Json& meeting, const Json&,
                                 const std::vector<Json>& segments, std::string_view request,
                                 const ReportRoute& route, double) {
        captured = Json{{"request", request},
                        {"segments", segments.size()},
                        {"meeting", meeting.at("id")},
                        {"provider", route.provider}};
        return brief_result();
    };
    const Json result = start_custom_report(fixture_store.store, harness.deps, "every commitment",
                                            ReportRoute{"openrouter", "m", Json()}, "");
    CHECK(result.at("ok") == true);
    CHECK(captured == Json{{"request", "every commitment"},
                           {"segments", 4},
                           {"meeting", kPastId},
                           {"provider", "openrouter"}});
    const Json report = fixture_store.reports().at(0);
    CHECK(report.at("status") == "ready");
    CHECK(report.at("title") == "Brief");
}

TEST_CASE("a cancelled report worker settles its claim without model input",
          "[test_meeting_custom_report]") {
    StoreFixture fixture_store;
    ReportRepository repository;
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    fv::CredentialStore credentials(backend, fv::identity());
    fv::MapEnvReader env;
    fv::DotEnv dotenv(std::filesystem::path("fv-meeting-intel-tests-no-such-dir") / ".env");
    ReportDeps deps{.repository = repository, .keys = {credentials, env, dotenv}};
    InlineExecutor worker;
    fv::CancelSource stop;
    deps.worker = &worker;
    deps.cancel = stop.token();
    const bool withdrawn = GENERATE(false, true);
    if (withdrawn) {
        deps.destination_allowed = [] {
            return false;
        };
    } else {
        stop.cancel();
    }
    int called = 0;
    deps.generator = [&](const Json&, const Json&, const std::vector<Json>&, std::string_view,
                         const ReportRoute&, double) {
        ++called;
        return brief_result();
    };
    CHECK(start_custom_report(fixture_store.store, deps, "a brief",
                              ReportRoute{"ollama", "m", Json()}, "")
              .at("ok") == true);
    CHECK(called == 0);
    const Json report = fixture_store.reports().at(0);
    CHECK(report.at("status") == "failed");
    CHECK(report.at("message") == "Report writing was stopped. Ask for it again when ready.");
}

TEST_CASE("TestLifecycle::test_a_rejected_claim_never_reaches_the_model",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestLifecycle::test_a_rejected_claim_never_reaches_the_model
    fv::meeting_state::MeetingState live;
    live.meeting_id = std::string(kPastId);
    live.status = "active";
    StoreFixture fixture_store(live);
    ReportHarness harness;
    InlineExecutor inline_worker;
    int called = 0;
    harness.deps.worker = &inline_worker;
    harness.deps.generator = [&](const Json&, const Json&, const std::vector<Json>&,
                                 std::string_view, const ReportRoute&, double) {
        ++called;
        return brief_result();
    };
    const Json result = start_custom_report(fixture_store.store, harness.deps, "a brief",
                                            ReportRoute{"openrouter", "m", Json()}, "");
    CHECK(result == Json{{"ok", false}, {"report_id", nullptr}, {"error", "meeting_not_ready"}});
    CHECK(called == 0);
}

TEST_CASE("TestLifecycle::test_a_failing_pass_leaves_a_readable_failure_not_a_stuck_record",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestLifecycle::test_a_failing_pass_leaves_a_readable_failure_not_a_stuck_record
    const bool readable = GENERATE(true, false);
    StoreFixture fixture_store;
    ReportHarness harness;
    InlineExecutor inline_worker;
    harness.deps.worker = &inline_worker;
    harness.deps.generator = [readable](const Json&, const Json&, const std::vector<Json>&,
                                        std::string_view, const ReportRoute&, double) -> Json {
        if (readable) {
            throw ReportUnavailable("No API key is configured.");
        }
        throw std::runtime_error("boom");
    };
    start_custom_report(fixture_store.store, harness.deps, "a brief",
                        ReportRoute{"openrouter", "m", Json()}, "");
    const Json report = fixture_store.reports().at(0);
    CHECK(report.at("status") == "failed");
    CHECK(contains(report.at("message").get<std::string>(),
                   readable ? "No API key is configured." : "The report could not be written."));
    // The slot is free again.
    CHECK(fixture_store.request("host", "run_after").ok);
}

TEST_CASE("TestLifecycle::test_a_run_whose_report_was_discarded_publishes_nothing",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestLifecycle::test_a_run_whose_report_was_discarded_publishes_nothing
    StoreFixture fixture_store;
    const std::string report_id = fixture_store.request().target_id.value_or("");
    fixture_store.store->apply(
        "host", std::nullopt,
        Json::array({Json{{"op", "remove_custom_report"}, {"report_id", report_id}}}));
    ReportHarness harness;
    harness.deps.generator = [](const Json&, const Json&, const std::vector<Json>&,
                                std::string_view, const ReportRoute&, double) {
        return Json{{"markdown", "# late"}};
    };
    run_custom_report(fixture_store.store, harness.deps, report_id, "run_1", "a brief",
                      ReportRoute{"openrouter", "m", Json()});
    CHECK(fixture_store.reports() == Json::array());
}

TEST_CASE("TestLifecycle::test_the_worker_runs_off_the_caller_thread",
          "[test_meeting_custom_report]") {
    // from
    // tests/test_meeting_custom_report.py::TestLifecycle::test_the_worker_runs_off_the_caller_thread
    StoreFixture fixture_store;
    ReportHarness harness;
    std::mutex mutex;
    std::condition_variable done;
    std::optional<std::thread::id> worker;
    std::wstring name;
    harness.deps.generator = [&](const Json&, const Json&, const std::vector<Json>&,
                                 std::string_view, const ReportRoute&, double) {
#if defined(_WIN32)
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(GetCurrentThread(), &description))) {
            name = description;
            LocalFree(description);
        }
#endif
        {
            std::lock_guard lock(mutex);
            worker = std::this_thread::get_id();
        }
        done.notify_all();
        return Json{{"markdown", "# Brief"}, {"title", "Brief"}, {"sources", Json::object()}};
    };
    CHECK(start_custom_report(fixture_store.store, harness.deps, "a brief",
                              ReportRoute{"openrouter", "m", Json()}, "")
              .at("ok") == true);
    {
        std::unique_lock lock(mutex);
        REQUIRE(done.wait_for(lock, std::chrono::seconds(5), [&] { return worker.has_value(); }));
        CHECK(*worker != std::this_thread::get_id());
    }
#if defined(_WIN32)
    CHECK(name == L"fv-meeting-custom-report");
#endif
    // The worker publishes after the generator returns: wait for it before the store goes.
    CHECK(eventually([&] { return fixture_store.reports().at(0).at("status") == "ready"; }));
    // ... and for the worker to let go of the store, whose clock and ids the fixture owns.
    CHECK(eventually([&] { return fixture_store.store.use_count() == 1; }));
}

// ---- test_installed_agent_core.py: custom reports ----

TEST_CASE("a report through an installed agent", "[test_installed_agent_core]") {
    // from tests/test_installed_agent_core.py::test_a_report_through_an_installed_agent
    ReportHarness harness;
    Json seen;
    harness.deps.agent_task = [&](std::string_view agent_id, std::string_view model,
                                  std::string_view, std::string_view, const Json& tools,
                                  fv::agents::ToolHandler handler, double timeout_s,
                                  std::string_view closing, const fv::CancelToken&) {
        seen = Json{{"agent_id", agent_id},
                    {"model", model},
                    {"closing", closing},
                    {"tools", tools},
                    {"timeout_s", timeout_s}};
        const auto [text, error] =
            handler("search_transcript", Json{{"query", "beta"}}, Json::object());
        CHECK(contains(fv::py::lower(text), "beta"));
        CHECK_FALSE(error);
        return fv::agents::PassOutcome{.ok = true,
                                       .text = "# Exec brief\n\nShip October 12.",
                                       .usage = Json{{"total_tokens", 9}}};
    };
    const std::vector<Json> segments = {Json{{"id", "sg_1"},
                                             {"start_s", 1.0},
                                             {"text", "We ship the beta October 12."},
                                             {"channel", "mic"}}};
    const Json result =
        generate_report(Json{{"id", "m1"}, {"title", "Sync"}}, Json{{"meeting_id", "m1"}}, segments,
                        "A brief", ReportRoute{"claude_code", "haiku", Json()}, harness.deps);
    CHECK(result.at("title") == "Exec brief");
    CHECK(result.at("sources").at("provider") == "claude_code");
    CHECK(result.at("sources").at("tools_used") == Json{{"search_transcript", 1}});
    CHECK(contains(seen.at("closing").get<std::string>(), "finished report"));
    CHECK(seen.at("agent_id") == "claude_code");
    CHECK(seen.at("tools") == detail::report_mcp_tools(false, false));
    CHECK(seen.at("timeout_s") == kReportDefaultTimeoutS);
    CHECK(result.at("usage") == Json{{"total_tokens", 9}});
}

TEST_CASE("a report from a missing agent says so", "[test_installed_agent_core]") {
    // from tests/test_installed_agent_core.py::test_a_report_from_a_missing_agent_says_so
    ReportHarness harness;
    harness.deps.agent_task = [](std::string_view, std::string_view, std::string_view,
                                 std::string_view, const Json&, fv::agents::ToolHandler, double,
                                 std::string_view,
                                 const fv::CancelToken&) -> fv::agents::PassOutcome {
        throw fv::agents::AgentUnavailable("Codex is not installed on this computer.");
    };
    CHECK(contains(unavailable([&] {
                       generate_report(Json{{"id", "m1"}}, Json::object(), {}, "A brief",
                                       ReportRoute{"codex", "", Json()}, harness.deps);
                   }),
                   "not installed"));
}

// ---- FioriVoce's own ----

TEST_CASE("a two-round report is stored ready with the document's title and its tool use",
          "[custom_report]") {
    // The acceptance: one read_transcript call, then the document.
    StoreFixture fixture_store;
    ReportHarness harness(fixture("two_round_acceptance"));
    InlineExecutor inline_worker;
    harness.deps.worker = &inline_worker;
    const Json started = start_custom_report(fixture_store.store, harness.deps, "every commitment",
                                             harness.route(), "p_host");
    REQUIRE(started.at("ok") == true);
    const Json report = fixture_store.reports().at(0);
    CHECK(report.at("id") == started.at("report_id"));
    CHECK(report.at("status") == "ready");
    CHECK(report.at("title") == "Vendor renewal commitments");
    CHECK(report.at("markdown").get<std::string>().starts_with("# Vendor renewal commitments"));
    CHECK(report.at("sources").at("tools_used") == Json{{"read_transcript", 1}});
    CHECK(report.at("sources").at("provider") == "custom_report");
    CHECK(report.at("sources").at("model") == "test-model");
    CHECK(report.at("requested_by") == "p_host");
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 2);
    CHECK(bodies[0].at("temperature") == kReportTemperature);
    CHECK(bodies[0].at("model") == "test-model");
    CHECK(bodies[0].at("messages").at(0).at("content").get<std::string>() ==
          report_system_prompt());
    for (const auto& request : harness.provider.requests()) {
        CHECK(request.path == "/v1/chat/completions");
    }
}

TEST_CASE("a pass past its deadline asks for a narrower request", "[custom_report]") {
    ReportHarness harness(fixture("one_turn"));
    CHECK(unavailable([&] {
              generate_report(report_meeting(), ended_state().to_json(), report_segments(),
                              "a brief", harness.route(), harness.deps, 0.0);
          }) == "The report took too long to write. Try a narrower request.");
    CHECK(harness.provider.requests().empty());
}

TEST_CASE("an endpoint without its key and a route without a model say what to fix",
          "[custom_report]") {
    ReportHarness harness;
    // OpenRouter needs a key, and none is saved, in the environment or in .env.
    CHECK(unavailable([&] {
              generate_report(report_meeting(), ended_state().to_json(), report_segments(),
                              "a brief", ReportRoute{"openrouter", "m", Json()}, harness.deps);
          }) == "No API key is configured for the meeting text endpoint. Add one in Settings, "
                "then retry.");
    // A custom endpoint has no default model.
    CHECK(unavailable([&] {
              generate_report(report_meeting(), ended_state().to_json(), report_segments(),
                              "a brief", harness.route(""), harness.deps);
          }) == "Choose a meeting text model first.");
    CHECK(harness.provider.requests().empty());
}

TEST_CASE("a model without tools gets one round with the whole prompt", "[custom_report]") {
    ReportHarness harness(fixture("keeps_reading"));
    const Json metadata = {{"model", "test-model"},
                           {"protocol", "chat"},
                           {"tools", false},
                           {"context_window", 8192},
                           {"max_output_tokens", 1024}};
    const Json result = generate_report(
        report_meeting(), ended_state().to_json(), report_segments(), "a brief",
        ReportRoute{"custom_report", "test-model", harness.endpoint(metadata)}, harness.deps);
    CHECK(result.at("markdown").get<std::string>().starts_with("# Partial"));
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 1);
    CHECK_FALSE(bodies[0].contains("tools"));
}

TEST_CASE("an installed agent that stops early says why", "[custom_report]") {
    ReportHarness harness;
    const int row = GENERATE(0, 1, 2, 3);
    fv::agents::PassOutcome outcome;
    if (row == 0) {
        outcome.canceled = true;
    } else if (row == 1) {
        outcome.error = "Claude Code stalled.";
    } else if (row == 3) {
        outcome.ok = true;
        outcome.text = "  ```\n```  ";
    }
    harness.deps.agent_task = [outcome](std::string_view, std::string_view, std::string_view,
                                        std::string_view, const Json&, fv::agents::ToolHandler,
                                        double, std::string_view, const fv::CancelToken&) {
        return outcome;
    };
    const std::string message = unavailable([&] {
        generate_report(report_meeting(), ended_state().to_json(), report_segments(), "a brief",
                        ReportRoute{"opencode_cli", "", Json()}, harness.deps);
    });
    if (outcome.canceled) {
        CHECK(message == "The report was canceled.");
    } else if (!outcome.error.empty()) {
        CHECK(message == "Claude Code stalled.");
    } else if (!outcome.ok) {
        CHECK(message == "The agent could not write the report.");
    } else {
        CHECK(message == "The report came back empty. Try rephrasing the request.");
    }
}

TEST_CASE("an installed agent's failing read tool answers as an error", "[custom_report]") {
    ReportHarness harness;
    std::pair<std::string, bool> answer;
    harness.deps.agent_task = [&](std::string_view, std::string_view, std::string_view,
                                  std::string_view, const Json&, fv::agents::ToolHandler handler,
                                  double, std::string_view, const fv::CancelToken&) {
        answer = handler("search_transcript", Json{{"query", "beta"}, {"limit", "many"}},
                         Json::object());
        return fv::agents::PassOutcome{.ok = true, .text = "# Brief\n\nbody"};
    };
    const Json result =
        generate_report(report_meeting(), ended_state().to_json(), report_segments(), "a brief",
                        ReportRoute{"codex", "", Json()}, harness.deps);
    CHECK(answer ==
          std::pair<std::string, bool>("search_transcript failed; continue without it.", true));
    CHECK(result.at("sources").at("model") == "default");
    CHECK(result.at("sources").at("tools_used") == Json{{"search_transcript", 1}});
}

TEST_CASE("a worker that cannot start fails the report it claimed", "[custom_report]") {
    struct Refusing final : fv::Executor {
        void post(fv::Task) override { throw std::runtime_error("no thread"); }
        fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task) override { return {}; }
        bool is_current() const override { return false; }
    } refusing;
    StoreFixture fixture_store;
    ReportHarness harness;
    harness.deps.worker = &refusing;
    const Json result = start_custom_report(fixture_store.store, harness.deps, "a brief",
                                            ReportRoute{"openrouter", "m", Json()}, "");
    CHECK(result.at("ok") == false);
    CHECK(result.at("error") == "start_failed");
    const Json report = fixture_store.reports().at(0);
    CHECK(report.at("id") == result.at("report_id"));
    CHECK(report.at("status") == "failed");
    CHECK(report.at("message") == "The report could not be started. Retry later.");
}

TEST_CASE("a failing report never logs the request, the transcript or the report",
          "[custom_report]") {
    LogCapture logs;
    StoreFixture fixture_store;
    ReportHarness harness(fixture("failing_tools"));
    InlineExecutor inline_worker;
    harness.deps.worker = &inline_worker;
    harness.deps.generator = [](const Json&, const Json&, const std::vector<Json>&,
                                std::string_view request, const ReportRoute&, double) -> Json {
        throw std::runtime_error("secret request: " + std::string(request));
    };
    start_custom_report(fixture_store.store, harness.deps, "the confidential pricing",
                        ReportRoute{"openrouter", "m", Json()}, "");
    // A pass whose tool fails (bad JSON, unknown tool) logs nothing it read either.
    const Json result = harness.run("the confidential pricing");
    CHECK(result.at("markdown") == "# Brief\n\nbody");
    const std::string text = logs.text();
    CHECK(contains(text, "Custom report failed"));
    CHECK_FALSE(contains(text, "confidential"));
    CHECK_FALSE(contains(text, "twelve"));
    CHECK_FALSE(contains(text, "# Brief"));
}

TEST_CASE("a report reads past meetings through a RepositoryHistorySource", "[custom_report]") {
    // Python offers recall over the meeting repository; the composition builds the
    // HistorySource over it (lead amendment 26).
    fv::FakeClock clock;
    fv::meeting_state::testing::FakeMeetingRepository stored(clock);
    const auto add_meeting = [&](std::string_view id, std::string_view title,
                                 std::string_view segment_id, std::string_view text) {
        stored.create_meeting(Json{{"id", id},
                                   {"title", title},
                                   {"status", "ended"},
                                   {"started_at", "2026-03-01T10:00:00+00:00"},
                                   {"host_token", std::string("h") + std::string(id)},
                                   {"guest_token", std::string("g") + std::string(id)},
                                   {"spool_dir", std::string("spool_") + std::string(id)}});
        fv::meeting_state::TranscriptSegment segment;
        segment.segment_id = std::string(segment_id);
        segment.meeting_id = std::string(id);
        segment.channel = "mic";
        segment.start_s = 30.0;
        segment.end_s = 34.0;
        segment.text = std::string(text);
        stored.add_segments({segment});
    };
    add_meeting(kPastId, "Vendor renewal", "sg_1", "the discount budget for this renewal");
    add_meeting("m_q1", "Q1 review", "sg_2", "we adopted the discount ceiling");
    RepositoryHistorySource history(stored);

    ReportHarness harness(
        replies(Json::array({chat_reply("", Json::array({tool_call("c1", "search_past_meetings",
                                                                   R"({"query": "discount"})")})),
                             chat_reply("# Brief\n\nbody")})));
    harness.consent(true, false);
    RecallDeps recall{&history, [&harness] { return harness.settings; }, nullptr, nullptr};
    harness.deps.recall = &recall;
    const Json result = harness.run();
    CHECK(result.at("sources").at("past_meetings") == true);
    CHECK(result.at("sources").at("tools_used") == Json{{"search_past_meetings", 1}});
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 2);
    const auto tools = tool_messages(bodies[1]);
    REQUIRE(tools.size() == 1);
    const std::string answer = tools[0].at("content").get<std::string>();
    CHECK(contains(answer, "we adopted the discount ceiling"));
    CHECK(contains(answer, "Q1 review"));
    // The meeting being reported on is never recalled as a past one.
    CHECK_FALSE(contains(answer, "for this renewal"));
}

TEST_CASE("a worker thread survives a publish that throws", "[custom_report]") {
    // An exception leaving a std::thread would end the process; Python's worker thread only
    // printed it.
    StoreFixture fixture_store;
    std::atomic<int> notified = 0;
    const auto subscription = fixture_store.store->subscribe(
        [&notified](std::int64_t, const std::vector<fv::meeting_state::OpResult>&) {
            if (++notified == 2) {
                throw 42; // not a std::exception, so the store's own guard lets it through
            }
        });
    ReportHarness harness;
    harness.deps.generator = [](const Json&, const Json&, const std::vector<Json>&,
                                std::string_view, const ReportRoute&, double) {
        return Json{{"markdown", "# Brief"}, {"title", "Brief"}, {"sources", Json::object()}};
    };
    CHECK(start_custom_report(fixture_store.store, harness.deps, "a brief",
                              ReportRoute{"openrouter", "m", Json()}, "")
              .at("ok") == true);
    CHECK(eventually([&] { return notified.load() == 2; }));
    CHECK(eventually([&] { return fixture_store.store.use_count() == 1; }));
    CHECK(fixture_store.reports().at(0).at("status") == "ready");
}

TEST_CASE("a failure message is stored clipped to 500 code points", "[custom_report]") {
    StoreFixture fixture_store;
    ReportHarness harness;
    InlineExecutor inline_worker;
    harness.deps.worker = &inline_worker;
    harness.deps.generator = [](const Json&, const Json&, const std::vector<Json>&,
                                std::string_view, const ReportRoute&, double) -> Json {
        std::string message;
        for (int i = 0; i < 600; ++i) {
            message += "é";
        }
        throw ReportUnavailable(message);
    };
    start_custom_report(fixture_store.store, harness.deps, "a brief",
                        ReportRoute{"openrouter", "m", Json()}, "");
    const std::string message = fixture_store.reports().at(0).at("message").get<std::string>();
    CHECK(fv::py::len(message) == fv::meeting_state::MAX_MESSAGE_CHARS);
}
