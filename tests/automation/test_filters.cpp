#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "automation/detail/filter_verdict.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "base/testing/fake_clock.h"
#include "dictation/job.h"
#include "focus/capture_service.h"
#include "process/run_once.h"
#include "pyport/errors.h"
#include "support/hook_support.h"
#include "support/loopback_receiver.h"

// tests/test_automation_hooks_filters.py, test_automation_fix_hooks_filter_cancel.py,
// test_automation_fix_hooks_filter_surrogate.py and test_automation_integration_fixes.py's
// program filter cancel: text filters against a real HTTP server on 127.0.0.1 and real filter
// programs (fv_automation_hook_child in place of `python -c ...`).

using namespace fv::automation;
using fv::Json;
using fv::dictation::DictationJob;
using fv::dictation::JobMode;
using test::Answer;
using test::ReceivedRequest;
using test::Receiver;
using test::wait_until;
using namespace std::chrono_literals;

namespace {

const std::string TEXT = "please send the report to Ana";
constexpr std::string_view BEFORE = "before_cleanup";
constexpr std::string_view AFTER = "after_cleanup";

// An Answer that rewrites the request's text.
std::function<std::string(const ReceivedRequest&)>
reply_text(std::function<std::string(const std::string&)> transform) {
    return [transform](const ReceivedRequest& request) {
        return Json{{"text", transform(request.json()["text"].get<std::string>())}}.dump();
    };
}

std::string upper(const std::string& text) {
    std::string out = text;
    for (char& c : out) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return out;
}

std::shared_ptr<DictationJob> job(JobMode mode = JobMode::Dictation, std::string action_id = "",
                                  bool app = true) {
    auto made = std::make_shared<DictationJob>();
    made->mode = mode;
    made->action_id = std::move(action_id);
    if (app) {
        fv::focus::AppIdentity identity;
        identity.app_id = "slack";
        identity.name = "Slack";
        made->focus = fv::focus::FocusRequest::resolved(fv::focus::FocusSnapshot{identity, {}});
    }
    made->recognition = fv::dictation::RecognitionContext{"en", {}};
    return made;
}

Json http_filter(const std::string& id, const std::string& url, std::string_view stage = BEFORE,
                 const Json& extra = Json::object()) {
    Json row = {
        {"id", id}, {"name", "Filter " + id}, {"kind", "http"}, {"stage", stage}, {"url", url}};
    for (const auto& [key, value] : extra.items()) {
        row[key] = value;
    }
    return row;
}

Json program_filter(const std::string& id, const std::vector<std::string>& commands,
                    std::string_view stage = BEFORE, const Json& extra = Json::object()) {
    Json row = {{"id", id},       {"name", "Filter " + id},        {"kind", "program"},
                {"stage", stage}, {"program", test::hook_child()}, {"args", commands}};
    for (const auto& [key, value] : extra.items()) {
        row[key] = value;
    }
    return row;
}

// UPPER_PROGRAM.
const std::vector<std::string> UPPER_PROGRAM = {"read", "upper-text"};

Json settings_with(const std::vector<Json>& rows, bool leases_allowed = false) {
    return Json{{"automation_filters", rows}, {"api_text_filters", leases_allowed}};
}

FilterOutcome outcome(std::string text, bool canceled = false, bool changed = false,
                      bool program_changed = false) {
    return FilterOutcome{std::move(text), canceled, changed, program_changed};
}

// The leases, the filter.failed events published during the test, and a receiver.
struct Filters {
    Filters() {
        subscription = bus.subscribe(
            [this](const Event& event) {
                std::lock_guard lock(mutex);
                failed.push_back(event);
            },
            std::vector<std::string>{"filter.failed"});
    }
    ~Filters() {
        bus.close();
        receiver.close();
    }
    Filters(const Filters&) = delete;
    Filters& operator=(const Filters&) = delete;

    FilterContext context() { return FilterContext{leases, http, bus}; }
    FilterOutcome apply(std::string_view stage, const std::string& text,
                        const DictationJob* the_job, const Json& settings,
                        const fv::CancelToken& cancel = {}) {
        return fv::automation::apply(stage, text, the_job, std::optional<Json>(settings), context(),
                                     cancel);
    }
    std::vector<Event> failures() {
        std::lock_guard lock(mutex);
        return failed;
    }
    // At least `count` filter.failed events; fails the test when they do not come.
    std::vector<Event> wait_for_failures(std::size_t count) {
        REQUIRE(wait_until([&] { return failures().size() >= count; }, 5s));
        return failures();
    }

    Receiver receiver;
    FilterLeases leases;
    EventBus bus;
    std::shared_ptr<fv::http::Client> http = test::loopback_client();
    std::mutex mutex;
    std::vector<Event> failed;
    std::shared_ptr<EventSubscription> subscription;
};

} // namespace

// -- when filters apply
// ------------------------------------------------------------------------------

TEST_CASE("test_nothing_configured_returns_the_text_unchanged",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings_with({})) == outcome(TEXT));
}

TEST_CASE("test_only_dictation_and_action_jobs_are_filtered",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    const Json settings = settings_with({http_filter("f", f.receiver.url("/f"))});
    CHECK(f.apply(BEFORE, TEXT, nullptr, settings) == outcome(TEXT));
    CHECK(f.apply(BEFORE, TEXT, job(JobMode::Command).get(), settings) == outcome(TEXT));
    CHECK(f.apply(BEFORE, TEXT, job(JobMode::Transform).get(), settings) == outcome(TEXT));
    test::sleep_ms(100);
    CHECK(f.receiver.requests().empty());
}

TEST_CASE("test_no_words_are_never_sent_to_a_filter", "[test_automation_hooks_filters][platform]") {
    // Nor are they a filter cancel ("Not pasted - a text filter handled it" for an empty
    // transcript).
    const std::string_view stage = GENERATE(BEFORE, AFTER);
    const std::string silence =
        GENERATE(std::string(""), std::string(" "), std::string("   "), std::string("\n\t"));
    Filters f;
    f.receiver.answer("/f", {Answer{.body = R"({"cancel": true})"}});
    const FilterOutcome result =
        f.apply(stage, silence, job().get(),
                settings_with({http_filter("f", f.receiver.url("/f"), stage)}));
    CHECK(result == outcome(silence));
    CHECK_FALSE(result.canceled);
    CHECK(f.receiver.requests().empty());
    CHECK(f.receiver.arrivals().empty());
}

TEST_CASE("test_an_http_filter_gets_the_contract_request_and_replaces_the_text",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/f", {Answer{.reply = reply_text(upper)}});
    const auto the_job = job(JobMode::Action, "claude");
    const FilterOutcome result = f.apply(
        AFTER, TEXT, the_job.get(), settings_with({http_filter("f", f.receiver.url("/f"), AFTER)}));

    CHECK(result == outcome(upper(TEXT), false, true));
    const auto request = f.receiver.requests().at(0);
    CHECK(request.header("content-type") == "application/json");
    CHECK(request.json() ==
          Json{{"v", 1},
               {"stage", "after_cleanup"},
               {"job_id", the_job->id},
               {"mode", "action"},
               {"action_id", "claude"},
               {"text", TEXT},
               {"language", "en"},
               {"app", {{"id", "slack"}, {"name", "Slack"}, {"category", "work"}}}});
}

TEST_CASE("test_unknown_app_and_language_are_null", "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/f", {Answer{.body = "{}"}});
    DictationJob bare;
    bare.mode = JobMode::Dictation;
    f.apply(BEFORE, TEXT, &bare, settings_with({http_filter("f", f.receiver.url("/f"))}));
    const Json body = f.receiver.requests().at(0).json();
    CHECK(body["app"].is_null());
    CHECK(body["language"].is_null());
    CHECK(body["action_id"].is_null());
}

TEST_CASE("test_saved_filters_run_in_order_then_leases",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/a", {Answer{.reply = reply_text([](auto& t) { return t + " A"; })}});
    f.receiver.answer("/b", {Answer{.reply = reply_text([](auto& t) { return t + " B"; })}});
    f.receiver.answer("/lease", {Answer{.reply = reply_text([](auto& t) { return t + " L"; })}});
    f.leases.register_lease(f.receiver.url("/lease"), BEFORE);
    const std::vector<Json> rows{http_filter("a", f.receiver.url("/a")),
                                 http_filter("b", f.receiver.url("/b"))};

    const FilterOutcome allowed = f.apply(BEFORE, "x", job().get(), settings_with(rows, true));
    CHECK(allowed.text == "x A B L");
    // Leases need the text filters permission, checked on every dictation.
    const FilterOutcome refused = f.apply(BEFORE, "x", job().get(), settings_with(rows));
    CHECK(refused.text == "x A B");
}

TEST_CASE("test_only_enabled_filters_for_the_stage_run",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/f", {Answer{.reply = reply_text(upper)}});
    const Json settings =
        settings_with({http_filter("after", f.receiver.url("/f"), AFTER),
                       http_filter("off", f.receiver.url("/f"), BEFORE, {{"enabled", false}})});
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings).text == TEXT);
    CHECK(f.receiver.requests().empty());
    CHECK(f.apply(AFTER, TEXT, job().get(), settings).text == upper(TEXT));
}

TEST_CASE("test_lease_modes_choose_the_jobs", "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/lease", {Answer{.reply = reply_text(upper)}});
    f.leases.register_lease(f.receiver.url("/lease"), BEFORE, std::nullopt, Json{"action"});
    const Json settings = settings_with({}, true);
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings).text == TEXT);
    CHECK(f.apply(BEFORE, TEXT, job(JobMode::Action).get(), settings).text == upper(TEXT));
}

TEST_CASE("test_settings_are_read_when_not_given", "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/f", {Answer{.reply = reply_text(upper)}});
    const Json saved = settings_with({http_filter("f", f.receiver.url("/f"))});
    FilterContext context = f.context();
    context.load_settings = [saved] {
        return saved;
    };
    CHECK(fv::automation::apply(BEFORE, TEXT, job().get(), std::nullopt, context).text ==
          upper(TEXT));
}

// -- replies
// -----------------------------------------------------------------------------------------

TEST_CASE("test_replies_that_keep_the_text", "[test_automation_hooks_filters][platform]") {
    const Answer answer =
        GENERATE(Answer{.status = 200, .body = "{}"}, Answer{.status = 204},
                 Answer{.status = 200, .body = ""},
                 Answer{.status = 200, .body = R"({"cancel": false, "note": "ignored"})"});
    Filters f;
    f.receiver.answer("/f", {answer});
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({http_filter("f", f.receiver.url("/f"))})) == outcome(TEXT));
    test::sleep_ms(100);
    CHECK(f.failures().empty());
}

TEST_CASE("test_a_cancel_stops_the_chain_and_delivers_nothing",
          "[test_automation_hooks_filters][platform]") {
    const std::string body =
        GENERATE(std::string(R"({"cancel": true})"), std::string(R"({"text": ""})"),
                 std::string(R"({"text": "  "})"), std::string(R"({"text": " \n "})"));
    Filters f;
    f.receiver.answer("/cancel", {Answer{.body = body}});
    f.receiver.answer("/later", {Answer{.reply = reply_text(upper)}});
    const Json settings = settings_with({http_filter("c", f.receiver.url("/cancel")),
                                         http_filter("later", f.receiver.url("/later"))});
    const FilterOutcome result = f.apply(BEFORE, TEXT, job().get(), settings);
    CHECK(result.canceled);
    CHECK(f.receiver.at("/later").empty());
}

TEST_CASE("test_a_bad_answer_keeps_the_text_and_says_why",
          "[test_automation_hooks_filters][platform]") {
    struct Bad {
        Answer answer;
        std::string reason;
    };
    const Bad bad =
        GENERATE(Bad{Answer{.status = 500, .body = R"({"text": "no"})"}, "HTTP 500"},
                 Bad{Answer{.status = 404}, "HTTP 404"},
                 Bad{Answer{.status = 302, .headers = {{"Location", "/elsewhere"}}},
                     "HTTP 302: redirects aren't followed"},
                 Bad{Answer{.body = "rewritten, but not JSON"}, "The reply wasn't JSON"},
                 Bad{Answer{.body = R"(["a list"])"}, "The reply wasn't a JSON object"},
                 Bad{Answer{.body = R"({"text": 5})"}, "\"text\" wasn't a string"},
                 Bad{Answer{.body = R"({"text": null})"}, "\"text\" wasn't a string"},
                 Bad{Answer{.body = R"({"cancel": "yes"})"}, "\"cancel\" wasn't true or false"});
    Filters f;
    f.receiver.answer("/f", {bad.answer});
    f.receiver.answer("/next", {Answer{.reply = reply_text(upper)}});
    const Json settings = settings_with(
        {http_filter("f", f.receiver.url("/f")), http_filter("next", f.receiver.url("/next"))});
    const FilterOutcome result = f.apply(BEFORE, TEXT, job().get(), settings);

    // The text is kept and the next filter still runs.
    CHECK(result == outcome(upper(TEXT), false, true));
    CHECK(f.receiver.at("/elsewhere").empty());
    const Event event = f.wait_for_failures(1)[0];
    CHECK(event.data == Json{{"filter_id", "f"},
                             {"name", "Filter f"},
                             {"stage", "before_cleanup"},
                             {"reason", bad.reason}});
    CHECK(event.to_json().find(TEXT) == std::string::npos);
}

TEST_CASE("test_an_unreachable_filter_keeps_the_text",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    const Json settings =
        settings_with({http_filter("f", std::format("http://127.0.0.1:{}/f", test::closed_port()),
                                   BEFORE, {{"timeout_ms", 5000}})});
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings) == outcome(TEXT));
    const std::string reason = f.wait_for_failures(1)[0].data["reason"].get<std::string>();
    CHECK((reason == "Couldn't connect" || reason == "No answer within 5 s" ||
           reason == "No answer within 3 s"));
}

// -- time limits
// ---------------------------------------------------------------------------------------

TEST_CASE("test_a_slow_filter_times_out_to_the_original_text",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/slow", {Answer{.delay = 5s, .reply = reply_text(upper)}});
    const Json settings = settings_with(
        {http_filter("slow", f.receiver.url("/slow"), BEFORE, {{"timeout_ms", 300}})});
    const auto started = std::chrono::steady_clock::now();
    const FilterOutcome result = f.apply(BEFORE, TEXT, job().get(), settings);
    const double elapsed = test::seconds_since(started);

    CHECK(result == outcome(TEXT));
    CHECK(elapsed >= 0.29);
    CHECK(elapsed < 1.0);
    CHECK(f.wait_for_failures(1)[0].data["reason"] == "No answer within 0.3 s");
}

TEST_CASE("test_setting_up_requests_is_not_charged_to_a_filters_time",
          "[test_automation_hooks_filters][platform]") {
    // OpenWhisper built its HTTP client on first use (a few hundred ms) before a stage's clock
    // started. Here the client is the app's, made before any filter runs: a fresh one answers a
    // 200 ms filter in time.
    Filters f;
    f.http = fv::http::make_curl_client(
        fv::http::CurlClientOptions{.proxy = [](std::string_view) -> std::optional<std::string> {
            return std::nullopt;
        }});
    f.receiver.answer("/f", {Answer{.reply = reply_text(upper)}});
    const Json settings =
        settings_with({http_filter("f", f.receiver.url("/f"), BEFORE, {{"timeout_ms", 200}})});
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings).text == upper(TEXT));
    CHECK(f.failures().empty());
}

TEST_CASE("test_registering_a_lease_prepares_requests_in_the_background",
          "[test_automation_hooks_filters][platform]") {
    // Nothing needs preparing (the app's client already exists): registering returns at once and
    // contacts nobody.
    Filters f;
    const auto started = std::chrono::steady_clock::now();
    f.leases.register_lease(f.receiver.url("/f"), BEFORE);
    CHECK(test::seconds_since(started) < 0.1);
    test::sleep_ms(100);
    CHECK(f.receiver.arrivals().empty());
}

TEST_CASE("test_the_stage_budget_holds_and_skips_what_is_left",
          "[test_automation_hooks_filters][platform]") {
    Filters f;
    f.receiver.answer("/hang", {Answer{.delay = 10s}});
    std::vector<Json> rows;
    for (const char* name : {"one", "two", "three"}) {
        rows.push_back(http_filter(name, f.receiver.url("/hang"), BEFORE, {{"timeout_ms", 2000}}));
    }
    const auto started = std::chrono::steady_clock::now();
    const FilterOutcome result = f.apply(BEFORE, TEXT, job().get(), settings_with(rows));
    const double elapsed = test::seconds_since(started);

    CHECK(result == outcome(TEXT));
    CHECK(elapsed >= 2.9);
    CHECK(elapsed < 3.6); // Python's 3.4, with room for a machine running tests in parallel
    const auto failures = f.wait_for_failures(3);
    std::vector<std::string> reasons;
    std::vector<std::string> ids;
    for (const auto& event : failures) {
        reasons.push_back(event.data["reason"].get<std::string>());
        ids.push_back(event.data["filter_id"].get<std::string>());
    }
    CHECK(reasons[0] == "No answer within 2 s");
    // The second filter only had what was left of the stage's 3 s: 1 s less whatever the first
    // one's 2 s limit overran by.
    REQUIRE(reasons[1].starts_with("No answer within "));
    const double second = std::stod(reasons[1].substr(std::string("No answer within ").size()));
    CHECK(second >= 0.5);
    CHECK(second <= 1.0);
    CHECK(reasons[2] == "Skipped: this stage's 3 s for filters ran out");
    CHECK(ids == std::vector<std::string>{"one", "two", "three"});
}

TEST_CASE("a filter slower than the stage returns the words after 3 seconds",
          "[filters][platform]") {
    Filters f;
    f.receiver.answer("/hang", {Answer{.delay = 10s}});
    const auto started = std::chrono::steady_clock::now();
    const FilterOutcome result =
        f.apply(BEFORE, TEXT, job().get(),
                settings_with({http_filter("slow", f.receiver.url("/hang"), BEFORE,
                                           {{"timeout_ms", 5000}})}));
    const double elapsed = test::seconds_since(started);
    CHECK(result == outcome(TEXT));
    CHECK(elapsed >= 2.9);
    CHECK(elapsed < 3.6);
    // The filter had what was left of the 3 s when it started.
    const std::string reason = f.wait_for_failures(1)[0].data["reason"].get<std::string>();
    REQUIRE(reason.starts_with("No answer within "));
    const double limit = std::stod(reason.substr(std::string("No answer within ").size()));
    CHECK(limit >= 2.9);
    CHECK(limit <= 3.0);
}

// -- program filters
// -----------------------------------------------------------------------------------

TEST_CASE("test_a_program_filter_reads_stdin_and_answers_on_stdout",
          "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings_with({program_filter("p", UPPER_PROGRAM)})) ==
          outcome(upper(TEXT), false, true));
}

TEST_CASE("test_a_program_filter_sees_the_whole_request",
          "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    Filters f;
    const auto the_job = job();
    const FilterOutcome result =
        f.apply(BEFORE, "h\xc3\xa9llo", the_job.get(),
                settings_with({program_filter("p", {"read", "request-text"})}));
    CHECK(Json::parse(result.text) ==
          Json{{"v", 1},
               {"stage", "before_cleanup"},
               {"job_id", the_job->id},
               {"mode", "dictation"},
               {"action_id", nullptr},
               {"text", "h\xc3\xa9llo"},
               {"language", "en"},
               {"app", {{"id", "slack"}, {"name", "Slack"}, {"category", "work"}}}});
}

TEST_CASE("test_a_failing_program_filter_keeps_the_text",
          "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    struct Failing {
        std::vector<std::string> commands;
        std::string reason;
    };
    const Failing failing =
        GENERATE(Failing{{"read", "exit", "4"}, "Exited with code 4"},
                 Failing{{"read", "write", "not json\n"}, "The reply wasn't JSON"});
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({program_filter("p", failing.commands)})) == outcome(TEXT));
    CHECK(f.wait_for_failures(1)[0].data["reason"] == failing.reason);
}

TEST_CASE("test_a_program_filter_with_no_output_keeps_the_text",
          "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings_with({program_filter("p", {"read"})})) ==
          outcome(TEXT));
    test::sleep_ms(100);
    CHECK(f.failures().empty());
}

TEST_CASE("test_a_program_filter_can_cancel", "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({program_filter("p", {"read", "write", "{\"cancel\": true}\n"})}))
              .canceled);
}

TEST_CASE("test_a_hanging_program_filter_is_stopped_at_its_limit",
          "[test_automation_hooks_filters][platform]") {
    test::require_program_start();
    Filters f;
    const Json settings =
        settings_with({program_filter("p", {"sleep", "30"}, BEFORE, {{"timeout_ms", 500}})});
    const auto started = std::chrono::steady_clock::now();
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings) == outcome(TEXT));
    CHECK(test::seconds_since(started) < 2.5);
    CHECK(f.wait_for_failures(1)[0].data["reason"] == "Timed out after 0.5 s");
}

// -- leases
// --------------------------------------------------------------------------------------------

TEST_CASE("test_leases_accept_loopback_addresses", "[test_automation_hooks_filters][platform]") {
    const std::string url = GENERATE(
        std::string("http://127.0.0.1:8123/filter?x=1"), std::string("http://localhost:9/f"),
        std::string("http://[::1]:9/f"), std::string("https://LOCALHOST/f"));
    FilterLeases leases;
    const Lease lease = leases.register_lease(url, AFTER, Json(800), Json{"dictation"});
    CHECK(lease.id.starts_with("flt_"));
    CHECK(lease.id.size() == 20);
    CHECK(lease.url == url);
    CHECK(lease.stage == "after_cleanup");
    CHECK(lease.timeout_ms == 800);
    CHECK(lease.modes == std::vector<std::string>{"dictation"});
    const double now =
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    CHECK(std::abs(lease.expires_at - (now + filters::LEASE_TTL_S)) <= 2);
    CHECK(leases.leases() == std::vector<Lease>{lease});
}

TEST_CASE("test_leases_refuse_other_addresses", "[test_automation_hooks_filters][platform]") {
    const std::string url =
        GENERATE(std::string("http://192.168.1.10/f"), std::string("http://example.com/f"),
                 std::string("http://127.0.0.2/f"), std::string("http://localhost.example.com/f"),
                 std::string("ftp://127.0.0.1/f"), std::string("127.0.0.1:9/f"), std::string(""));
    FilterLeases leases;
    CHECK_THROWS_AS(leases.register_lease(url, BEFORE), fv::py::ValueError);
    CHECK(leases.leases().empty());
}

TEST_CASE("test_leases_validate_their_settings", "[test_automation_hooks_filters][platform]") {
    struct Refused {
        std::string stage;
        std::optional<Json> timeout_ms;
        std::optional<Json> modes;
    };
    const Refused refused = GENERATE(Refused{"during_cleanup", std::nullopt, std::nullopt},
                                     Refused{"before_cleanup", Json(50), std::nullopt},
                                     Refused{"before_cleanup", Json(6000), std::nullopt},
                                     Refused{"before_cleanup", Json(true), std::nullopt},
                                     Refused{"before_cleanup", Json("1500"), std::nullopt},
                                     Refused{"before_cleanup", std::nullopt, Json("dictation")},
                                     Refused{"before_cleanup", std::nullopt, Json{"command"}},
                                     Refused{"before_cleanup", std::nullopt, Json::array()},
                                     Refused{"before_cleanup", std::nullopt, Json{"file"}});
    FilterLeases leases;
    CHECK_THROWS_AS(leases.register_lease("http://127.0.0.1:9/f", refused.stage, refused.timeout_ms,
                                          refused.modes),
                    fv::py::ValueError);
}

TEST_CASE("test_at_most_ten_leases_even_when_registered_at_once",
          "[test_automation_hooks_filters][platform]") {
    FilterLeases leases;
    std::mutex mutex;
    std::vector<Lease> made;
    int refused = 0;
    std::vector<std::thread> threads;
    for (int i = 0; i < 25; ++i) {
        threads.emplace_back([&] {
            try {
                Lease lease = leases.register_lease("http://127.0.0.1:9/f", BEFORE);
                std::lock_guard lock(mutex);
                made.push_back(std::move(lease));
            } catch (const fv::py::ValueError&) {
                std::lock_guard lock(mutex);
                ++refused;
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    CHECK(made.size() == filters::MAX_LEASES);
    CHECK(refused == 15);
    std::vector<std::string> ids;
    for (const auto& lease : made) {
        ids.push_back(lease.id);
    }
    std::ranges::sort(ids);
    CHECK(std::ranges::unique(ids).begin() == ids.end());
    CHECK(leases.leases().size() == filters::MAX_LEASES);
}

TEST_CASE("test_leases_expire_unless_renewed", "[test_automation_hooks_filters][platform]") {
    fv::FakeClock clock;
    FilterLeases leases(clock, 0.3);
    const Lease kept = leases.register_lease("http://127.0.0.1:9/kept", BEFORE);
    const Lease lapsed = leases.register_lease("http://127.0.0.1:9/lapsed", BEFORE);
    clock.advance(200ms);
    const auto renewed = leases.renew_lease(kept.id);
    REQUIRE(renewed);
    CHECK(renewed->expires_at > kept.expires_at);
    clock.advance(200ms);
    CHECK(leases.leases() == std::vector<Lease>{*renewed});
    CHECK_FALSE(leases.renew_lease(lapsed.id));
    CHECK_FALSE(leases.renew_lease("flt_unknown"));
}

TEST_CASE("test_drop_lease", "[test_automation_hooks_filters][platform]") {
    FilterLeases leases;
    const Lease lease = leases.register_lease("http://127.0.0.1:9/f", BEFORE);
    CHECK(leases.drop_lease(lease.id));
    CHECK_FALSE(leases.drop_lease(lease.id));
    CHECK(leases.leases().empty());
    leases.register_lease("http://127.0.0.1:9/f", BEFORE);
    CHECK(leases.drop_all_leases() == 1);
}

TEST_CASE("test_a_failing_lease_is_reported_by_id", "[test_automation_hooks_filters][platform]") {
    Filters f;
    const Lease lease = f.leases.register_lease(
        std::format("http://127.0.0.1:{}/f", test::closed_port()), BEFORE, Json(200));
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings_with({}, true)) == outcome(TEXT));
    const Json data = f.wait_for_failures(1)[0].data;
    CHECK(data["filter_id"] == lease.id);
    CHECK(data["name"] == "API filter");
}

TEST_CASE("a lease answers the live API's shape", "[filters][platform]") {
    FilterLeases leases;
    const Lease lease = leases.register_lease(" http://127.0.0.1:9/f ", AFTER, Json(1500.0),
                                              Json{"action", "dictation", "action"});
    CHECK(lease.url == "http://127.0.0.1:9/f");
    CHECK(lease.timeout_ms == 1500);
    CHECK(lease.modes == std::vector<std::string>{"action", "dictation"});
    const Json shape = lease.to_json();
    CHECK(shape.size() == 7);
    CHECK(shape["id"] == lease.id);
    CHECK(shape["modes"] == Json{"action", "dictation"});
    CHECK(shape["expires_at"] == lease.expires_at);
    CHECK(shape["ttl_s"] == 60.0);
    CHECK_THROWS_AS(leases.register_lease("http://127.0.0.1:9/f", AFTER, std::nullopt,
                                          Json::array({Json::array({"dictation"})})),
                    fv::py::TypeError);
}

TEST_CASE("leases of an action whose action refuses a program's words are skipped",
          "[filters][platform]") {
    Filters f;
    f.receiver.answer("/lease", {Answer{.reply = reply_text(upper)}});
    f.leases.register_lease(f.receiver.url("/lease"), BEFORE);
    Json settings = settings_with({}, true);
    settings["actions"] = Json::array({{{"id", "ask"},
                                        {"name", "Ask Claude"},
                                        {"kind", "agent"},
                                        {"params", {{"agent", "claude_code"}}}}});
    const auto agent_job = job(JobMode::Action, "ask");
    CHECK(f.apply(BEFORE, TEXT, agent_job.get(), settings) == outcome(TEXT));
    settings["api_agent_prompts"] = true;
    CHECK(f.apply(BEFORE, TEXT, agent_job.get(), settings) ==
          outcome(upper(TEXT), false, true, true));
}

TEST_CASE("a stage's two parts share its budget and only leases are a program's words",
          "[filters][platform]") {
    Filters f;
    f.receiver.answer("/saved", {Answer{.reply = reply_text([](auto& t) { return t + " S"; })}});
    f.receiver.answer("/lease", {Answer{.reply = reply_text([](auto& t) { return t + " L"; })}});
    f.leases.register_lease(f.receiver.url("/lease"), BEFORE);
    StageRun run(std::string(BEFORE),
                 settings_with({http_filter("s", f.receiver.url("/saved"))}, true), f.context());
    const auto the_job = job();
    const FilterOutcome saved = run.saved("x", *the_job);
    CHECK(saved == outcome("x S", false, true, false));
    CHECK(run.budget_s() < filters::STAGE_BUDGET_S);
    const FilterOutcome leased = run.leased(saved.text, *the_job);
    CHECK(leased == outcome("x S L", false, true, true));
}

// -- privacy
// ---------------------------------------------------------------------------------------------

TEST_CASE("test_logs_never_carry_the_text_or_the_address",
          "[test_automation_hooks_filters][platform]") {
    test::ScopedLogCapture capture;
    Filters f;
    f.receiver.answer("/f", {Answer{.status = 500}});
    f.apply(BEFORE, TEXT, job().get(),
            settings_with({http_filter("f", f.receiver.url("/f?key=sekrit")),
                           program_filter("p", {"read", "exit", "1"})}));
    f.wait_for_failures(2);
    const std::string text = capture.text();
    for (const std::string& needle :
         {TEXT, std::string("sekrit"), std::format(":{}", f.receiver.port())}) {
        CHECK(text.find(needle) == std::string::npos);
    }
}

// -- cancel (test_automation_fix_hooks_filter_cancel.py)
// ------------------------------------------------

namespace {

struct Background {
    std::thread worker;
    FilterOutcome result;
    std::chrono::steady_clock::time_point at;
};

void apply_in_background(Filters& f, Background& background, Json settings,
                         fv::CancelToken cancel) {
    background.worker =
        std::thread([&f, &background, settings = std::move(settings), cancel = std::move(cancel)] {
            background.result = f.apply(BEFORE, TEXT, job().get(), settings, cancel);
            background.at = std::chrono::steady_clock::now();
        });
}

} // namespace

TEST_CASE("test_a_cancel_stops_waiting_on_an_http_filter_and_skips_the_rest",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    Filters f;
    f.receiver.answer("/slow", {Answer{.body = R"({"text": "too late"})", .delay = 3s}});
    const Json settings =
        settings_with({http_filter("slow", f.receiver.url("/slow"), BEFORE, {{"timeout_ms", 2500}}),
                       http_filter("later", f.receiver.url("/later"))});
    fv::CancelSource cancel;
    Background background;
    apply_in_background(f, background, settings, cancel.token());
    REQUIRE(wait_until([&] { return f.receiver.arrived_at("/slow"); }, 5s));
    cancel.cancel();
    const auto canceled_at = std::chrono::steady_clock::now();
    background.worker.join();

    CHECK(background.result == outcome(TEXT));
    CHECK(test::seconds_between(canceled_at, background.at) < 0.25);
    test::sleep_ms(200);
    CHECK_FALSE(f.receiver.arrived_at("/later"));
    CHECK(f.failures().empty());
}

TEST_CASE("test_a_cancel_before_the_stage_sends_nothing",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    Filters f;
    fv::CancelSource cancel;
    cancel.cancel();
    const Json settings = settings_with({http_filter("f", f.receiver.url("/f"))});
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings, cancel.token()) == outcome(TEXT));
    test::sleep_ms(200);
    CHECK(f.receiver.arrivals().empty());
    CHECK(f.failures().empty());
}

TEST_CASE("test_a_cancel_after_one_filter_changed_the_text_keeps_the_original",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    Filters f;
    fv::CancelSource cancel;
    f.receiver.answer("/first", {Answer{.reply = [&cancel](const ReceivedRequest&) {
                          cancel.cancel();
                          return std::string(R"({"text": "CHANGED"})");
                      }}});
    const Json settings = settings_with({http_filter("first", f.receiver.url("/first")),
                                         http_filter("second", f.receiver.url("/second"))});
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings, cancel.token()) == outcome(TEXT));
    test::sleep_ms(200);
    CHECK_FALSE(f.receiver.arrived_at("/second"));
}

TEST_CASE("test_a_cancel_stops_waiting_on_a_program_filter",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    test::require_program_start();
    Filters f;
    const Json settings = settings_with(
        {program_filter("p", {"read", "sleep", "4"}, BEFORE, {{"timeout_ms", 5000}})});
    fv::CancelSource cancel;
    Background background;
    apply_in_background(f, background, settings, cancel.token());
    test::sleep_ms(500);
    cancel.cancel();
    const auto canceled_at = std::chrono::steady_clock::now();
    background.worker.join();

    CHECK(background.result == outcome(TEXT));
    CHECK(test::seconds_between(canceled_at, background.at) < 0.25);
    CHECK(f.failures().empty());
}

TEST_CASE("test_a_cancel_wins_over_the_answer_it_caused",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    // A program filter sees the same cancel, stops, and answers "failed" at once; that answer
    // must not be reported as the filter failing.
    for (int i = 0; i < 20; ++i) {
        fv::CancelSource cancel;
        const auto verdict = detail::await_verdict(
            [cancel](const fv::CancelToken&) mutable {
                test::sleep_ms(5); // the wait has started polling
                cancel.cancel();
                return detail::Verdict{detail::VerdictKind::Failed, {}, "Canceled"};
            },
            std::nullopt, cancel.token(), "");
        CHECK(verdict.kind == detail::VerdictKind::Canceled);
    }
}

TEST_CASE("test_without_a_cancel_filters_run_as_before",
          "[test_automation_fix_hooks_filter_cancel][platform]") {
    test::require_program_start();
    Filters f;
    f.receiver.answer("/f", {Answer{.body = R"({"text": "new words"})"}});
    const Json settings =
        settings_with({http_filter("f", f.receiver.url("/f")), program_filter("p", UPPER_PROGRAM)});
    fv::CancelSource never;
    CHECK(f.apply(BEFORE, TEXT, job().get(), settings, never.token()) ==
          outcome("NEW WORDS", false, true));
}

// -- surrogates (test_automation_fix_hooks_filter_surrogate.py)
// -------------------------------------------

TEST_CASE("test_an_http_reply_with_a_lone_surrogate_keeps_the_text",
          "[test_automation_fix_hooks_filter_surrogate][platform]") {
    const std::string body = GENERATE(std::string(R"({"text": "hi \ud83d"})"),
                                      std::string(R"({"text": "\ude00 there"})"),
                                      std::string(R"({"text": "a\udfff\ud800b"})"));
    Filters f;
    f.receiver.answer("/f", {Answer{.body = body}});
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({http_filter("f", f.receiver.url("/f"))})) == outcome(TEXT));
    CHECK(f.wait_for_failures(1)[0].data["reason"] == "\"text\" wasn't valid Unicode");
}

TEST_CASE("test_a_program_reply_with_a_lone_surrogate_keeps_the_text",
          "[test_automation_fix_hooks_filter_surrogate][platform]") {
    test::require_program_start();
    Filters f;
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({program_filter(
                      "p", {"read", "write", R"({"text": "x \ud83d"})"})})) == outcome(TEXT));
    CHECK(f.wait_for_failures(1)[0].data["reason"] == "\"text\" wasn't valid Unicode");
}

TEST_CASE("test_a_whole_surrogate_pair_is_an_ordinary_character",
          "[test_automation_fix_hooks_filter_surrogate][platform]") {
    Filters f;
    // The pair as JSON escapes: a backslash, "u" and four hex digits each.
    const std::string escape = std::string(1, '\\') + "u";
    f.receiver.answer(
        "/f", {Answer{.body = "{\"text\": \"done " + escape + "d83d" + escape + "de00\"}"}});
    CHECK(f.apply(BEFORE, TEXT, job().get(),
                  settings_with({http_filter("f", f.receiver.url("/f"))})) ==
          outcome("done \xf0\x9f\x98\x80", false, true));
    test::sleep_ms(100);
    CHECK(f.failures().empty());
}

// -- test_automation_integration_fixes.py
// ---------------------------------------------------------------

TEST_CASE("test_a_canceled_program_filter_stops_its_program",
          "[test_automation_integration_fixes]") {
    fv::CancelSource cancel;
    std::optional<fv::CancelToken> seen;
    const FilterRunOnce run_once = [&seen](const std::vector<std::string>&, std::string_view,
                                           double, const fv::CancelToken& token) {
        seen = token;
        return fv::RunOnceResult{.error = std::string(fv::CANCELED), .canceled = true};
    };
    const auto verdict = detail::program_verdict(run_once, {"prog"}, "{}", 1.0, cancel.token());
    REQUIRE(seen);
    // The program gets the job's own cancel: cancelling it reaches the run.
    CHECK_FALSE(seen->cancelled());
    cancel.cancel();
    CHECK(seen->cancelled());
    CHECK(verdict.kind == detail::VerdictKind::Failed);
}

TEST_CASE("a program filter's exit code decides before its output", "[filters]") {
    const auto answer = [](fv::RunOnceResult result) {
        return [result](const std::vector<std::string>&, std::string_view, double,
                        const fv::CancelToken&) {
            return result;
        };
    };
    const auto verdict = [](const FilterRunOnce& run_once) {
        return detail::program_verdict(run_once, {"prog"}, "{}", 1.0, {});
    };
    CHECK(verdict(answer({.exit_code = 0, .output = R"({"text": "new"})"})) ==
          detail::Verdict{detail::VerdictKind::Replace, "new", {}});
    CHECK(verdict(answer({.exit_code = 2, .output = R"({"text": "new"})"})) ==
          detail::Verdict{detail::VerdictKind::Failed, {}, "Exited with code 2"});
    // Python's `code != 0` with a code of None and no error.
    CHECK(verdict(answer({.output = R"({"text": "new"})"})) ==
          detail::Verdict{detail::VerdictKind::Failed, {}, "Exited with code None"});
}

TEST_CASE("replies are told apart as Python reads them", "[filters][platform]") {
    using detail::interpret;
    using detail::VerdictKind;
    CHECK(interpret("").kind == VerdictKind::Keep);
    CHECK(interpret(" \r\n\t\x0b\x0c").kind == VerdictKind::Keep);
    CHECK(interpret("\xef\xbb\xbf{}").reason == "The reply wasn't JSON");
    CHECK(interpret("\xff").reason == "The reply wasn't JSON");
    CHECK(interpret(R"({"cancel": null})").reason == "\"cancel\" wasn't true or false");
    CHECK(interpret(R"({"cancel": false, "text": "a"})") ==
          detail::Verdict{VerdictKind::Replace, "a", {}});
    CHECK(interpret(R"({"text": "a", "text": "b"})").text == "b");
    CHECK(interpret(R"({"other": 1})").kind == VerdictKind::Keep);
    CHECK_THROWS_AS(interpret(std::string(3000, '[') + std::string(3000, ']')),
                    fv::py::RecursionError);
}
