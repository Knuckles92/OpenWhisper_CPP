#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "automation/event_bus.h"
#include "automation/routing.h"
#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "http/testing/fake_http_client.h"
#include "llm/judge.h"
#include "llm/model_catalog.h"
#include "llm/openai_client.h"
#include "llm/testing/fake_judge.h"
#include "pyport/json.h"
#include "routing_support.h"

// Route by voice's status line and budget: test_automation_routing_status.py,
// test_automation_fix_routing_status.py and two functions of test_text_judge.py, over the app's
// own judge provider (keys in an in-memory store, HTTP to an in-process fake); then the replay
// of status and route_budget (tests/golden/services.actions.routing.json).

using namespace fv;
using namespace fv::automation;
using namespace fv::automation::test;
using Catch::Approx;

namespace {

const Json kAction = Json{{"id", "c1"},
                          {"name", "Ask Claude"},
                          {"kind", "clipboard"},
                          {"spoken_names", Json::array({"Claude"})}};

Json settings(const Json& overrides = Json::object()) {
    Json values = Json{{"voice_routing_enabled", true},
                       {"typesafe_enabled", true},
                       {"actions", Json::array({kAction})},
                       {"hotkeys", Json{{"route_record", "ctrl+alt+r"}}}};
    for (const auto& [key, value] : overrides.items()) {
        values[key] = value;
    }
    return values;
}

Json with(const Json& base, const Json& overrides) {
    Json values = base;
    for (const auto& [key, value] : overrides.items()) {
        values[key] = value;
    }
    return values;
}

Json action_off() {
    Json action = kAction;
    action["enabled"] = false;
    return action;
}

std::string where_fast() {
    return "Settings → Meeting Mode → Fast judgments";
}

// The app's judge provider over an in-memory credential store, an environment without keys and
// a .env that does not exist; text models answer through `model`, which refuses everything but
// chat requests.
struct Rig {
    Rig() {
        llm::forget_server_specs();
        model->set_handler([this](const http::Request& request) { return answer(request); });
        llm::JudgeHooks hooks;
        hooks.proxy_for = [](std::string_view) {
            return std::optional<std::string>();
        };
        hooks.new_connection = [connection = connection]() -> std::shared_ptr<http::Client> {
            return connection;
        };
        hooks.make_text_client = [model = model](const llm::TextLLMProfile& profile,
                                                 double timeout_s, std::string_view key) {
            return llm::create_openai_client(model, profile, timeout_s, key, {}, 0);
        };
        environment =
            std::make_unique<llm::JudgeEnvironment>(model, store, env, dotenv, std::move(hooks));
        provider = llm::make_judge_provider(*environment);
    }
    ~Rig() {
        provider.reset();
        environment.reset();
        llm::forget_server_specs();
    }
    Rig(const Rig&) = delete;
    Rig& operator=(const Rig&) = delete;

    void with_key(const llm::JudgeRoute& route = llm::typesafe_route()) {
        store.set(route.credential_env, "test-key");
    }
    RoutingStatus status(const Json& values) { return automation::status(values, *provider); }

    http::ScriptedResponse answer(const http::Request& request) {
        http::ScriptedResponse reply;
        if (!request.url.ends_with("/chat/completions")) {
            reply.error = http::Error{http::ErrorKind::Connect, "connection refused"};
            return reply;
        }
        const Json body = py::loads(std::get<std::string>(request.body));
        std::string content = "{}";
        {
            std::lock_guard lock(mutex);
            chats.push_back(body);
            timeouts.push_back(std::chrono::duration<double>(request.idle_timeout).count());
            if (reply_to) {
                content = reply_to(body);
            }
        }
        reply.status = 200;
        reply.reason = "OK";
        reply.headers = {{"Content-Type", "application/json"}};
        reply.body = py::dumps(Json{
            {"id", "chatcmpl-1"},
            {"object", "chat.completion"},
            {"created", 0},
            {"model", body.value("model", std::string())},
            {"choices",
             Json::array({Json{{"index", 0},
                               {"finish_reason", "stop"},
                               {"message", Json{{"role", "assistant"}, {"content", content}}}}})},
            {"usage", Json{{"prompt_tokens", 123}, {"completion_tokens", 5}}}});
        return reply;
    }

    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
    MapEnvReader env{
        {"LOCALAPPDATA", ""}, {"XDG_DATA_HOME", ""}, {"HOME", ""}, {"USERPROFILE", ""}};
    DotEnv dotenv{std::filesystem::path("fv-routing-tests-no-such-dir") / ".env"};
    std::shared_ptr<http::FakeHttpClient> connection = std::make_shared<http::FakeHttpClient>();
    std::shared_ptr<http::FakeHttpClient> model = std::make_shared<http::FakeHttpClient>();
    std::unique_ptr<llm::JudgeEnvironment> environment;
    std::unique_ptr<llm::JudgeProvider> provider;

    std::mutex mutex;
    std::function<std::string(const Json& body)> reply_to;
    std::vector<Json> chats;
    std::vector<double> timeouts;
};

} // namespace

// -- test_automation_routing_status.py -------------------------------------------------------

TEST_CASE("test_off", "[test_automation_routing_status]") {
    Rig rig;
    CHECK(rig.status(settings(Json{{"voice_routing_enabled", false}})) ==
          RoutingStatus{false, false, "Route by voice is off. Turn it on above.", ""});
    CHECK_FALSE(rig.status(Json::object()).enabled);
}

TEST_CASE("test_on_but_fast_judgments_are_off", "[test_automation_routing_status]") {
    Rig rig;
    rig.with_key();
    CHECK(
        rig.status(settings(Json{{"typesafe_enabled", false}})) ==
        RoutingStatus{true, false,
                      "Route by voice needs Fast judgments. Turn them on in " + where_fast() + ".",
                      "fast_judgments"});
}

TEST_CASE("test_on_but_the_chosen_routes_key_is_missing", "[test_automation_routing_status]") {
    for (const auto& [provider, phrase] : std::vector<std::pair<std::string, std::string>>{
             {"typesafe", "a TypeSafe API key"}, {"openrouter", "an OpenRouter API key"}}) {
        INFO(provider);
        Rig rig;
        // The other route's key doesn't count: routing goes where Fast judgments says.
        rig.with_key(provider == "typesafe" ? llm::openrouter_route() : llm::typesafe_route());
        CHECK(rig.status(settings(Json{{"typesafe_provider", provider}})) ==
              RoutingStatus{true, false,
                            "Route by voice needs " + phrase + ". Add one in " + where_fast() + ".",
                            "fast_judgments"});
    }
}

TEST_CASE("test_on_but_no_actions", "[test_automation_routing_status]") {
    Rig rig;
    rig.with_key();
    CHECK(rig.status(settings(Json{{"actions", Json::array()}})) ==
          RoutingStatus{true, false,
                        "Route by voice is on, but there are no actions yet. Add one above.",
                        "actions"});
}

TEST_CASE("test_on_but_every_action_is_off", "[test_automation_routing_status]") {
    Rig rig;
    rig.with_key();
    CHECK(rig.status(settings(Json{{"actions", Json::array({action_off()})}})) ==
          RoutingStatus{true, false,
                        "Route by voice is on, but every action is turned off. Turn one on above.",
                        "actions"});
}

TEST_CASE("test_ready_says_what_is_sent_and_where", "[test_automation_routing_status]") {
    Rig rig;
    rig.with_key();
    CHECK(rig.status(settings()) ==
          RoutingStatus{true, true,
                        "Route by voice is on. Everything you record with the Route by voice "
                        "shortcut is sent to TypeSafe to choose the action.",
                        ""});
    rig.with_key(llm::openrouter_route());
    CHECK(rig.status(settings(
              Json{{"typesafe_provider", "openrouter"}, {"voice_routing_on_dictation", true}})) ==
          RoutingStatus{true, true,
                        "Route by voice is on. Everything you record with the Route by voice "
                        "shortcut, and dictations that start with an action's name, are sent to "
                        "OpenRouter to choose the action.",
                        ""});
}

TEST_CASE("test_status_never_raises", "[test_automation_routing_status]") {
    // Where Python breaks load_actions, the provider fails: reading the route or the key.
    class BrokenProvider final : public llm::JudgeProvider {
    public:
        std::shared_ptr<llm::Judge> judge(const Json&) override {
            throw std::runtime_error("settings unreadable");
        }
        llm::JudgeRoute route(const Json&) override {
            throw std::runtime_error("settings unreadable");
        }
        bool key_present(const Json&) override { throw std::runtime_error("settings unreadable"); }
    } broken;
    CHECK(automation::status(settings(), broken) ==
          RoutingStatus{false, false, "Route by voice isn't available right now.", ""});
    Rig rig;
    rig.with_key();
    CHECK_FALSE(rig.status(Json()).enabled);
}

// -- test_automation_fix_routing_status.py ---------------------------------------------------

TEST_CASE("test_nothing_to_listen_to_is_not_ready", "[test_automation_fix_routing_status]") {
    Rig rig;
    rig.with_key();
    for (const Json& nothing :
         {Json{{"hotkeys", Json::object()}}, Json{{"hotkeys", Json{{"route_record", ""}}}},
          Json{{"hotkeys", nullptr}}}) {
        INFO(nothing.dump());
        CHECK(rig.status(with(settings(Json{{"voice_routing_on_dictation", false}}), nothing)) ==
              RoutingStatus{true, false,
                            "Route by voice is on, but has nothing to listen to yet. Set a Route "
                            "by voice shortcut below, or turn on Also listen to normal "
                            "dictations.",
                            "trigger"});
    }
    Json no_hotkeys = settings();
    no_hotkeys.erase("hotkeys");
    CHECK(rig.status(no_hotkeys).fix == "trigger");
}

TEST_CASE("test_ready_says_exactly_what_is_sent", "[test_automation_fix_routing_status]") {
    Rig rig;
    rig.with_key();
    CHECK(rig.status(settings()) ==
          RoutingStatus{true, true,
                        "Route by voice is on. Everything you record with the Route by voice "
                        "shortcut is sent to TypeSafe to choose the action.",
                        ""});
    CHECK(rig.status(settings(Json{{"voice_routing_on_dictation", true}})).message ==
          "Route by voice is on. Everything you record with the Route by voice shortcut, and "
          "dictations that start with an action's name, are sent to TypeSafe to choose the "
          "action.");
    CHECK(rig.status(
              settings(Json{{"voice_routing_on_dictation", true}, {"hotkeys", Json::object()}})) ==
          RoutingStatus{true, true,
                        "Route by voice is on. Dictations that start with an action's name are "
                        "sent to TypeSafe to choose the action.",
                        ""});
}

TEST_CASE("test_fix_names_what_is_missing", "[test_automation_fix_routing_status]") {
    struct Row {
        std::string id;
        Json overrides;
        bool has_key;
        std::string fix;
    };
    const std::vector<Row> rows = {
        {"off", Json{{"voice_routing_enabled", false}}, true, ""},
        {"fast-judgments-off", Json{{"typesafe_enabled", false}}, true, "fast_judgments"},
        {"no-key", Json::object(), false, "fast_judgments"},
        {"no-actions", Json{{"actions", Json::array()}}, true, "actions"},
        {"actions-off", Json{{"actions", Json::array({action_off()})}}, true, "actions"},
        {"ready", Json::object(), true, ""},
    };
    for (const Row& row : rows) {
        INFO(row.id);
        Rig rig;
        if (row.has_key) {
            rig.with_key();
        }
        CHECK(rig.status(settings(row.overrides)).fix == row.fix);
    }
}

// -- test_text_judge.py (the two Route by voice functions) -----------------------------------

namespace {

// Settings that choose the text-model route over Ollama's qwen3:4b for AI insights and
// OpenRouter for cleanup, with Route by voice on for plain dictations and one Todo action.
Json text_settings() {
    return Json{
        {"typesafe_enabled", true},
        {"typesafe_provider", "text_model"},
        {"meeting_llm_provider", "ollama"},
        {"meeting_llm_model", "qwen3:4b"},
        {"transcript_cleanup_provider", "openrouter"},
        {"transcript_cleanup_model", "vendor/small-model"},
        {"voice_routing_enabled", true},
        {"voice_routing_on_dictation", true},
        {"actions",
         Json::array({Json{
             {"id", "a_todo"}, {"name", "Todo"}, {"kind", "clipboard"}, {"enabled", true}}})}};
}

// The option number whose name is `name` (number_of).
std::string number_of(const Json& options, std::string_view name) {
    for (const auto& [label, option] : options.items()) {
        if (option.at("name") == std::string(name)) {
            return label;
        }
    }
    return "";
}

} // namespace

TEST_CASE("test_route_by_voice_on_a_local_model_gets_its_own_budget", "[test_text_judge]") {
    const Json values = text_settings();
    CHECK(route_budget(values) == TEXT_MODEL_ROUTE_TIMEOUT_S);
    CHECK(route_budget(Json::object()) == ROUTE_TIMEOUT_S);

    Rig rig;
    // answer_all(choice=choose): every asked question answered, the route by the action and the
    // payload by the words after its name.
    rig.reply_to = [](const Json& body) {
        const Json asked = py::loads(body.at("messages").back().at("content").get<std::string>());
        Json out = Json::object();
        for (const auto& [name, question] : asked.at("questions").items()) {
            const std::string option =
                number_of(question.at("options"), name == "route" ? "a_todo" : "buy milk");
            out[name] = Json{{"option", option}, {"probability", 0.95}};
        }
        return py::dumps(out);
    };
    EventBus bus;
    const RouteDecision decision =
        decide("Todo, buy milk", values, ROUTE_AUTO, *rig.provider, bus, {}, "job_t");
    bus.close();
    CHECK(decision.action_id == "a_todo");
    CHECK(decision.payload == "Buy milk");
    std::lock_guard lock(rig.mutex);
    REQUIRE(rig.timeouts.size() == 1);
    CHECK(rig.timeouts[0] == Approx(TEXT_MODEL_ROUTE_TIMEOUT_S).margin(0.5));
}

TEST_CASE("test_routing_status_says_the_words_stay_here", "[test_text_judge]") {
    Rig rig;
    const RoutingStatus status = rig.status(text_settings());
    CHECK(status.ready);
    CHECK(status.message.find("checked on this computer by Ollama to choose the action") !=
          std::string::npos);
}

// -- review additions ------------------------------------------------------------------------

TEST_CASE("status of settings that are not a mapping is unavailable", "[routing]") {
    // settings or {}: an empty list is {}, any other list has no .get.
    llm::FakeJudgeProvider provider;
    CHECK(automation::status(Json::array({1}), provider) ==
          RoutingStatus{false, false, "Route by voice isn't available right now.", ""});
    CHECK(automation::status("settings", provider) ==
          RoutingStatus{false, false, "Route by voice isn't available right now.", ""});
    CHECK(automation::status(Json::array(), provider) ==
          RoutingStatus{false, false, "Route by voice is off. Turn it on above.", ""});
}

// -- golden vectors --------------------------------------------------------------------------

TEST_CASE("routing status and budget replay OpenWhisper", "[golden]") {
    golden::for_each(kRoutingModule, "status", [](const golden::Case& c) {
        // OpenWhisper's status over the settings, with typesafe.key_present answering the
        // vector's key presence: the route from the settings, the key from the vector.
        const Json values = c.arg<Json>(0);
        llm::FakeJudgeProvider provider;
        provider.set_route(llm::route_from_settings(values.is_object() ? values : Json::object()));
        provider.set_key_present(c.arg<bool>(1));
        const RoutingStatus status = automation::status(values, provider);
        CHECK(
            golden::equal(rebranded(c.result<Json>()),
                          Json::array({status.enabled, status.ready, status.message, status.fix})));
    });
    golden::for_each(kRoutingModule, "route_budget", [](const golden::Case& c) {
        CHECK(route_budget(c.arg<Json>(0)) == c.result<double>());
    });
}
