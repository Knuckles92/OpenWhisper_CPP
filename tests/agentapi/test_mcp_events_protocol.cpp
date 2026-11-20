// MCP Events on the wire: tests/fixtures/transcripts/mcp_events.json, recorded from OpenWhisper's
// services/agent_mcp/events_ext.py on its in-app MCP app (tools/parity/record_transcripts.py,
// scenario tools/parity/transcripts/mcp_events.py), replayed against agentapi::McpApp::answer
// with agentapi::McpEvents installed, over the same settings, callback answers and name lookups.
#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <map>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <variant>

#include "agentapi/history_server.h"
#include "agentapi/mcp_events.h"
#include "automation/event_bus.h"
#include "base/clock.h"
#include "events_support.h"
#include "golden/golden.h"
#include "http/client.h"
#include "pyport/json.h"
#include "pyport/regex.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;

Json load_transcript(std::string_view name) {
    const auto path = fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" /
                      (std::string(name) + ".json");
    std::ifstream in(path, std::ios::binary);
    REQUIRE(in);
    std::ostringstream text;
    text << in.rdbuf();
    return fv::py::loads(text.str());
}

// The scenario's _Callback: every callback URL, answering as `answer` says and keeping what it
// was sent. Nothing leaves the process.
class FakeCallback final : public fv::http::Client {
public:
    std::expected<fv::http::Response, fv::http::Error> send(const fv::http::Request& request,
                                                            const fv::CancelToken&,
                                                            fv::http::BodySink* sink) override {
        const Json payload = fv::py::loads(std::get<std::string>(request.body));
        Json answer;
        {
            std::lock_guard lock(mutex_);
            Json call = Json::object();
            call["url"] = request.url;
            call["connect_to"] = request.pinned_address ? Json(*request.pinned_address) : Json();
            call["type"] =
                payload.is_object() && payload.contains("type") ? payload.at("type") : Json();
            calls_.push_back(std::move(call));
            answer = answer_;
        }
        if (answer == "timeout") {
            return std::unexpected(fv::http::Error{fv::http::ErrorKind::Timeout, "timed out"});
        }
        if (answer == "refused") {
            return std::unexpected(fv::http::Error{fv::http::ErrorKind::Connect, "refused"});
        }
        if (answer == "tls") {
            return std::unexpected(fv::http::Error{fv::http::ErrorKind::Tls, "handshake"});
        }
        fv::http::Response response;
        response.status = 200;
        std::string body = "{}";
        if (answer == "echo") {
            Json echo = Json::object();
            echo["challenge"] = payload.at("challenge");
            body = fv::py::dumps(echo);
        } else if (answer == "wrong") {
            body = R"({"challenge": "not it"})";
        } else if (answer == "garbage") {
            body = "not json";
        } else if (answer == "number") {
            body = R"({"challenge": 5})";
        } else if (answer.is_number_integer()) {
            response.status = answer.get<int>();
        }
        response.headers = {{"content-type", "application/json"}};
        if (sink != nullptr) {
            sink->on_head(response);
            sink->write(std::span<const char>(body.data(), body.size()));
        } else {
            response.body = body;
        }
        return response;
    }

    void set_answer(Json answer) {
        std::lock_guard lock(mutex_);
        answer_ = std::move(answer);
    }
    Json take_calls() {
        std::lock_guard lock(mutex_);
        Json out = std::move(calls_);
        calls_ = Json::array();
        return out;
    }

private:
    std::mutex mutex_;
    Json answer_ = "echo";
    Json calls_ = Json::array();
};

std::vector<std::string> strings(const Json& array) {
    std::vector<std::string> out;
    for (const Json& item : array) {
        out.push_back(item.get<std::string>());
    }
    return out;
}

// The subscription id a recorded events/subscribe or events/unsubscribe names, under OpenWhisper's
// principal and under OpenWhisper_CPP's; nullopt when the request is refused before an id exists.
std::optional<std::pair<std::string, std::string>> ids_of(const std::string& body) {
    Json message;
    try {
        message = fv::py::loads(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!message.is_object() || !message.contains("params") || !message.at("params").is_object()) {
        return std::nullopt;
    }
    const Json& params = message.at("params");
    try {
        const std::string name = events_ext::event_name(params.value("name", Json()));
        const Json arguments =
            events_ext::normalize_arguments(name, params.value("arguments", Json()));
        const Json delivery = params.value("delivery", Json());
        if (!delivery.is_object() || !delivery.contains("url") || !delivery.at("url").is_string()) {
            return std::nullopt;
        }
        const std::string url = delivery.at("url").get<std::string>();
        return std::pair{subscription_id(events_ext::legacy_principal(), url, name, arguments),
                         subscription_id(events_ext::principal(), url, name, arguments)};
    } catch (const events_ext::EventsError&) {
        return std::nullopt;
    }
}

std::string replace_all(std::string text, const std::string& from, const std::string& to) {
    for (auto at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size())) {
        text.replace(at, from.size(), to);
    }
    return text;
}

} // namespace

TEST_CASE("MCP Events answers every recorded request as OpenWhisper did", "[golden]") {
    const Json transcript = load_transcript("mcp_events");
    const Json& context = transcript.at("context");
    const std::string token = context.at("token").get<std::string>();
    REQUIRE(context.at("principal") == events_ext::legacy_principal());
    fv::testkit::TempDataRoot root;
    const auto database =
        copy_history_fixture(root.path(), context.at("database_name").get<std::string>());
    fv::MemorySettingsStore settings(context.at("initial").at("settings"));
    fv::automation::EventBus bus;
    MemoryCredentials credentials;
    auto callback = std::make_shared<FakeCallback>();

    McpEventsOptions options;
    // The golden was recorded on a host where binding IPv4 multicast does not make
    // the address a local interface. Model that host independently of this machine.
    options.network.is_this_computer = [](const fv::net::IpAddress& address) {
        const auto text = address.to_string();
        return fv::net::is_loopback(address) || text == "100.100.100.100" ||
               text == "fd7a:115c:a1e0::53" || text == "100.82.22.3";
    };
    const Json names = context.at("names");
    options.network.resolve =
        [names](
            std::string_view host) -> std::expected<std::vector<fv::net::IpAddress>, std::string> {
        const std::string key(host);
        if (!names.contains(key)) {
            return std::unexpected(std::string("unknown name"));
        }
        std::vector<fv::net::IpAddress> out;
        for (const Json& address : names.at(key)) {
            out.push_back(*fv::net::parse_address(address.get<std::string>()));
        }
        return out;
    };
    McpEvents events(settings, credentials.store, bus, root.path() / "mcp_event_subscriptions.json",
                     callback, fv::system_clock(), options);
    events.start_delivery();

    std::string app_name = context.at("initial").at("app").get<std::string>();
    std::string peer = context.at("initial").at("peer").get<std::string>();
    callback->set_answer(context.at("initial").at("answer"));
    const auto make = [&](std::vector<std::string> allowed) {
        McpAppOptions app_options;
        app_options.database = database;
        app_options.token = token;
        app_options.allowed_hosts = std::move(allowed);
        app_options.enabled = [] {
            return true;
        };
        app_options.settings = &settings;
        auto app = std::make_unique<McpApp>(std::move(app_options));
        events.install(app->protocol());
        return app;
    };
    auto main_app = make({});
    auto tailscale_app = make(strings(context.at("tailscale_hosts")));

    // OpenWhisper's subscription ids -> OpenWhisper_CPP's, learned from the requests.
    std::map<std::string, std::string> ids;
    static const fv::py::Regex refresh_before(R"re("refreshBefore":"([^"]*)")re");
    static const fv::py::Regex stamp(R"(\d{4}-\d\d-\d\dT\d\d:\d\d:\d\dZ)");

    const Json& exchanges = transcript.at("exchanges");
    std::size_t replayed = 0;
    for (std::size_t index = 0; index < exchanges.size(); ++index) {
        const Json& exchange = exchanges[index];
        const Json& recorded = exchange.at("request");
        const Json& expected = exchange.at("response");
        INFO("exchange " << index << ": " << recorded.at("path").get<std::string>() << " "
                         << recorded.at("body").get<std::string>().substr(0, 400));
        if (recorded.at("path") == "/__scenario__") {
            const Json change = fv::py::loads(recorded.at("body").get<std::string>());
            if (change.contains("app")) {
                app_name = change.at("app").get<std::string>();
            }
            if (change.contains("peer")) {
                peer = change.at("peer").get<std::string>();
            }
            if (change.contains("settings")) {
                settings.set_contents(change.at("settings"));
            }
            if (change.contains("answer")) {
                callback->set_answer(change.at("answer"));
            }
            if (change.value("clear", false)) {
                events.clear_subscriptions();
            }
            continue;
        }
        if (recorded.at("path") == "/__calls__") {
            const Json calls = callback->take_calls();
            CHECK(fv::py::py_equal(fv::py::loads(expected.at("body").get<std::string>()), calls));
            continue;
        }
        const std::string request_body = recorded.at("body").get<std::string>();
        if (const auto mapped = ids_of(request_body)) {
            ids[mapped->first] = mapped->second;
        }

        fv::net::HttpRequest request;
        request.method = recorded.at("method").get<std::string>();
        request.path = recorded.at("path").get<std::string>();
        request.tls = false;
        request.peer = {peer, 50000};
        request.body = request_body;
        for (const Json& pair : recorded.at("headers")) {
            request.headers.emplace_back(pair[0].get<std::string>(), pair[1].get<std::string>());
        }
        McpApp& app = app_name == "tailscale" ? *tailscale_app : *main_app;
        const McpHttpAnswer answer = app.answer(request);
        ++replayed;

        const std::string recorded_body = expected.at("body").get<std::string>();
        std::string expected_body = fv::golden::rebrand(recorded_body);
        for (const auto& [theirs, ours] : ids) {
            expected_body = replace_all(expected_body, theirs, ours);
        }
        std::string body = answer.response.body;
        if (exchange.contains("normalized")) {
            const auto found = refresh_before.search(body);
            REQUIRE(found);
            CHECK(stamp.fullmatch(found->group(1)));
            body = replace_all(body, "\"refreshBefore\":\"" + found->group(1) + "\"",
                               "\"refreshBefore\":\"<normalized>\"");
        }
        CHECK(answer.response.status == expected.at("status").get<int>());
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
        CHECK(answer.response.headers == expected_headers);
        CHECK(body == expected_body);
        if (content_length) {
            CHECK(answer.response.body.size() ==
                  *content_length + expected_body.size() - recorded_body.size());
        }
    }
    CHECK(replayed > 250);
    events.stop_delivery();
    bus.close();
}
