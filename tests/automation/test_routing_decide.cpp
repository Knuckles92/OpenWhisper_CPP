#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <expected>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "automation/routing.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/judge.h"
#include "llm/testing/fake_judge.h"
#include "llm/typesafe.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "routing_support.h"

// Route by voice's decision: test_automation_routing_decide.py,
// test_automation_fix_routing_timeout.py and test_automation_fix_routing_failure.py, then the
// replay of RoutePlan.read over canned answers (tests/golden/services.actions.routing.json).

using namespace fv;
using namespace fv::automation;
using namespace fv::automation::test;
using namespace std::chrono_literals;
using Catch::Approx;

namespace {

using SteadyClock = std::chrono::steady_clock;

const Json kClaude = Json{{"id", "c1"},
                          {"name", "Ask Claude"},
                          {"kind", "open_link"},
                          {"params", Json{{"template", "claude://claude.ai/new?q={text}"}}},
                          {"spoken_names", Json::array({"Claude"})},
                          {"description", "Open a new Claude chat with my words."}};
const Json kTodo = Json{{"id", "t1"},
                        {"name", "Todo"},
                        {"kind", "webhook"},
                        {"params", Json{{"url", "https://example.test/hook?token=secret"}}},
                        {"spoken_names", Json::array({"to-do list"})},
                        {"description", "Add an item to my todo list."}};
const Json kCodex = Json{{"id", "x1"},
                         {"name", "Codex"},
                         {"kind", "agent"},
                         {"params", Json{{"agent", "codex"}}},
                         {"enabled", false}};

const std::string kAsked = "Claude, what's two plus two?";
const std::string kAfterName = "what's two plus two?";

Json make_settings(const Json& overrides = Json::object()) {
    Json settings = Json{{"actions", Json::array({kClaude, kTodo, kCodex})},
                         {"voice_routing_enabled", true},
                         {"voice_routing_on_dictation", true},
                         {"typesafe_enabled", true}};
    for (const auto& [key, value] : overrides.items()) {
        settings[key] = value;
    }
    return settings;
}

using Handler = std::function<std::optional<Json>(const Json& state, const Json& questions)>;

// answering(): chooses `choice` for the route and `payload` (a span) for the words.
Handler answering(std::string choice, double confidence,
                  std::optional<std::string> payload = std::nullopt) {
    return [choice, confidence, payload](const Json&,
                                         const Json& questions) -> std::optional<Json> {
        const Json& options = questions.at("route").at("criteria");
        const double rest =
            (1 - confidence) / static_cast<double>(std::max<std::size_t>(1, options.size() - 1));
        Json probabilities = Json::object();
        for (const auto& [key, value] : options.items()) {
            probabilities[key] = key == choice ? confidence : rest;
        }
        Json answers = Json{{"route", Json{{"type", "choice"},
                                           {"choice", choice},
                                           {"confidence", confidence},
                                           {"probabilities", probabilities}}}};
        if (questions.contains("payload")) {
            std::vector<std::string> spans;
            for (const auto& [key, value] : questions.at("payload").at("criteria").items()) {
                spans.push_back(key);
            }
            const std::string picked = payload && std::ranges::find(spans, *payload) != spans.end()
                                           ? *payload
                                           : spans.front();
            Json weights = Json::object();
            for (const std::string& span : spans) {
                weights[span] = span == picked ? 0.9 : 0.1 / static_cast<double>(spans.size() - 1);
            }
            answers["payload"] = Json{{"type", "choice"},
                                      {"choice", picked},
                                      {"confidence", 0.9},
                                      {"probabilities", weights}};
        }
        return answers;
    };
}

// The plan decide makes for `text` under `settings`, for answers scripted before the call.
RoutePlan plan_for(const std::string& text, const Json& settings,
                   std::string_view route = ROUTE_AUTO) {
    std::vector<Action> enabled;
    for (const Action& action : load_actions(settings)) {
        if (action.enabled) {
            enabled.push_back(action);
        }
    }
    return *plan(text, enabled, route);
}

Json answered(const Handler& handler, const std::string& text, const Json& settings) {
    const RoutePlan request = plan_for(text, settings);
    return *handler(request.state, request.questions);
}

// An event bus of its own, a fake judge behind a fake provider, and decide over them.
struct Rig {
    Rig() { provider.set_judge(judge); }
    ~Rig() { bus.close(); }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    RouteDecision decide(const std::string& text, const Json& settings,
                         std::string_view route = ROUTE_AUTO, const CancelToken& cancel = {},
                         const std::string& job_id = "job0123456789abc") {
        return automation::decide(text, settings, route, provider, bus, cancel, job_id);
    }

    // route.decided payloads, oldest first.
    std::vector<Json> decided() const {
        std::vector<Json> out;
        for (const Event& event :
             bus.recent(500, std::vector<std::string>{std::string(catalog::ROUTE_DECIDED)})) {
            out.push_back(event.data);
        }
        return out;
    }

    EventBus bus;
    llm::FakeJudgeProvider provider;
    std::shared_ptr<llm::FakeJudge> judge = std::make_shared<llm::FakeJudge>();
};

Json event(const std::string& job_id, const Json& action_id, const Json& confidence,
           bool fallback) {
    return Json{{"job_id", job_id},
                {"action_id", action_id},
                {"confidence", confidence},
                {"fallback", fallback}};
}

RouteDecision routed(std::string action_id, std::string payload, double confidence) {
    return RouteDecision{std::move(action_id), std::move(payload), confidence, ""};
}

RouteDecision failed(std::string_view failure) {
    return RouteDecision{std::nullopt, "", std::nullopt, std::string(failure)};
}

std::vector<std::string> keys_of(const Json& object) {
    std::vector<std::string> keys;
    for (const auto& [key, value] : object.items()) {
        keys.push_back(key);
    }
    return keys;
}

using Ids = std::vector<std::string>;

// Keys from an in-memory store, a map environment without keys and a .env that does not exist,
// and a judge environment over them whose HTTP never leaves the process.
struct JudgeRig {
    explicit JudgeRig(llm::JudgeHooks hooks = {}) {
        if (!hooks.proxy_for) {
            hooks.proxy_for = [](std::string_view) {
                return std::optional<std::string>();
            };
        }
        environment =
            std::make_unique<llm::JudgeEnvironment>(http, store, env, dotenv, std::move(hooks));
    }

    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
    MapEnvReader env{
        {"LOCALAPPDATA", ""}, {"XDG_DATA_HOME", ""}, {"HOME", ""}, {"USERPROFILE", ""}};
    DotEnv dotenv{std::filesystem::path("fv-routing-tests-no-such-dir") / ".env"};
    std::shared_ptr<http::FakeHttpClient> http = std::make_shared<http::FakeHttpClient>();
    std::unique_ptr<llm::JudgeEnvironment> environment;
};

// Records what reaches the log while it is installed.
class CapturedLog final : public log::Sink {
public:
    void write(const log::Record& record) override {
        std::lock_guard lock(mutex_);
        messages_.emplace_back(record.message);
    }
    std::vector<std::string> messages() const {
        std::lock_guard lock(mutex_);
        return messages_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> messages_;
};

struct LogScope {
    LogScope() : previous(log::level()) {
        log::set_level(log::Level::Debug);
        log::add_sink(sink);
    }
    ~LogScope() {
        log::remove_sink(sink);
        log::set_level(previous);
    }
    LogScope(const LogScope&) = delete;
    LogScope& operator=(const LogScope&) = delete;

    log::Level previous;
    std::shared_ptr<CapturedLog> sink = std::make_shared<CapturedLog>();
};

double seconds_since(SteadyClock::time_point start) {
    return std::chrono::duration<double>(SteadyClock::now() - start).count();
}

} // namespace

// -- when the judge is asked -----------------------------------------------------------------

TEST_CASE("test_off_unless_spoken_routing_is_on", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.99));
    for (std::string_view route : {ROUTE_AUTO, ROUTE_HOTKEY}) {
        CHECK(rig.decide(kAsked, make_settings(Json{{"voice_routing_enabled", false}}), route) ==
              RouteDecision{});
    }
    CHECK(rig.judge->calls().empty());
    CHECK(rig.decided().empty());
}

TEST_CASE("test_plain_dictation_routes_only_while_routing_on_dictation_is_on",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.99));
    CHECK(rig.decide(kAsked, make_settings(Json{{"voice_routing_on_dictation", false}})) ==
          RouteDecision{});
    CHECK(rig.decide(kAsked, make_settings(), "") == RouteDecision{});
    CHECK(rig.judge->calls().empty());
}

TEST_CASE("test_plain_dictation_without_a_name_at_the_start_is_never_sent",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.99));
    for (const std::string text :
         {"I told Claude yesterday that the plan changed.", "Codex, write a test.",
          "Remind me to call the dentist.", "   "}) {
        CHECK(rig.decide(text, make_settings()) == RouteDecision{});
    }
    CHECK(rig.judge->calls().empty());
    CHECK(rig.decided().empty());
}

TEST_CASE("test_plain_dictation_asks_once_about_the_named_action_with_both_questions",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.99));
    rig.decide(kAsked, make_settings());
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    const auto& call = calls[0];
    CHECK(call.timeout_s == ROUTE_TIMEOUT_S);
    CHECK(call.state == Json{{"dictation", kAsked}});
    CHECK(keys_of(call.questions) == Ids{"route", "payload"});
    const Json& route = call.questions.at("route");
    CHECK(route.at("type") == "choice");
    CHECK(keys_of(route.at("criteria")) == Ids{"c1", std::string(ROUTE_NONE)});
    CHECK(route.at("criteria").at("c1") ==
          Json{{"name", "Ask Claude"},
               {"spoken_names", Json::array({"Claude"})},
               {"purpose", "Open a new Claude chat with my words."},
               {"does", "Opens an app or website with the words filled in, ready for the user "
                        "to send."}});
    const Json& payload = call.questions.at("payload");
    CHECK(payload.at("type") == "choice");
    CHECK(keys_of(payload.at("criteria")) == Ids{kAfterName, kAsked, std::string(ROUTE_NONE)});
    CHECK(payload.at("instructions").at("action_names") == Json::array({"Ask Claude", "Claude"}));
}

TEST_CASE("test_route_by_voice_offers_every_enabled_action_named_ones_first",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("t1", 0.99));
    rig.decide("Buy oat milk, add it to my to-do list.", make_settings(), ROUTE_HOTKEY);
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(keys_of(calls[0].questions.at("route").at("criteria")) ==
          Ids{"t1", "c1", std::string(ROUTE_NONE)});
    CHECK(keys_of(calls[0].questions.at("payload").at("criteria")) ==
          Ids{"Buy oat milk", "Buy oat milk, add it to my to-do list.", std::string(ROUTE_NONE)});
}

TEST_CASE("test_route_by_voice_without_a_name_asks_only_which_action",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("t1", 0.9));
    const RouteDecision decision =
        rig.decide("Remind me to call the dentist.", make_settings(), ROUTE_HOTKEY);
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(keys_of(calls[0].questions) == Ids{"route"});
    CHECK(keys_of(calls[0].questions.at("route").at("criteria")) ==
          Ids{"c1", "t1", std::string(ROUTE_NONE)});
    CHECK(decision == routed("t1", "Remind me to call the dentist.", 0.9));
}

TEST_CASE("test_action_settings_never_reach_the_judge", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("t1", 0.9));
    rig.decide("Remind me to call the dentist.", make_settings(), ROUTE_HOTKEY);
    Json sent = Json::array();
    for (const auto& call : rig.judge->calls()) {
        sent.push_back(Json{{"state", call.state}, {"questions", call.questions}});
    }
    const std::string dumped = py::dumps(sent);
    for (std::string_view secret : {"example.test", "token=secret", "claude://", "x1", "Codex"}) {
        INFO(secret);
        CHECK(dumped.find(secret) == std::string::npos);
    }
}

// -- the threshold, failures and the payload -------------------------------------------------

TEST_CASE("test_a_confident_choice_routes_the_words_meant_for_the_action",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.93, kAfterName));
    const RouteDecision decision = rig.decide(kAsked, make_settings(), ROUTE_AUTO, {}, "job_a");
    CHECK(decision == routed("c1", "What's two plus two?", 0.93));
    CHECK(rig.decided() == std::vector<Json>{event("job_a", "c1", 0.93, false)});
}

TEST_CASE("test_the_threshold_is_inclusive", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", ROUTE_CONFIDENCE));
    CHECK(rig.decide(kAsked, make_settings()).action_id == "c1");
    rig.judge->set_handler(answering("c1", ROUTE_CONFIDENCE - 0.01));
    CHECK(rig.decide(kAsked, make_settings(), ROUTE_AUTO, {}, "job_b") == RouteDecision{});
    CHECK(rig.decided().back() ==
          event("job_b", nullptr, py::round_ndigits(ROUTE_CONFIDENCE - 0.01, 4), true));
}

TEST_CASE("test_choosing_none_pastes", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering(std::string(ROUTE_NONE), 0.99));
    CHECK(rig.decide("Claude said the deadline moved.", make_settings(), ROUTE_AUTO, {}, "job_c") ==
          RouteDecision{});
    CHECK(rig.decided() == std::vector<Json>{event("job_c", nullptr, 0.99, true)});
}

TEST_CASE("test_a_failed_judgment_pastes_and_says_so", "[test_automation_routing_decide]") {
    const std::vector<std::pair<std::string, Handler>> judges = {
        // TypeSafeJudge's failure (timeout, HTTP)
        {"no-answer",
         [](const Json&, const Json&) {
             return std::optional<Json>();
         }},
        // a judge that throws instead
        {"raises",
         [](const Json&, const Json&) -> std::optional<Json> {
             throw std::runtime_error("timed out");
         }},
        // not a candidate
        {"unknown-choice",
         [](const Json&, const Json&) {
             return std::optional<Json>(
                 Json{{"route", Json{{"choice", "gone"}, {"confidence", 1.0}}}});
         }},
        // no route answer
        {"no-route",
         [](const Json&, const Json&) {
             return std::optional<Json>(
                 Json{{"payload", Json{{"choice", std::string(ROUTE_NONE)}}}});
         }},
    };
    for (const auto& [id, handler] : judges) {
        INFO(id);
        Rig rig;
        rig.judge->set_handler(handler);
        CHECK(rig.decide(kAsked, make_settings(), ROUTE_AUTO, {}, "job_d") ==
              failed(FAILURE_UNREACHABLE));
        CHECK(rig.judge->calls().size() == 1);
        CHECK(rig.decided() == std::vector<Json>{event("job_d", nullptr, nullptr, true)});
    }
}

TEST_CASE("test_none_of_these_spans_counts_for_the_whole_text",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler([](const Json&, const Json& questions) {
        const Ids spans = keys_of(questions.at("payload").at("criteria"));
        return std::optional<Json>(Json{
            {"route",
             Json{{"choice", "c1"}, {"confidence", 0.9}, {"probabilities", Json{{"c1", 0.95}}}}},
            {"payload",
             Json{{"choice", spans[0]},
                  {"confidence", 0.2},
                  {"probabilities",
                   Json{{spans[0], 0.4}, {spans[1], 0.3}, {std::string(ROUTE_NONE), 0.3}}}}}});
    });
    CHECK(rig.decide(kAsked, make_settings()).payload == kAsked);
}

TEST_CASE("test_without_a_payload_answer_the_whole_text_goes", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->push(Json{{"route", Json{{"choice", "c1"}, {"confidence", 0.9}}}});
    CHECK(rig.decide(kAsked, make_settings()) == routed("c1", kAsked, 0.9));
}

TEST_CASE("test_payload_without_probabilities_uses_the_choice",
          "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->push(Json{{"route", Json{{"choice", "c1"}, {"confidence", 0.9}}},
                         {"payload", Json{{"choice", kAfterName}}}});
    CHECK(rig.decide(kAsked, make_settings()).payload == "What's two plus two?");
}

TEST_CASE("test_decide_never_raises", "[test_automation_routing_decide]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.9));
    // No job: Python's getattr(None, "route", "") is "".
    CHECK(rig.decide(kAsked, make_settings(), "") == RouteDecision{});
    CHECK(rig.decide(kAsked, Json{{"actions", "garbage"},
                                  {"voice_routing_enabled", true},
                                  {"voice_routing_on_dictation", true}}) == RouteDecision{});
    CHECK(rig.decide("", make_settings()) == RouteDecision{});
    CHECK(rig.decide(kAsked, Json()) == RouteDecision{});

    // A provider that throws where the app's would read a broken credential store.
    class BrokenProvider final : public llm::JudgeProvider {
    public:
        std::shared_ptr<llm::Judge> judge(const Json&) override {
            throw std::runtime_error("store gone");
        }
        llm::JudgeRoute route(const Json&) override { throw std::runtime_error("store gone"); }
        bool key_present(const Json&) override { throw std::runtime_error("store gone"); }
    } broken;
    CHECK(automation::decide(kAsked, make_settings(), ROUTE_AUTO, broken, rig.bus, {}, "job_e") ==
          failed(FAILURE_UNREACHABLE));
}

TEST_CASE("test_the_words_are_never_logged", "[test_automation_routing_decide]") {
    LogScope logs;
    {
        Rig rig;
        rig.judge->set_handler(answering("c1", 0.9));
        rig.decide(kAsked, make_settings());
    }
    {
        Rig rig;
        rig.judge->set_handler([](const Json&, const Json&) -> std::optional<Json> {
            throw std::runtime_error("Claude, what's two plus two?");
        });
        rig.decide(kAsked, make_settings());
    }
    const auto messages = logs.sink->messages();
    CHECK_FALSE(messages.empty());
    for (const std::string& message : messages) {
        INFO(message);
        CHECK(message.find("two plus two") == std::string::npos);
    }
}

// -- request size ----------------------------------------------------------------------------

TEST_CASE("test_a_long_dictation_is_shown_by_its_start_and_end",
          "[test_automation_routing_decide]") {
    std::string text = "Claude, ";
    for (int i = 0; i < 800; ++i) {
        text += "lorem ipsum dolor sit amet ";
    }
    text += "the end.";
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.9));
    const RouteDecision decision = rig.decide(text, make_settings());
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    const std::string encoded =
        py::dumps(Json{{"state", calls[0].state}, {"questions", calls[0].questions}});
    CHECK(encoded.size() < llm::kMaxStateChars);
    const std::string dictation = calls[0].state.at("dictation").get<std::string>();
    CHECK(dictation.starts_with("Claude, lorem"));
    CHECK(dictation.ends_with("the end."));
    CHECK(decision.payload.ends_with("the end."));
    CHECK(decision.payload.size() > 20'000);
}

TEST_CASE("test_fifty_wordy_actions_still_fit_one_request", "[test_automation_routing_decide]") {
    Json actions = Json::array();
    for (int i = 0; i < 50; ++i) {
        Json spoken = Json::array();
        for (char w : std::string("abcdefgh")) {
            spoken.push_back("spoken " + std::to_string(i) + " " + std::string(1, w));
        }
        std::string description;
        for (int k = 0; k < 7; ++k) {
            description += "A long description of what this does. ";
        }
        actions.push_back(Json{{"id", "a" + std::to_string(i)},
                               {"name", "Action number " + std::to_string(i)},
                               {"kind", "scripts"},
                               {"spoken_names", spoken},
                               {"description", description}});
    }
    Rig rig;
    rig.judge->set_handler(answering("a7", 0.9));
    rig.decide("Do the thing for me please.", make_settings(Json{{"actions", actions}}),
               ROUTE_HOTKEY);
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].questions.at("route").at("criteria").size() == 51);
    const std::string encoded =
        py::dumps(Json{{"state", calls[0].state}, {"questions", calls[0].questions}});
    CHECK(encoded.size() <= 20'000);
}

// -- through the real client -----------------------------------------------------------------

TEST_CASE("test_the_request_passes_the_real_clients_validation",
          "[test_automation_routing_decide]") {
    // A TypeSafeJudge over a fake transport: the request and answers have the real shapes.
    struct Sent {
        std::mutex mutex;
        std::vector<std::pair<Json, double>> calls;
    };
    auto sent = std::make_shared<Sent>();
    JudgeRig judges;
    llm::TypeSafeJudgeOptions options;
    options.transport =
        [sent](const Json& payload,
               double timeout_s) -> std::expected<llm::TransportReply, std::string> {
        {
            std::lock_guard lock(sent->mutex);
            sent->calls.emplace_back(payload, timeout_s);
        }
        const Json answers =
            *answering("c1", 0.8, kAfterName)(payload.at("state"), payload.at("questions"));
        return llm::TransportReply{
            200, py::dumps(Json{{"answers", answers}, {"usage", Json{{"input_tokens", 700}}}})};
    };
    Rig rig;
    rig.provider.set_judge(
        std::make_shared<llm::TypeSafeJudge>("test-key", *judges.environment, options));
    CHECK(rig.decide(kAsked, make_settings()) == routed("c1", "What's two plus two?", 0.8));
    std::lock_guard lock(sent->mutex);
    REQUIRE(sent->calls.size() == 1);
    CHECK(sent->calls[0].second == ROUTE_TIMEOUT_S);
    CHECK(sent->calls[0].first.at("model") == std::string(llm::kTypeSafeModel));
}

TEST_CASE("test_the_judge_follows_the_fast_judgments_route", "[test_automation_routing_decide]") {
    for (const auto* judge_route : {&llm::typesafe_route(), &llm::openrouter_route()}) {
        INFO(judge_route->id);
        // One fake connection serves every connection the keep-alive pool opens.
        auto connection = std::make_shared<http::FakeHttpClient>();
        connection->set_handler([](const http::Request& request) {
            const Json payload = py::loads(std::get<std::string>(request.body));
            http::ScriptedResponse reply;
            reply.status = 200;
            reply.reason = "OK";
            reply.headers = {{"Content-Type", "application/json"}};
            reply.body = py::dumps(Json{
                {"answers", *answering("c1", 0.9)(payload.at("state"), payload.at("questions"))}});
            return reply;
        });
        llm::JudgeHooks hooks;
        hooks.new_connection = [connection]() -> std::shared_ptr<http::Client> {
            return connection;
        };
        JudgeRig judges(std::move(hooks));
        judges.store.set(judge_route->credential_env, "test-key");
        const auto provider = llm::make_judge_provider(*judges.environment);
        EventBus bus;
        const RouteDecision decision =
            automation::decide(kAsked, make_settings(Json{{"typesafe_provider", judge_route->id}}),
                               ROUTE_AUTO, *provider, bus, {}, "job_f");
        bus.close();
        CHECK(decision.action_id == "c1");
        const auto requests = connection->requests();
        REQUIRE(requests.size() == 1);
        CHECK(requests[0].url == judge_route->endpoint);
        CHECK(py::loads(std::get<std::string>(requests[0].body)).at("model") == judge_route->model);
        // The request's idle timeout is what is left of the route's budget when the judge's
        // worker sends it (its deadline minus a steady-clock now the judge reads itself, with no
        // clock to inject): never more than the budget, and short of it only by the time spent
        // before sending, which a loaded machine stretches past Approx's 1e-5 (1.499 once).
        const double idle = std::chrono::duration<double>(requests[0].idle_timeout).count();
        CHECK(idle <= ROUTE_TIMEOUT_S);
        CHECK(idle > ROUTE_TIMEOUT_S - 0.25);
        CHECK(judges.http->requests().empty());
    }
}

TEST_CASE("test_without_fast_judgments_nothing_is_sent", "[test_automation_routing_decide]") {
    auto connection = std::make_shared<http::FakeHttpClient>();
    llm::JudgeHooks hooks;
    hooks.new_connection = [connection]() -> std::shared_ptr<http::Client> {
        return connection;
    };
    JudgeRig judges(std::move(hooks));
    judges.store.set(llm::kTypeSafeCredentialEnv, "test-key");
    const auto provider = llm::make_judge_provider(*judges.environment);
    EventBus bus;
    // Nothing is sent, and Route by voice can say the judge couldn't be asked.
    CHECK(automation::decide(kAsked, make_settings(Json{{"typesafe_enabled", false}}), ROUTE_AUTO,
                             *provider, bus, {}, "job_g") == failed(FAILURE_UNREACHABLE));
    judges.store.remove(llm::kTypeSafeCredentialEnv);
    CHECK(automation::decide(kAsked, make_settings(), ROUTE_AUTO, *provider, bus, {}, "job_h") ==
          failed(FAILURE_UNREACHABLE));
    std::vector<bool> fallbacks;
    for (const Event& sent :
         bus.recent(500, std::vector<std::string>{std::string(catalog::ROUTE_DECIDED)})) {
        fallbacks.push_back(sent.data.at("fallback").get<bool>());
    }
    bus.close();
    CHECK(fallbacks == std::vector<bool>{true, true});
    CHECK(connection->requests().empty());
    CHECK(judges.http->requests().empty());
}

// -- test_automation_fix_routing_failure.py --------------------------------------------------

TEST_CASE("test_a_missing_key_at_call_time_is_a_failure_and_sends_nothing",
          "[test_automation_fix_routing_failure]") {
    auto connection = std::make_shared<http::FakeHttpClient>();
    llm::JudgeHooks hooks;
    hooks.new_connection = [connection]() -> std::shared_ptr<http::Client> {
        return connection;
    };
    JudgeRig judges(std::move(hooks));
    const auto provider = llm::make_judge_provider(*judges.environment);
    EventBus bus;
    CHECK(automation::decide(kAsked, make_settings(), ROUTE_HOTKEY, *provider, bus, {}, "job_i") ==
          failed(FAILURE_UNREACHABLE));
    std::vector<Json> decided;
    for (const Event& sent :
         bus.recent(500, std::vector<std::string>{std::string(catalog::ROUTE_DECIDED)})) {
        decided.push_back(sent.data);
    }
    bus.close();
    CHECK(decided == std::vector<Json>{event("job_i", nullptr, nullptr, true)});
    CHECK(connection->requests().empty());
}

// -- test_automation_fix_routing_timeout.py --------------------------------------------------

namespace {

// Generous for a loaded test machine, and still well below the 2.4 s seen in the review.
constexpr double kSlackS = 0.4;

// A transport that answers in two parts, each just under the timeout, unless released.
struct Trickle {
    std::mutex mutex;
    std::condition_variable changed;
    bool closing = false;
    std::promise<void> finished;

    // closing.wait(1.2): true when released.
    bool wait(std::chrono::milliseconds limit) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, limit, [this] { return closing; });
    }
    void close() {
        {
            std::lock_guard lock(mutex);
            closing = true;
        }
        changed.notify_all();
    }
};

} // namespace

TEST_CASE("test_a_judge_answering_in_slow_parts_is_cut_off_at_the_timeout",
          "[test_automation_fix_routing_timeout]") {
    auto trickle = std::make_shared<Trickle>();
    auto ended = trickle->finished.get_future();
    const Json body = Json{{"answers", answered(answering("c1", 0.99), kAsked, make_settings())}};
    JudgeRig judges;
    llm::TypeSafeJudgeOptions options;
    options.transport = [trickle, body](const Json&,
                                        double) -> std::expected<llm::TransportReply, std::string> {
        trickle->wait(1200ms); // the status line
        trickle->wait(1200ms); // the body
        trickle->finished.set_value();
        return llm::TransportReply{200, py::dumps(body)};
    };
    std::weak_ptr<llm::Judge> asked;
    {
        Rig rig;
        const auto judge =
            std::make_shared<llm::TypeSafeJudge>("test-key", *judges.environment, options);
        asked = judge;
        rig.provider.set_judge(judge);
        const auto started = SteadyClock::now();
        const RouteDecision decision = rig.decide(kAsked, make_settings(), ROUTE_AUTO, {}, "job_j");
        const double elapsed = seconds_since(started);

        CHECK(elapsed < ROUTE_TIMEOUT_S + kSlackS);
        CHECK(decision == failed(FAILURE_TIMEOUT));
        CHECK(rig.decided() == std::vector<Json>{event("job_j", nullptr, nullptr, true)});
    }
    trickle->close();
    // The transport, and the judge asking through it, end before the judge environment they run
    // under goes away.
    CHECK(ended.wait_for(10s) == std::future_status::ready);
    const auto limit = SteadyClock::now() + 10s;
    while (!asked.expired() && SteadyClock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(asked.expired());
}

TEST_CASE("test_a_judge_that_never_answers_is_cut_off_at_the_timeout",
          "[test_automation_fix_routing_timeout]") {
    Rig rig;
    rig.judge->push(answered(answering("c1", 0.99), kAsked, make_settings()), 30s);
    const auto started = SteadyClock::now();
    const RouteDecision decision = rig.decide(kAsked, make_settings());
    const double elapsed = seconds_since(started);

    CHECK(elapsed >= ROUTE_TIMEOUT_S - 0.05);
    CHECK(elapsed < ROUTE_TIMEOUT_S + kSlackS);
    CHECK(decision.failure == FAILURE_TIMEOUT);
    CHECK_FALSE(decision.action_id);
}

TEST_CASE("test_an_answer_inside_the_timeout_still_routes",
          "[test_automation_fix_routing_timeout]") {
    Rig rig;
    rig.judge->push(answered(answering("c1", 0.99), kAsked, make_settings()), 300ms);
    const RouteDecision decision = rig.decide(kAsked, make_settings());
    CHECK(decision.action_id == "c1");
    CHECK(decision.failure.empty());
}

TEST_CASE("test_a_cancel_stops_waiting_at_once", "[test_automation_fix_routing_timeout]") {
    Rig rig;
    rig.judge->push(answered(answering("c1", 0.99), kAsked, make_settings()), 30s);
    CancelSource cancel;
    std::optional<RouteDecision> result;
    SteadyClock::time_point at;
    std::thread worker([&] {
        result = rig.decide(kAsked, make_settings(), ROUTE_AUTO, cancel.token());
        at = SteadyClock::now();
    });
    std::this_thread::sleep_for(200ms);
    cancel.cancel();
    const auto canceled_at = SteadyClock::now();
    worker.join();

    REQUIRE(result);
    CHECK(*result == RouteDecision{});
    CHECK(std::chrono::duration<double>(at - canceled_at).count() < 0.25);
    // A cancelled dictation was neither routed nor pasted: nothing to announce.
    CHECK(rig.decided().empty());
}

TEST_CASE("test_a_cancel_before_routing_sends_nothing", "[test_automation_fix_routing_timeout]") {
    Rig rig;
    rig.judge->set_handler(answering("c1", 0.99));
    CancelSource cancel;
    cancel.cancel();
    CHECK(rig.decide(kAsked, make_settings(), ROUTE_AUTO, cancel.token()) == RouteDecision{});
    CHECK(rig.judge->calls().empty());
    CHECK(rig.decided().empty());
}

// -- review additions ------------------------------------------------------------------------

TEST_CASE("read takes falsy answers as unanswered and fails on other values that are not a "
          "mapping",
          "[routing]") {
    const RoutePlan request = plan_for(kAsked, make_settings());
    // (answers or {}).get("route")
    for (const Json& falsy : {Json(), Json::array(), Json(0), Json(""), Json(false)}) {
        INFO(falsy.dump());
        CHECK_FALSE(request.read(falsy));
    }
    for (const Json& other : {Json::array({"c1"}), Json("route"), Json(5), Json(true)}) {
        INFO(other.dump());
        CHECK_THROWS_AS(request.read(other), py::PyError);
    }
}

TEST_CASE("a judge answering with something other than a mapping pastes without an event",
          "[routing]") {
    // Python's read raises on .get, and decide returns None before publishing.
    Rig rig;
    rig.judge->push(Json::array({"c1"}));
    CHECK(rig.decide(kAsked, make_settings()) == RouteDecision{});
    CHECK(rig.judge->calls().size() == 1);
    CHECK(rig.decided().empty());
}

TEST_CASE("a cancel while the judge's key is read sends nothing", "[routing]") {
    struct KeyRead {
        std::mutex mutex;
        std::condition_variable changed;
        bool entered = false;
        bool released = false;
    };
    // A provider whose judge() blocks, as reading a key from a slow credential store would.
    class SlowKeyProvider final : public llm::JudgeProvider {
    public:
        SlowKeyProvider(std::shared_ptr<KeyRead> read, std::shared_ptr<llm::Judge> judge)
            : read_(std::move(read)), judge_(std::move(judge)) {}
        std::shared_ptr<llm::Judge> judge(const Json&) override {
            std::unique_lock lock(read_->mutex);
            read_->entered = true;
            read_->changed.notify_all();
            read_->changed.wait(lock, [this] { return read_->released; });
            return std::exchange(judge_, nullptr);
        }
        llm::JudgeRoute route(const Json&) override { return llm::typesafe_route(); }
        bool key_present(const Json&) override { return true; }
        bool handed_over() {
            std::lock_guard lock(read_->mutex);
            return judge_ == nullptr;
        }

    private:
        std::shared_ptr<KeyRead> read_;
        std::shared_ptr<llm::Judge> judge_;
    };

    auto read = std::make_shared<KeyRead>();
    auto fake = std::make_shared<llm::FakeJudge>();
    fake->set_handler(answering("c1", 0.99));
    SlowKeyProvider provider(read, fake);
    EventBus bus;
    CancelSource cancel;
    std::optional<RouteDecision> result;
    std::thread worker([&] {
        result = automation::decide(kAsked, make_settings(), ROUTE_AUTO, provider, bus,
                                    cancel.token(), "job_k");
    });
    {
        std::unique_lock lock(read->mutex);
        REQUIRE(read->changed.wait_for(lock, 10s, [&] { return read->entered; }));
    }
    cancel.cancel();
    worker.join();
    {
        std::lock_guard lock(read->mutex);
        read->released = true;
    }
    read->changed.notify_all();
    // The helper ends once it drops its reference to the judge it was handed.
    const auto limit = SteadyClock::now() + 10s;
    while (!(provider.handed_over() && fake.use_count() == 1) && SteadyClock::now() < limit) {
        std::this_thread::sleep_for(10ms);
    }
    CHECK(provider.handed_over());
    CHECK(fake.use_count() == 1);
    REQUIRE(result);
    CHECK(*result == RouteDecision{});
    CHECK(fake->calls().empty());
    CHECK(bus.recent(500, std::vector<std::string>{std::string(catalog::ROUTE_DECIDED)}).empty());
    bus.close();
}

// -- golden vectors --------------------------------------------------------------------------

TEST_CASE("routing reads judgments as OpenWhisper does", "[golden]") {
    golden::for_each(kRoutingModule, "read", [](const golden::Case& c) {
        const auto request = plan(c.arg<std::string>(0), actions_arg(c, 2), c.arg<std::string>(1));
        REQUIRE(request);
        const Json answers = c.arg<Json>(3);
        if (c.raises()) {
            CHECK_THROWS(request->read(answers));
            return;
        }
        const std::optional<Judgment> judgment = request->read(answers);
        const Json& expected = c.result<Json>();
        if (expected.is_null()) {
            CHECK_FALSE(judgment);
            return;
        }
        REQUIRE(judgment);
        CHECK(golden::equal(
            expected,
            Json::array({judgment->action_id ? Json(*judgment->action_id) : Json(nullptr),
                         judgment->confidence, judgment->probability, judgment->payload})));
    });
}
