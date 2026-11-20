// The live API's routes (services/automation/api.py) as the in-app MCP listener serves them:
// tests/fixtures/transcripts/live_api.json, recorded from OpenWhisper's services/agent_mcp/app.py
// (tools/parity/record_transcripts.py, scenario tools/parity/transcripts/live_api.py), replayed
// against agentapi::McpApp::serve with agentapi::LiveApi installed; and the ported tests of
// tests/test_automation_api_routes.py, test_automation_fix_api_unicode.py,
// test_automation_fix_api_remote_filters.py and test_automation_fix_api_agent_prompts.py.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <map>
#include <memory>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "agentapi/app_control.h"
#include "agentapi/history_server.h"
#include "agentapi/live_api.h"
#include "agentapi/mcp_runtime.h"
#include "automation/catalog.h"
#include "automation/control.h"
#include "automation/event_bus.h"
#include "automation/filters.h"
#include "automation/permissions.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "golden/golden.h"
#include "live_support.h"
#include "net/reactor.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;
using namespace std::chrono_literals;
namespace permissions = fv::automation::permissions;

// tests/test_automation_api_routes.py TOKEN.
const std::string kLiveToken = "live-api-test-token-" + std::string(32, 'b');

// The wall time filter leases are stamped with in the transcript (filters.time pinned there).
constexpr std::chrono::seconds kLeaseNow{1'800'000'000};

Json load_transcript(std::string_view name) {
    const auto path = fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" /
                      (std::string(name) + ".json");
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return fv::py::loads(text.str());
}

// An event's id and time differ from run to run: compared as placeholders.
std::string without_volatile(const std::string& text) {
    static const std::regex id(R"("id":"evt_[0-9a-f]{24}")");
    static const std::regex time(R"("time":"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d\.\d{3}Z")");
    return std::regex_replace(std::regex_replace(text, id, R"("id":"evt_")"), time, R"("time":"")");
}

// The listener with the live API mounted, over a copy of the History API fixture.
struct LiveFixture {
    explicit LiveFixture(Json initial_settings = Json::object())
        : database(copy_history_fixture(root.path(), "live.db")),
          settings(std::move(initial_settings)), live(settings, nullptr, *bus, leases, io, clock),
          app(make_app()) {}

    std::unique_ptr<McpApp> make_app(std::vector<std::string> allowed_hosts = {}) {
        McpAppOptions options;
        options.database = database;
        options.token = kLiveToken;
        options.allowed_hosts = std::move(allowed_hosts);
        options.enabled = [] {
            return true;
        };
        options.settings = &settings;
        auto made = std::make_unique<McpApp>(std::move(options));
        live.install(made->live_routes());
        return made;
    }

    Served call(std::string method, std::string path, std::optional<std::string> body = {},
                std::vector<std::pair<std::string, std::optional<std::string>>> headers = {}) {
        return serve(*app, live_request(std::move(method), std::move(path), kLiveToken,
                                        std::move(body), std::move(headers)));
    }
    Served post(std::string path, Json body) {
        return call("POST", std::move(path), fv::py::dumps(body));
    }
    void grant(std::initializer_list<std::string_view> names) {
        Json values = *settings.snapshot();
        for (const std::string_view name : names) {
            values[std::string(name)] = true;
        }
        settings.set_contents(values);
    }

    fv::testkit::TempDataRoot root;
    std::filesystem::path database;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock{std::chrono::system_clock::time_point(kLeaseNow)};
    fv::ManualExecutor io{clock};
    std::shared_ptr<fv::automation::EventBus> bus =
        std::make_shared<fv::automation::EventBus>(fv::automation::DEFAULT_BUFFER_SIZE, 0, clock);
    fv::automation::FilterLeases leases{clock};
    std::shared_ptr<RouteControl> control = std::make_shared<RouteControl>();
    ScopedAppControl registration{control};
    LiveApi live;
    std::unique_ptr<McpApp> app;
};

Json body_of(std::string_view text) {
    return fv::py::loads(text);
}

} // namespace

// --- The transcript ------------------------------------------------------------------------------

namespace {

// The scenario's fake app control (tools/parity/transcripts/live_api.py _Control).
class ScenarioControl final : public fv::automation::AppControl {
public:
    explicit ScenarioControl(const Json& context)
        : live_(context.at("live_state")), actions_(context.at("actions")),
          run_job_(context.at("run_job").get<std::string>()),
          start_job_(context.at("start_job").get<std::string>()) {}

    fv::automation::LiveState state() override {
        return {live_[0].get<bool>(),        live_[1].get<bool>(),
                live_[2].get<bool>(),        live_[3].get<std::string>(),
                live_[4].get<std::string>(), live_[5].get<std::string>(),
                live_[6].get<std::string>()};
    }
    Json list_actions() override { return actions_; }
    std::string run_action(std::string_view, std::string_view, std::string_view) override {
        check();
        return run_job_;
    }
    std::string start_recording(std::optional<std::string>, std::string_view) override {
        check();
        return start_job_;
    }
    void stop_recording() override { check(); }
    void cancel() override { check(); }
    void notify(std::string_view, std::string_view) override { check(); }

    void fail(const Json& change) {
        if (change.is_null()) {
            fail_ = nullptr;
        } else if (change.contains("permission")) {
            fail_ = [key = change.at("permission").get<std::string>()] {
                throw permissions::PermissionDenied(key);
            };
        } else if (change.contains("error")) {
            fail_ = [] {
                throw std::runtime_error("the secret words");
            };
        } else {
            fail_ = [code = change.at("code").get<std::string>(),
                     message = fv::golden::rebrand(change.at("message").get<std::string>())] {
                throw fv::automation::ControlError(code, message);
            };
        }
    }

private:
    void check() const {
        if (fail_) {
            fail_();
        }
    }

    Json live_;
    Json actions_;
    std::string run_job_;
    std::string start_job_;
    std::function<void()> fail_;
};

} // namespace

TEST_CASE("the live API answers every recorded request as OpenWhisper did", "[golden]") {
    const Json transcript = load_transcript("live_api");
    const Json& context = transcript.at("context");
    const Json& initial = context.at("initial");
    fv::testkit::TempDataRoot root;
    const auto database =
        copy_history_fixture(root.path(), context.at("database_name").get<std::string>());
    fv::MemorySettingsStore settings(initial.at("settings"));
    fv::FakeClock clock{std::chrono::system_clock::time_point(
        std::chrono::seconds(context.at("lease_now").get<std::int64_t>()))};
    fv::ManualExecutor io{clock};
    fv::automation::FilterLeases leases{clock};
    auto control = std::make_shared<ScenarioControl>(context);
    ScopedAppControl registration{control};

    bool enabled = initial.at("enabled").get<bool>();
    bool streams = initial.at("streams").get<bool>();
    std::string app_name = initial.at("app").get<std::string>();
    std::string peer = initial.at("peer").get<std::string>();
    std::map<std::string, std::string> lease_ids; // recorded -> this run's

    std::shared_ptr<fv::automation::EventBus> bus;
    std::unique_ptr<LiveApi> live;
    std::unique_ptr<McpApp> main_app;
    std::unique_ptr<McpApp> tailscale_app;
    const auto rebuild = [&](const Json& bus_options) {
        main_app.reset();
        tailscale_app.reset();
        live.reset();
        if (bus) {
            bus->close();
        }
        bus = std::make_shared<fv::automation::EventBus>(
            bus_options.at("buffer_size").get<std::size_t>(),
            bus_options.at("seq_base").get<std::int64_t>(), clock);
        live = std::make_unique<LiveApi>(
            settings, [&streams] { return streams; }, *bus, leases, io, clock);
        const auto make = [&](std::vector<std::string> allowed) {
            McpAppOptions options;
            options.database = database;
            options.token = context.at("token").get<std::string>();
            options.allowed_hosts = std::move(allowed);
            options.enabled = [&enabled] {
                return enabled;
            };
            options.settings = &settings;
            auto app = std::make_unique<McpApp>(std::move(options));
            live->install(app->live_routes());
            return app;
        };
        main_app = make({});
        std::vector<std::string> hosts;
        for (const Json& host : context.at("tailscale_hosts")) {
            hosts.push_back(host.get<std::string>());
        }
        tailscale_app = make(hosts);
    };
    rebuild(initial.at("bus"));

    const Json& exchanges = transcript.at("exchanges");
    for (std::size_t index = 0; index < exchanges.size(); ++index) {
        const Json& exchange = exchanges[index];
        const Json& recorded = exchange.at("request");
        if (recorded.at("path") == "/__scenario__") {
            const Json change = fv::py::loads(recorded.at("body").get<std::string>());
            if (change.contains("app")) {
                app_name = change.at("app").get<std::string>();
            }
            if (change.contains("enabled")) {
                enabled = change.at("enabled").get<bool>();
            }
            if (change.contains("streams")) {
                streams = change.at("streams").get<bool>();
            }
            if (change.contains("peer")) {
                peer = change.at("peer").get<std::string>();
            }
            if (change.contains("settings")) {
                settings.set_contents(change.at("settings"));
            }
            if (change.contains("control")) {
                fv::automation::set_app_control(change.at("control").get<bool>() ? control
                                                                                 : nullptr);
            }
            if (change.contains("fail")) {
                control->fail(change.at("fail"));
            }
            if (change.contains("bus")) {
                rebuild(change.at("bus"));
            }
            if (change.contains("publish")) {
                for (const Json& item : change.at("publish")) {
                    bus->publish_event(item[0].get<std::string>(), item[1],
                                       item.size() > 2 ? item[2].get<bool>() : true);
                }
            }
            if (change.value("leases", "") == "clear") {
                leases.drop_all_leases();
            }
            continue;
        }
        fv::net::HttpRequest request;
        request.method = recorded.at("method").get<std::string>();
        request.path = recorded.at("path").get<std::string>();
        for (const auto& [ours, theirs] : lease_ids) {
            if (const auto at = request.path.find(ours); at != std::string::npos) {
                request.path.replace(at, ours.size(), theirs);
            }
        }
        request.tls = false;
        request.peer = {peer, 50000};
        request.body = recorded.at("body").get<std::string>();
        for (const Json& pair : recorded.at("headers")) {
            request.headers.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
        }
        for (const Json& pair : recorded.at("query")) {
            request.query.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
        }

        McpApp& app = app_name == "tailscale" ? *tailscale_app : *main_app;
        const Served answer = serve(app, request);
        const Json& expected = exchange.at("response");

        INFO("exchange " << index << ": " << request.method << " " << request.path << " "
                         << request.body.substr(0, 300));
        CHECK(answer.status == expected.at("status").get<int>());
        fv::net::Headers expected_headers;
        std::optional<std::size_t> content_length;
        for (const Json& pair : expected.at("headers")) {
            const std::string name = pair[0].get<std::string>();
            if (name == "content-length") {
                content_length = std::stoull(pair[1].get<std::string>());
            } else {
                expected_headers.emplace_back(name, pair[1].get<std::string>());
            }
        }
        CHECK(answer.headers == expected_headers);
        const std::string recorded_body = expected.at("body").get<std::string>();
        std::string expected_body = fv::golden::rebrand(recorded_body);
        std::string body = answer.body;
        if (answer.stream) {
            // A stream ends after its replay here: the scenario's streams are off.
            CHECK(answer.stream->closed());
            expected_body = without_volatile(expected_body);
            body = without_volatile(body);
        } else if (answer.status == 201 && expected.at("status") == 201) {
            // A new lease: this run's id stands for the recorded one from here on.
            const std::string theirs = body_of(answer.body).at("id").get<std::string>();
            const std::string ours = body_of(recorded_body).at("id").get<std::string>();
            lease_ids[ours] = theirs;
            body.replace(body.find(theirs), theirs.size(), ours);
        } else if (answer.status == 200 && request.path.ends_with("/renew")) {
            const std::string theirs = body_of(answer.body).at("id").get<std::string>();
            for (const auto& [ours, mapped] : lease_ids) {
                if (mapped == theirs) {
                    body.replace(body.find(theirs), theirs.size(), ours);
                }
            }
        }
        if (content_length && !answer.stream) {
            CHECK(body.size() + recorded_body.size() == *content_length + expected_body.size());
        }
        if (request.method == "HEAD") {
            // The server sends the head alone; the length is still the body's.
            body.clear();
        }
        CHECK(body == expected_body);
    }
    main_app.reset();
    tailscale_app.reset();
    live.reset();
    bus->close();
}

// --- tests/test_automation_api_routes.py
// ----------------------------------------------------------

TEST_CASE("test_every_route_needs_the_token", "[test_automation_api_routes]") {
    LiveFixture f;
    for (const auto& [method, path] :
         std::vector<std::pair<std::string, std::string>>{{"GET", "/v1/live/state"},
                                                          {"GET", "/v1/events"},
                                                          {"GET", "/v1/actions"},
                                                          {"POST", "/v1/actions/a1/run"},
                                                          {"POST", "/v1/recording/start"},
                                                          {"POST", "/v1/recording/stop"},
                                                          {"POST", "/v1/recording/cancel"},
                                                          {"POST", "/v1/filters"},
                                                          {"POST", "/v1/filters/l1/renew"},
                                                          {"DELETE", "/v1/filters/l1"},
                                                          {"POST", "/v1/notify"}}) {
        INFO(path);
        const Served response =
            serve(*f.app, live_request(method, path, kLiveToken, {}, {{"authorization", {}}}));
        CHECK(response.status == 401);
        CHECK(response.error().at("code") == "unauthorized");
    }
}

TEST_CASE("test_state_has_control_state_permissions_and_listeners",
          "[test_automation_api_routes]") {
    LiveFixture f;
    f.control->live = {true, false, false, "action", "a1", "job-1", "9.9.9"};
    const auto none = [](const fv::automation::Event&) {
    };
    auto a = f.bus->subscribe(none, std::nullopt, std::string(SSE_LABEL));
    auto b = f.bus->subscribe(none, std::nullopt, std::string(CHANNEL_LABEL));
    auto c = f.bus->subscribe(none, std::nullopt, std::string(CHANNEL_LABEL));
    auto d = f.bus->subscribe(none, std::nullopt, "webhooks");

    const Served response = f.call("GET", "/v1/live/state");
    CHECK(fv::py::py_equal(response.json(), Json::parse(R"({
        "recording": true, "busy": false, "meeting": false, "mode": "action",
        "action_id": "a1", "job_id": "job-1", "version": "9.9.9",
        "permissions": {"live_events": true, "run_actions": false, "control_recording": false,
                        "text_filters": false, "agent_prompts": false},
        "listeners": {"events": 1, "claude_channel": 2}})")));
}

TEST_CASE("test_without_the_app_behind_it_state_and_actions_are_unavailable",
          "[test_automation_api_routes]") {
    LiveFixture f;
    fv::automation::set_app_control(nullptr);
    for (const std::string path : {"/v1/live/state", "/v1/actions"}) {
        const Served response = f.call("GET", path);
        CHECK(response.status == 503);
        CHECK(response.error() == Json{{"code", "unavailable"},
                                       {"message", std::string(fv::identity().product) +
                                                       " isn't ready for this yet."}});
    }
}

TEST_CASE("test_actions_are_listed_with_the_token_alone", "[test_automation_api_routes]") {
    LiveFixture f;
    const Served response = f.call("GET", "/v1/actions");
    CHECK(response.status == 200);
    CHECK(
        response.json() ==
        Json::parse(
            R"({"actions": [{"id": "a1", "name": "Ask Claude", "kind": "open_link", "enabled": true}]})"));
}

TEST_CASE("test_powerful_routes_are_off_by_default", "[test_automation_api_routes]") {
    struct Case {
        std::string method, path;
        std::optional<std::string> body;
        std::string_view permission;
    };
    for (const Case& c : std::vector<Case>{
             {"POST", "/v1/actions/a1/run", R"({"text": "hello"})", permissions::RUN_ACTIONS},
             {"POST", "/v1/notify", R"({"text": "hello"})", permissions::RUN_ACTIONS},
             {"POST", "/v1/recording/start", "{}", permissions::CONTROL_RECORDING},
             {"POST", "/v1/recording/stop", std::nullopt, permissions::CONTROL_RECORDING},
             {"POST", "/v1/recording/cancel", std::nullopt, permissions::CONTROL_RECORDING},
             {"POST", "/v1/filters", R"({"url": "http://127.0.0.1:9/f", "stage": "after_cleanup"})",
              permissions::TEXT_FILTERS},
             {"POST", "/v1/filters/l1/renew", std::nullopt, permissions::TEXT_FILTERS},
             {"DELETE", "/v1/filters/l1", std::nullopt, permissions::TEXT_FILTERS}}) {
        DYNAMIC_SECTION(c.method << " " << c.path) {
            LiveFixture f;
            const Served response = f.call(c.method, c.path, c.body);
            CHECK(response.status == 403);
            CHECK(response.error().at("code") == "permission_denied");
            CHECK(response.error().at("message") ==
                  permissions::PermissionDenied(std::string(c.permission)).message());
            CHECK(f.control->calls().empty());
        }
    }
}

TEST_CASE("test_permissions_are_reread_on_every_request", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    CHECK(f.post("/v1/actions/a1/run", {{"text", "hi"}}).status == 202);
    f.settings.set_contents(Json{{std::string(permissions::RUN_ACTIONS), false}});
    CHECK(f.post("/v1/actions/a1/run", {{"text", "hi"}}).status == 403);
    // A malformed saved value means the default, which is off.
    f.settings.set_contents(Json{{std::string(permissions::RUN_ACTIONS), "yes"}});
    CHECK(f.post("/v1/actions/a1/run", {{"text", "hi"}}).status == 403);
    CHECK(f.control->calls().size() == 1);
}

TEST_CASE("test_run_action_returns_the_job", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    const Served response = f.post("/v1/actions/a1/run", {{"text", "make it so"}});
    CHECK(response.status == 202);
    CHECK(response.json() == Json{{"job_id", "job-run"}});
    CHECK(f.control->calls() ==
          std::vector<std::vector<std::string>>{{"run", "a1", "make it so", "api"}});
}

TEST_CASE("test_run_action_text_must_be_1_to_20000_characters", "[test_automation_api_routes]") {
    for (const std::string& body :
         {std::string("{}"), std::string(R"({"text": ""})"), std::string(R"({"text": 5})"),
          std::string(R"({"text": ["hi"]})"),
          R"({"text": ")" + std::string(MAX_ACTION_TEXT + 1, 'x') + R"("})"}) {
        DYNAMIC_SECTION(body.substr(0, 20)) {
            LiveFixture f;
            f.grant({permissions::RUN_ACTIONS});
            const Served response = f.call("POST", "/v1/actions/a1/run", body);
            CHECK(response.status == 422);
            CHECK(response.error() ==
                  Json{{"code", "invalid_parameters"},
                       {"message", "text must be text of 1 to 20,000 characters."}});
            CHECK(f.control->calls().empty());
        }
    }
}

TEST_CASE("test_the_longest_text_is_accepted_even_fully_escaped", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    std::string escaped;
    std::string text;
    for (std::size_t i = 0; i < MAX_ACTION_TEXT; ++i) {
        escaped += "\\u00e9";
        text += "\xC3\xA9";
    }
    const Served response = f.call("POST", "/v1/actions/a1/run", R"({"text": ")" + escaped + "\"}");
    CHECK(response.status == 202);
    CHECK(f.control->calls() == std::vector<std::vector<std::string>>{{"run", "a1", text, "api"}});
}

TEST_CASE("test_bodies_must_be_small_json_objects", "[test_automation_api_routes]") {
    struct Case {
        std::string id, content;
        int status;
        std::string code;
    };
    for (const Case& c :
         std::vector<Case>{{"not-json", "not json", 422, "invalid_parameters"},
                           {"array", "[1, 2]", 422, "invalid_parameters"},
                           {"string", "\"text\"", 422, "invalid_parameters"},
                           {"not-utf8", "\xff\xfe", 422, "invalid_parameters"},
                           {"too-large", "{" + std::string(MAX_RUN_BODY + 1, ' ') + "}", 413,
                            "request_too_large"}}) {
        DYNAMIC_SECTION(c.id) {
            LiveFixture f;
            f.grant({permissions::RUN_ACTIONS});
            const Served response = f.call("POST", "/v1/actions/a1/run", c.content);
            CHECK(response.status == c.status);
            CHECK(response.error().at("code") == c.code);
            CHECK(f.control->calls().empty());
        }
    }
}

TEST_CASE("test_a_body_without_a_length_is_still_limited", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    // A chunked body of 40 chunks of 8 KiB: no Content-Length, read up to the limit.
    fv::net::HttpRequest request =
        live_request("POST", "/v1/actions/a1/run", kLiveToken, std::string(40 * 8192, ' '),
                     {{"content-length", std::nullopt}});
    CHECK(serve(*f.app, request).status == 413);
    // The server stopped reading one past its own cap and says so.
    request.body.clear();
    request.body_too_large = true;
    CHECK(serve(*f.app, request).status == 413);
    CHECK(f.control->calls().empty());
}

TEST_CASE("test_action_ids_are_bounded", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    const Served response =
        f.post("/v1/actions/" + std::string(65, 'a') + "/run", {{"text", "hi"}});
    CHECK(response.status == 422);
    CHECK(response.error().at("code") == "invalid_parameters");
    CHECK(f.control->calls().empty());
    // 64 characters, counted as code points, not bytes.
    std::string accented;
    for (int i = 0; i < 64; ++i) {
        accented += "\xC3\xA9";
    }
    CHECK(f.post("/v1/actions/" + accented + "/run", {{"text", "hi"}}).status == 202);
}

TEST_CASE("test_control_errors_keep_their_code_and_message", "[test_automation_api_routes]") {
    for (const auto& [code, status] : std::vector<std::pair<std::string, int>>{
             {"busy", 409}, {"not_found", 404}, {"refused", 422}, {"unavailable", 503}}) {
        DYNAMIC_SECTION(code) {
            LiveFixture f;
            f.grant({permissions::RUN_ACTIONS, permissions::CONTROL_RECORDING});
            f.control->fail_with(
                [c = code] { throw fv::automation::ControlError(c, "Said plainly."); });
            for (const auto& [method, path, body] :
                 std::vector<std::tuple<std::string, std::string, std::optional<std::string>>>{
                     {"POST", "/v1/actions/a1/run", R"({"text": "hi"})"},
                     {"POST", "/v1/recording/start", R"({"action_id": "a1"})"},
                     {"POST", "/v1/recording/stop", std::nullopt},
                     {"POST", "/v1/recording/cancel", std::nullopt},
                     {"POST", "/v1/notify", R"({"text": "hi"})"},
                     {"GET", "/v1/live/state", std::nullopt},
                     {"GET", "/v1/actions", std::nullopt}}) {
                INFO(path);
                const Served response = f.call(method, path, body);
                CHECK(response.status == status);
                CHECK(response.error() == Json{{"code", code}, {"message", "Said plainly."}});
            }
        }
    }
}

TEST_CASE("test_unexpected_failures_are_reported_without_their_message",
          "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    f.control->fail_with([] { throw std::runtime_error("the secret words"); });
    ScopedLogCapture capture;
    const Served response = f.post("/v1/actions/a1/run", {{"text", "the secret words"}});
    CHECK(response.status == 503);
    CHECK(response.error() == Json{{"code", "unavailable"},
                                   {"message", std::string(fv::identity().product) +
                                                   " couldn't do that right now. Try again."}});
    CHECK(capture.text().find("runtime_error") != std::string::npos);
    CHECK(capture.text().find("secret") == std::string::npos);
}

TEST_CASE("test_recording_routes", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::CONTROL_RECORDING});
    const Served plain = f.call("POST", "/v1/recording/start");
    CHECK(plain.status == 202);
    CHECK(plain.json() == Json{{"job_id", "job-rec"}});
    const Served action = f.post("/v1/recording/start", {{"action_id", "a1"}});
    CHECK(action.status == 202);
    CHECK(action.json() == Json{{"job_id", "job-rec"}});
    const Served stop = f.call("POST", "/v1/recording/stop");
    CHECK(stop.status == 202);
    CHECK(stop.json() == Json::object());
    const Served cancel = f.call("POST", "/v1/recording/cancel");
    CHECK(cancel.status == 202);
    CHECK(cancel.json() == Json::object());
    CHECK(f.control->calls() ==
          std::vector<std::vector<std::string>>{
              {"start", "", "api"}, {"start", "a1", "api"}, {"stop"}, {"cancel"}});
}

TEST_CASE("test_recording_start_checks_the_action_id", "[test_automation_api_routes]") {
    for (const std::string& body : {std::string(R"({"action_id": 3})"),
                                    R"({"action_id": ")" + std::string(65, 'a') + "\"}"}) {
        DYNAMIC_SECTION(body.substr(0, 18)) {
            LiveFixture f;
            f.grant({permissions::CONTROL_RECORDING});
            const Served response = f.call("POST", "/v1/recording/start", body);
            CHECK(response.status == 422);
            CHECK(response.error().at("code") == "invalid_parameters");
            CHECK(f.control->calls().empty());
        }
    }
}

TEST_CASE("test_notify_shows_short_messages", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    const Served shown = f.post("/v1/notify", {{"text", "Build passed"}, {"title", "CI"}});
    CHECK(shown.status == 202);
    CHECK(shown.json() == Json::object());
    for (const Json& body :
         {Json{{"text", "   "}}, Json{{"text", std::string(MAX_NOTICE_TEXT + 1, 'x')}},
          Json{{"text", "ok"}, {"title", std::string(MAX_NOTICE_TITLE + 1, 't')}},
          Json{{"text", "ok"}, {"title", 5}}}) {
        INFO(body.dump().substr(0, 40));
        const Served refused = f.post("/v1/notify", body);
        CHECK(refused.status == 422);
        CHECK(refused.error().at("code") == "invalid_parameters");
    }
    CHECK(f.control->calls() ==
          std::vector<std::vector<std::string>>{{"notify", "Build passed", "CI"}});
}

TEST_CASE("test_filter_leases", "[test_automation_api_routes]") {
    LiveFixture f;
    f.grant({permissions::TEXT_FILTERS});
    const Served created =
        f.post("/v1/filters",
               {{"url", "http://127.0.0.1:9/f"}, {"stage", "after_cleanup"}, {"timeout_ms", 900}});
    REQUIRE(created.status == 201);
    const Json lease = created.json();
    const std::string id = lease.at("id").get<std::string>();
    CHECK(id.starts_with("flt_"));
    CHECK(fv::py::py_equal(lease, Json::parse(R"({"id": ")" + id + R"(",
        "url": "http://127.0.0.1:9/f", "stage": "after_cleanup", "timeout_ms": 900,
        "modes": ["dictation", "action"], "expires_at": 1800000060.0, "ttl_s": 60.0})")));
    const Served defaulted =
        f.post("/v1/filters", {{"url", "http://127.0.0.1:9/f"}, {"stage", "after_cleanup"}});
    CHECK(defaulted.status == 201);
    CHECK(defaulted.json().at("timeout_ms") == 1500);

    const Served refused = f.post("/v1/filters", {{"url", "http://x/f"}, {"stage", "bad"}});
    CHECK(refused.status == 422);
    CHECK(refused.error().at("code") == "invalid_parameters");
    CHECK(refused.error().at("message") ==
          "A filter added over the API must listen on this computer (127.0.0.1, ::1 or "
          "localhost).");
    for (const Json& body :
         {Json{{"stage", "after_cleanup"}}, Json{{"url", "http://127.0.0.1:9/f"}},
          Json{{"url", "http://127.0.0.1:9/f"}, {"stage", "after_cleanup"}, {"timeout_ms", true}},
          Json{{"url", "http://127.0.0.1:9/f"}, {"stage", "after_cleanup"}, {"timeout_ms", "900"}},
          Json{{"url", std::string(MAX_URL_CHARS + 1, 'x')}, {"stage", "after_cleanup"}}}) {
        INFO(body.dump().substr(0, 60));
        CHECK(f.post("/v1/filters", body).status == 422);
    }
    CHECK(f.leases.leases().size() == 2);

    const Served renewed = f.call("POST", "/v1/filters/" + id + "/renew");
    CHECK(renewed.status == 200);
    CHECK(renewed.json() == lease);
    const Served expired = f.call("POST", "/v1/filters/other/renew");
    CHECK(expired.status == 404);
    CHECK(expired.error().at("code") == "not_found");
    const Served deleted = f.call("DELETE", "/v1/filters/" + id);
    CHECK(deleted.status == 204);
    CHECK(deleted.body.empty());
    CHECK(f.call("DELETE", "/v1/filters/other").status == 404);
}

TEST_CASE("test_event_stream_requests_are_checked_before_streaming",
          "[test_automation_api_routes]") {
    LiveFixture f;
    const Served unknown = f.call("GET", "/v1/events?types=recording.started,not.a.type");
    CHECK(unknown.status == 422);
    CHECK(unknown.error().at("code") == "invalid_parameters");
    CHECK(f.call("GET", "/v1/events", {}, {{"Last-Event-ID", "evt_1"}}).status == 422);
    CHECK(f.call("GET", "/v1/events?since=-1").status == 422);
    f.settings.set_contents(Json{{std::string(permissions::LIVE_EVENTS), false}});
    const Served denied = f.call("GET", "/v1/events");
    CHECK(denied.status == 403);
    CHECK(denied.error() ==
          Json{{"code", "permission_denied"},
               {"message", "Allow live events in " + std::string(fv::identity().product) +
                               " Settings > API & MCP."}});
    CHECK(f.bus->subscribers() == 0);
}

TEST_CASE("test_without_settings_the_defaults_apply", "[test_automation_api_routes]") {
    LiveFixture f;
    const Served state = f.call("GET", "/v1/live/state");
    CHECK(state.json().at("permissions") ==
          Json::parse(R"({"live_events": true, "run_actions": false, "control_recording": false,
                         "text_filters": false, "agent_prompts": false})"));
    CHECK(f.post("/v1/actions/a1/run", {{"text", "hi"}}).status == 403);
}

TEST_CASE("test_the_contract_lists_the_live_routes", "[test_automation_api_routes]") {
    LiveFixture f;
    const Json schema = f.call("GET", "/openapi.json").json();
    std::set<std::string> operations;
    for (const auto& [path, methods] : schema.at("paths").items()) {
        for (const auto& [method, operation] : methods.items()) {
            operations.insert(operation.at("operationId").get<std::string>());
        }
    }
    for (const std::string id :
         {"get_live_state", "stream_events", "list_actions", "run_action", "start_recording",
          "stop_recording", "cancel_recording", "create_filter_lease", "renew_filter_lease",
          "delete_filter_lease", "notify_user"}) {
        CHECK(operations.contains(id));
    }
    const Json& run =
        schema.at("paths").at("/v1/actions/{action_id}/run").at("post").at("requestBody");
    const Json& text =
        run.at("content").at("application/json").at("schema").at("properties").at("text");
    CHECK(run.at("required") == true);
    CHECK(text.at("maxLength") == MAX_ACTION_TEXT);
    // The standalone History API has none of them.
    CHECK_FALSE(fv::py::loads(openapi_document()).at("paths").contains("/v1/events"));
}

TEST_CASE("test_types_and_resume_positions_parse", "[test_automation_api_routes]") {
    CHECK_FALSE(parse_types(std::nullopt));
    CHECK_FALSE(parse_types(" , "));
    CHECK(parse_types("recording.started, history.saved") ==
          std::vector<std::string>{"history.saved", "recording.started"});
    CHECK_FALSE(parse_last_event_id(std::nullopt));
    CHECK(parse_last_event_id(" 42 ") == 42);
    CHECK_THROWS_AS(parse_types("recording.started,nope"), Invalid);
    for (const std::string raw : {"-1", "1.5", "9999999999999999999"}) {
        CHECK_THROWS_AS(parse_last_event_id(raw), Invalid);
    }
}

TEST_CASE("a superscript digit in Last-Event-ID fails as int() fails on it",
          "[test_automation_api_routes]") {
    // str.isdigit() takes the Latin-1 superscripts and int() refuses them: an unexpected failure.
    for (const std::string raw : {"\xC2\xB2", "1\xC2\xB3",
                                  "\xC2\xB9"
                                  "0"}) {
        CHECK_THROWS_AS(parse_last_event_id(raw), fv::py::ValueError);
    }
    // Still too long, or not digits, they are refused as any other.
    CHECK_THROWS_AS(parse_last_event_id(std::string(18, '1') + "\xC2\xB2"), Invalid);
    CHECK_THROWS_AS(parse_last_event_id("1\xC2\xBD"), Invalid);

    LiveFixture f;
    ScopedLogCapture capture;
    // The header arrives as bytes, read as Latin-1: 0xB2 is "²".
    const Served response = f.call("GET", "/v1/events", {}, {{"Last-Event-ID", "\xB2"}});
    CHECK(response.status == 503);
    CHECK(response.error().at("code") == "unavailable");
    CHECK(capture.text().find("Live API events failed: ValueError") != std::string::npos);
    CHECK(f.bus->subscribers() == 0);
}

TEST_CASE("test_frames_are_one_line_of_json_per_event", "[test_automation_api_routes]") {
    const std::string text = "two\nlines\r\nand \xE2\x80\xA8 more \xE2\x80\xA9 and \xC2\x85 \x0b "
                             "\x1c caf\xC3\xA9 \xF0\x9F\x98\x80";
    fv::automation::EventBus bus;
    const fv::automation::Event event =
        bus.publish_event("transcription.completed", Json{{"text", text}});
    const std::string frame = sse_frame(event);
    // Readers that split on any line break still see three lines.
    CHECK(frame == "id: " + std::to_string(event.seq) +
                       "\nevent: transcription.completed\ndata: " + event.to_json(true) + "\n\n");
    CHECK(std::all_of(frame.begin(), frame.end(),
                      [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    for (const char c : event.to_json(true)) {
        CHECK((c != '\n' && c != '\r' && c != '\x0b' && c != '\x0c' && c != '\x1c'));
    }
    CHECK(fv::py::loads(frame.substr(frame.find("data: ") + 6)) == event.to_dict());
    const std::string gap = sse_frame(gap_event(7, "evicted"), false);
    CHECK(gap.starts_with("event: events.gap\ndata: "));
    CHECK(gap.find(R"("missed_from_seq":7,"reason":"evicted")") != std::string::npos);
    CHECK(gap.find(R"("seq":0)") != std::string::npos);
    bus.close();
}

TEST_CASE("test_stopping_the_listener_drops_leased_filters",
          "[platform][test_automation_api_routes]") {
    fv::testkit::TempDataRoot root;
    const auto database = copy_history_fixture(root.path(), "history.db");
    fv::net::Reactor reactor{2};
    fv::ThreadPool pool{"live-test", 4};
    fv::Strand io{pool, "live-io", 4, fv::ShutdownMode::CancelQueued};
    fv::MemorySettingsStore settings;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    fv::automation::EventBus bus;
    fv::automation::FilterLeases leases;
    LiveApi live(settings, nullptr, bus, leases, io);
    McpRuntimeDeps deps;
    deps.database = database;
    deps.reactor = &reactor;
    deps.settings = &settings;
    deps.credentials = &credentials;
    deps.io = &io;
    McpRuntime runtime(deps);
    mount_live_api(runtime, live);
    auto port_finder = fv::net::HttpServer::create(reactor);
    const auto endpoint = port_finder->listen({"127.0.0.1", 0, std::nullopt, true});
    REQUIRE(endpoint);
    const int port = endpoint->port;
    port_finder->stop(0ms);
    runtime.start(port);
    REQUIRE(wait_until([&] { return runtime.status().state == "running"; }, 10s));
    leases.register_lease("http://127.0.0.1:9/filter", "after_cleanup");
    CHECK(leases.leases().size() == 1);
    runtime.stop(true);
    CHECK(runtime.status().state == "stopped");
    CHECK(leases.leases().empty());
    bus.close();
}

// --- tests/test_automation_fix_api_unicode.py ----------------------------------------------------

TEST_CASE("test_text_that_is_not_valid_unicode_is_refused", "[test_automation_fix_api_unicode]") {
    const std::string half_emoji = R"("fix this \ud83d")";
    for (const auto& [path, body] : std::vector<std::pair<std::string, std::string>>{
             {"/v1/actions/a1/run", R"({"text": )" + half_emoji + "}"},
             {"/v1/notify", R"({"text": )" + half_emoji + "}"},
             {"/v1/notify", R"({"text": "ok", "title": )" + half_emoji + "}"},
             {"/v1/recording/start", R"({"action_id": )" + half_emoji + "}"},
             {"/v1/filters",
              R"({"url": "http://127.0.0.1:9/\udc00", "stage": "after_cleanup"})"}}) {
        DYNAMIC_SECTION(path << " " << body) {
            LiveFixture f;
            f.grant({permissions::RUN_ACTIONS, permissions::CONTROL_RECORDING,
                     permissions::TEXT_FILTERS});
            const Served response = f.call("POST", path, body);
            CHECK(response.status == 422);
            CHECK(response.error().at("code") == "invalid_parameters");
            CHECK(response.error().at("message").get<std::string>().find("valid Unicode text") !=
                  std::string::npos);
            CHECK(f.control->calls().empty());
            CHECK(f.leases.leases().empty());
        }
    }
}

TEST_CASE("test_whole_emoji_and_escaped_text_still_pass", "[test_automation_fix_api_unicode]") {
    LiveFixture f;
    f.grant({permissions::RUN_ACTIONS});
    const Served response =
        f.call("POST", "/v1/actions/a1/run", R"({"text": "fix this \ud83d\ude00 caf\u00e9"})");
    CHECK(response.status == 202);
    CHECK(f.control->calls() == std::vector<std::vector<std::string>>{
                                    {"run", "a1", "fix this \xF0\x9F\x98\x80 caf\xC3\xA9", "api"}});
}

namespace {

fv::automation::Event unicode_event(std::string text) {
    fv::automation::Event event;
    event.id = "evt_" + std::string(24, '0');
    event.seq = 7;
    event.type = "transcription.completed";
    event.time = "2026-10-10T12:00:00.000Z";
    event.data = Json{{"job_id", "j"}, {"text", std::move(text)}};
    return event;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> lines;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find('\n', start);
        lines.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            return lines;
        }
        start = end + 1;
    }
}

} // namespace

TEST_CASE("test_a_frame_with_a_lone_surrogate_is_still_sent", "[test_automation_fix_api_unicode]") {
    // "\ud83d" as json.loads leaves it: its three-byte encoding.
    const fv::automation::Event event =
        unicode_event("half \xED\xA0\xBD an emoji, \xE2\x80\xA8 a line separator, caf\xC3\xA9");
    const std::string frame = sse_frame(event);
    CHECK(std::all_of(frame.begin(), frame.end(),
                      [](char c) { return static_cast<unsigned char>(c) < 0x80; }));
    const std::vector<std::string> lines = split_lines(frame);
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "id: 7");
    CHECK(lines[1] == "event: transcription.completed");
    CHECK(fv::py::loads(lines[2].substr(6)) == event.to_dict());
    CHECK(lines[2].find(R"(\ud83d)") != std::string::npos);
    CHECK(lines[3].empty());
    CHECK(lines[4].empty());
}

TEST_CASE("test_frames_are_ascii_so_line_readers_never_split_them",
          "[test_automation_fix_api_unicode]") {
    const std::string text = "one\xE2\x80\xA8two\xE2\x80\xA9three\xC2\x85"
                             "four\nfive\r caf\xC3\xA9 \xF0\x9F\x98\x80";
    const std::string frame = sse_frame(unicode_event(text));
    const std::vector<std::string> lines = split_lines(frame);
    // id, event, data and the blank line that ends the message.
    REQUIRE(lines.size() == 5);
    CHECK(lines[3].empty());
    CHECK(frame.find('\r') == std::string::npos);
    CHECK(fv::py::loads(lines[2].substr(6)).at("data").at("text") == text);
}

// --- tests/test_automation_fix_api_remote_filters.py --------------------------------------------

TEST_CASE("test_a_tailscale_peer_cannot_lease_a_filter",
          "[test_automation_fix_api_remote_filters]") {
    LiveFixture f(Json{{std::string(permissions::TEXT_FILTERS), true}});
    auto app = f.make_app({"100.82.22.3"});
    fv::net::HttpRequest request =
        live_request("POST", "/v1/filters", kLiveToken,
                     R"({"url": "http://127.0.0.1:9/filter", "stage": "after_cleanup"})",
                     {{"host", "100.82.22.3"}});
    request.peer = {"100.65.148.53", 1234};
    const Served response = serve(*app, request);
    CHECK(response.status == 403);
    CHECK(response.error() ==
          Json{{"code", "forbidden"},
               {"message", "Only a program on this computer can lease a text filter."}});
    CHECK(f.leases.leases().empty());
}

TEST_CASE("test_a_program_on_this_computer_still_can", "[test_automation_fix_api_remote_filters]") {
    LiveFixture f(Json{{std::string(permissions::TEXT_FILTERS), true}});
    const Served response = f.call(
        "POST", "/v1/filters", R"({"url": "http://127.0.0.1:9/filter", "stage": "after_cleanup"})");
    CHECK(response.status == 201);
    const auto leases = f.leases.leases();
    REQUIRE(leases.size() == 1);
    CHECK(leases[0].url == "http://127.0.0.1:9/filter");
}

TEST_CASE("test_who_counts_as_this_computer", "[test_automation_fix_api_remote_filters]") {
    for (const auto& [host, local] :
         std::vector<std::pair<std::string, bool>>{{"127.0.0.1", true},
                                                   {"127.8.9.10", true},
                                                   {"::1", true},
                                                   {"::ffff:127.0.0.1", true},
                                                   {"::1%3", true},
                                                   {"100.65.148.53", false},
                                                   {"192.168.1.10", false},
                                                   {"testclient", false},
                                                   {"", false}}) {
        INFO(host);
        CHECK(peer_is_local(host) == local);
    }
}

// --- tests/test_automation_fix_api_agent_prompts.py ----------------------------------------------

namespace {

const std::string kAgentPromptsMessage = "Allow sending words to your coding agents in " +
                                         std::string(fv::identity().product) +
                                         " Settings > API & MCP.";

// The three actions of the Python tests: an agent, a channel and a link action.
Json saved_actions(bool agent_prompts = false) {
    Json values = Json::parse(R"({"actions": [
        {"id": "agent", "name": "Agent hotkey", "kind": "agent",
         "params": {"agent": "claude_code", "mode": "handoff"}},
        {"id": "chan", "name": "Tell my session", "kind": "channel"},
        {"id": "link", "name": "Ask Claude", "kind": "open_link",
         "params": {"template": "https://x/?q={text}"}}]})");
    if (agent_prompts) {
        values[std::string(permissions::AGENT_PROMPTS)] = true;
    }
    return values;
}

// The control as the app registers it, with its UI thread run by the test.
struct ControlFixture {
    explicit ControlFixture(Json values = saved_actions()) : settings(std::move(values)) {}

    // fn() on a thread of its own while this thread runs the UI executor's tasks.
    template <class Fn> auto in_worker(Fn fn) {
        using T = decltype(fn());
        std::optional<T> value;
        std::exception_ptr error;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            try {
                value = fn();
            } catch (...) {
                error = std::current_exception();
            }
            done = true;
        });
        while (!done) {
            ui.run_all();
            std::this_thread::sleep_for(1ms);
        }
        worker.join();
        if (error) {
            std::rethrow_exception(error);
        }
        return *value;
    }

    fv::MemorySettingsStore settings;
    fv::ManualExecutor ui;
    FakeHost host;
    UiAppControl control{host, ui, settings};
};

} // namespace

TEST_CASE("test_recording_for_an_agent_or_channel_action_needs_agent_prompts",
          "[test_automation_fix_api_agent_prompts]") {
    for (const std::string action_id : {"agent", "chan"}) {
        for (const std::string source : {"api", "mcp"}) {
            DYNAMIC_SECTION(action_id << " " << source) {
                ControlFixture f;
                try {
                    f.in_worker([&] { return f.control.start_recording(action_id, source); });
                    FAIL("The start was not refused.");
                } catch (const permissions::PermissionDenied& denied) {
                    CHECK(denied.permission() == permissions::AGENT_PROMPTS);
                    CHECK(denied.message() == kAgentPromptsMessage);
                }
                CHECK(f.host.calls().empty());
            }
        }
    }
}

TEST_CASE("test_with_agent_prompts_allowed_the_recording_starts",
          "[test_automation_fix_api_agent_prompts]") {
    ControlFixture f(saved_actions(true));
    CHECK(f.in_worker([&] { return f.control.start_recording("chan", "api"); }) == "job-action");
    REQUIRE(f.host.calls().size() == 1);
    CHECK(f.host.calls()[0].name == "start_action");
    CHECK(f.host.calls()[0].args == std::vector<std::string>{"chan", "api"});
}

TEST_CASE("test_other_actions_and_plain_dictation_need_no_agent_prompts",
          "[test_automation_fix_api_agent_prompts]") {
    ControlFixture f;
    CHECK(f.in_worker([&] { return f.control.start_recording("link", "mcp"); }) == "job-action");
    f.host.recording = false;
    CHECK(f.in_worker([&] { return f.control.start_recording(std::nullopt, "api"); }) == "job-new");
}

TEST_CASE("test_the_app_refusing_for_agent_prompts_is_a_403",
          "[test_automation_fix_api_agent_prompts]") {
    for (const auto& [path, body] : std::vector<std::pair<std::string, Json>>{
             {"/v1/actions/agent/run", Json{{"text", "delete the tests"}}},
             {"/v1/recording/start", Json{{"action_id", "agent"}}}}) {
        DYNAMIC_SECTION(path) {
            LiveFixture f;
            f.grant({permissions::RUN_ACTIONS, permissions::CONTROL_RECORDING});
            f.control->fail_with([] {
                throw permissions::PermissionDenied(std::string(permissions::AGENT_PROMPTS));
            });
            const Served response = f.post(path, body);
            CHECK(response.status == 403);
            CHECK(response.error() ==
                  Json{{"code", "permission_denied"}, {"message", kAgentPromptsMessage}});
        }
    }
}
