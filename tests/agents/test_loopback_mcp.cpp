// The loopback MCP server (meeting/agent/installed/mcp_server.py): the ported MCP tests of
// tests/test_installed_agent_drivers.py over a real loopback socket, and the replay of
// tests/fixtures/transcripts/loopback_mcp.json (tools/parity/transcripts/loopback_mcp.py),
// recorded from OpenWhisper's own server, both through answer() and over the socket.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "agents/loopback_mcp.h"
#include "http/client.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "pass_support.h"
#include "pyport/json.h"
#include "pyport/text.h"
#include "testkit/testkit.h"

using fv::Json;
using fv::agents::LoopbackMcpServer;
using fv::agents::McpEndpoint;
using fv::agents_test::meeting_tools;
using fv::agents_test::patch_call;
using fv::agents_test::rebranded;
using fv::agents_test::Recorder;

namespace {

struct Reply {
    int status = 0;
    fv::http::Headers headers;
    std::string body;
    Json json() const { return body.empty() ? Json(nullptr) : fv::py::loads(body); }
};

// One request to the server over loopback, as urllib sends it.
Reply post_raw(const std::string& url, std::string body, fv::http::Headers headers,
               std::string method = "POST") {
    static const auto client = fv::http::make_curl_client();
    fv::http::Request request;
    request.method = std::move(method);
    request.url = url;
    request.headers = std::move(headers);
    if (!body.empty() || request.method == "POST") {
        request.body = std::move(body);
    }
    request.bypass_proxy = true;
    request.connect_timeout = std::chrono::milliseconds(5000);
    request.idle_timeout = std::chrono::milliseconds(10000);
    const auto response = client->send(request, {});
    REQUIRE(response.has_value());
    return Reply{response->status, response->headers, response->body};
}

// _post of the Python tests.
Reply post(const std::string& url, const Json& message, std::optional<std::string> token = {},
           fv::http::Headers headers = {}) {
    headers.emplace_back("Content-Type", "application/json");
    if (token) {
        headers.emplace_back("Authorization", "Bearer " + *token);
    }
    return post_raw(url, message.dump(), std::move(headers));
}

Json rpc(std::string_view method, std::optional<int> id = std::nullopt, Json params = nullptr) {
    Json message{{"jsonrpc", "2.0"}};
    if (id) {
        message["id"] = *id;
    }
    message["method"] = method;
    if (!params.is_null()) {
        message["params"] = std::move(params);
    }
    return message;
}

struct Server {
    fv::net::Reactor reactor{2};
    LoopbackMcpServer server{reactor};
    Server() { server.start(); }
    ~Server() { server.stop(); }
};

std::string decode_base64(std::string_view text) {
    static constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char c : text) {
        const auto at = alphabet.find(c);
        if (at == std::string_view::npos) {
            continue;
        }
        buffer = (buffer << 6) | static_cast<unsigned>(at);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>((buffer >> bits) & 0xFF);
        }
    }
    return out;
}

const Json& transcript() {
    static const Json document = fv::py::loads(fv::agents_test::read_all(
        fv::testkit::source_root() / "tests" / "fixtures" / "transcripts" / "loopback_mcp.json"));
    return document;
}

std::string body_of(const Json& recorded) {
    if (recorded.contains("body_base64")) {
        return decode_base64(recorded.at("body_base64").get<std::string>());
    }
    return recorded.value("body", std::string());
}

std::optional<std::string> header_value(const fv::net::Headers& headers, std::string_view name) {
    const auto found = fv::net::header(headers, name);
    return found ? std::optional<std::string>(std::string(*found)) : std::nullopt;
}

// The transcript's endpoints, opened on a server with the scenario's tool handler: it records
// each call and fails "explode" with RuntimeError("tool broke é").
struct Replay {
    Server running;
    Recorder recorder;
    std::shared_ptr<McpEndpoint> endpoint;
    std::shared_ptr<McpEndpoint> closed;

    Replay() {
        auto record = recorder.handler();
        const auto handler = [record](std::string_view name, const Json& args, const Json& meta) {
            auto answer = record(name, args, meta);
            if (name == "explode") {
                throw std::runtime_error("tool broke \xC3\xA9");
            }
            return answer;
        };
        endpoint = running.server.open_endpoint(transcript().at("tools"), handler);
        closed = running.server.open_endpoint(transcript().at("tools"), handler);
    }

    std::string fill(std::string text) const {
        for (const auto& [name, value] :
             {std::pair<std::string, std::string>{"{closed_token}", closed->token},
              {"{token}", endpoint->token}}) {
            for (auto at = text.find(name); at != std::string::npos; at = text.find(name)) {
                text.replace(at, name.size(), value);
            }
        }
        return text;
    }

    // The scenario's actions between requests.
    void act(const std::string& action) {
        if (action == "deactivate") {
            std::lock_guard lock(closed->lock);
            closed->active = false;
        } else {
            running.server.close_endpoint(closed);
        }
    }

    void check_calls() const {
        const auto calls = recorder.calls();
        const auto& expected = transcript().at("calls");
        REQUIRE(calls.size() == expected.size());
        for (std::size_t i = 0; i < calls.size(); ++i) {
            CHECK(Json(std::get<0>(calls[i])) == expected[i][0]);
            CHECK(std::get<1>(calls[i]) == expected[i][1]);
            CHECK(std::get<2>(calls[i]) == expected[i][2]);
        }
        CHECK(endpoint->listed.is_set() == transcript().at("listed").get<bool>());
    }
};

void check_answer(std::size_t index, const Json& expected, int status, const std::string& body,
                  const std::optional<std::string>& content_type) {
    INFO("exchange " << index << ": " << expected.at("request").at("method") << " "
                     << expected.at("request").at("path"));
    const auto& response = expected.at("response");
    CHECK(status == response.at("status").get<int>());
    CHECK(body == fv::agents_test::rebrand_text(body_of(response)));
    std::optional<std::string> recorded_type;
    for (const auto& pair : response.at("headers")) {
        if (fv::py::lower(pair[0].get<std::string>()) == "content-type") {
            recorded_type = pair[1].get<std::string>();
        }
    }
    CHECK(content_type == recorded_type);
}

} // namespace

TEST_CASE("mcp round trip", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_mcp_round_trip
    Server running;
    Recorder handler;
    const auto endpoint = running.server.open_endpoint(meeting_tools(), handler.handler());
    const auto url = running.server.url(*endpoint);
    const auto init =
        post(url, rpc("initialize", 1, {{"protocolVersion", "2025-06-18"}}), endpoint->token);
    CHECK(init.status == 200);
    CHECK(init.json().at("result").at("protocolVersion") == "2025-06-18");
    CHECK(post(url, rpc("notifications/initialized"), endpoint->token).status == 202);
    const auto listed = post(url, rpc("tools/list", 2), endpoint->token).json();
    std::vector<std::string> names;
    for (const auto& tool : listed.at("result").at("tools")) {
        names.push_back(tool.at("name").get<std::string>());
    }
    CHECK(std::find(names.begin(), names.end(), "patch_state") != names.end());
    CHECK(std::find(names.begin(), names.end(), "ask_question") != names.end());
    CHECK(endpoint->listed.is_set());
    auto params = patch_call();
    params["_meta"] = {{"k", "v"}};
    const auto called = post(url, rpc("tools/call", 3, params), endpoint->token).json();
    CHECK(called.at("result").at("isError") == false);
    const auto calls = handler.calls();
    REQUIRE(calls.size() == 1);
    CHECK(std::get<0>(calls[0]) == "patch_state");
    CHECK(std::get<1>(calls[0]) == patch_call().at("arguments"));
    CHECK(std::get<2>(calls[0]) == Json{{"k", "v"}});
    // A newer client probes first; the unknown method sends it to initialize.
    const auto discover = post(url, rpc("server/discover", 4), endpoint->token).json();
    CHECK(discover.at("error").at("code") == -32601);
}

TEST_CASE("mcp refuses strangers", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_mcp_refuses_strangers
    const auto [token_kind, origin, status] = GENERATE(table<std::string, std::string, int>({
        {"wrong", "", 401},
        {"none", "", 401},
        {"right", "https://evil.example", 403},
    }));
    Server running;
    Recorder handler;
    const auto endpoint = running.server.open_endpoint(meeting_tools(), handler.handler());
    std::optional<std::string> token;
    if (token_kind == "wrong") {
        token = "nope";
    } else if (token_kind == "right") {
        token = endpoint->token;
    }
    fv::http::Headers headers;
    if (!origin.empty()) {
        headers.emplace_back("Origin", origin);
    }
    CHECK(post(running.server.url(*endpoint), rpc("tools/list", 1), token, headers).status ==
          status);
    CHECK(handler.calls().empty());
}

TEST_CASE("a closed endpoint has no authority", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_a_closed_endpoint_has_no_authority
    Server running;
    Recorder handler;
    const auto endpoint = running.server.open_endpoint(meeting_tools(), handler.handler());
    const auto url = running.server.url(*endpoint);
    const auto token = endpoint->token;
    {
        std::lock_guard lock(endpoint->lock);
        endpoint->active = false; // closed, but the agent still holds the token
    }
    const auto reply = post(url, rpc("tools/call", 1, patch_call()), token).json();
    CHECK(reply.at("result").at("isError") == true);
    CHECK(reply.at("result").at("content").at(0).at("text").get<std::string>().find("ended") !=
          std::string::npos);
    CHECK(handler.calls().empty());
    running.server.close_endpoint(endpoint);
    CHECK(post(url, rpc("tools/list", 2), token).status == 401);
}

TEST_CASE("unknown tools are refused", "[test_installed_agent_drivers][platform]") {
    // from tests/test_installed_agent_drivers.py::test_unknown_tools_are_refused
    Server running;
    Recorder handler;
    const auto endpoint = running.server.open_endpoint(meeting_tools(), handler.handler());
    const auto reply = post(running.server.url(*endpoint),
                            rpc("tools/call", 1, {{"name", "Bash"}, {"arguments", Json::object()}}),
                            endpoint->token)
                           .json();
    CHECK(reply.at("result").at("isError") == true);
    CHECK(handler.calls().empty());
}

TEST_CASE("the server answers as OpenWhisper's did, request by request", "[loopback_mcp][golden]") {
    Replay replay;
    const auto& exchanges = transcript().at("exchanges");
    for (std::size_t i = 0; i < exchanges.size(); ++i) {
        const auto& exchange = exchanges[i];
        if (exchange.contains("action")) {
            replay.act(exchange.at("action").get<std::string>());
            continue;
        }
        const auto& recorded = exchange.at("request");
        fv::net::HttpRequest request;
        request.method = recorded.at("method").get<std::string>();
        auto path = replay.fill(recorded.at("path").get<std::string>());
        request.path = path.substr(0, path.find('?'));
        for (const auto& pair : recorded.at("headers")) {
            request.headers.emplace_back(pair[0].get<std::string>(),
                                         replay.fill(pair[1].get<std::string>()));
        }
        request.body = body_of(recorded);
        request.tls = false;
        // The server reads no body past the cap: the request says only how long it is.
        if (const auto length = header_value(request.headers, "Content-Length");
            length && std::stoll(*length) > 4 * 1024 * 1024) {
            request.body_too_large = true;
        }
        auto reply = replay.running.server.answer(request);
        check_answer(i, exchange, reply.status, reply.body,
                     header_value(reply.headers, "Content-Type"));
    }
    replay.check_calls();
}

TEST_CASE("the transcript replays over the loopback socket", "[loopback_mcp][platform]") {
    Replay replay;
    const auto& exchanges = transcript().at("exchanges");
    const auto base = "http://127.0.0.1:" + std::to_string(replay.running.server.port());
    for (std::size_t i = 0; i < exchanges.size(); ++i) {
        const auto& exchange = exchanges[i];
        if (exchange.contains("action")) {
            replay.act(exchange.at("action").get<std::string>());
            continue;
        }
        const auto& recorded = exchange.at("request");
        fv::http::Headers headers;
        bool trailing_space = false;
        std::optional<long long> declared;
        for (const auto& pair : recorded.at("headers")) {
            const auto name = pair[0].get<std::string>();
            const auto value = replay.fill(pair[1].get<std::string>());
            if (name == "Content-Length") {
                declared = std::stoll(value);
                continue; // the client counts the body itself
            }
            trailing_space = trailing_space || (!value.empty() && value.back() == ' ');
            headers.emplace_back(name, value);
        }
        if (trailing_space) {
            // HTTP parsers trim a header value's trailing whitespace, as Beast does; Python's
            // http.server kept it. answer() is checked with the value as recorded.
            continue;
        }
        auto body = body_of(recorded);
        if (declared && *declared > static_cast<long long>(body.size())) {
            body.assign(static_cast<std::size_t>(*declared), ' ');
        }
        const auto reply =
            post_raw(base + replay.fill(recorded.at("path").get<std::string>()), std::move(body),
                     std::move(headers), recorded.at("method").get<std::string>());
        std::optional<std::string> content_type;
        if (const auto found = fv::http::find_header(reply.headers, "Content-Type")) {
            content_type = *found;
        }
        check_answer(i, exchange, reply.status, reply.body, content_type);
    }
    replay.check_calls();
}

TEST_CASE("a HEAD request is not a method the server has", "[loopback_mcp][platform]") {
    Server running;
    Recorder handler;
    const auto endpoint = running.server.open_endpoint(Json::array(), handler.handler());
    const auto reply = post_raw(running.server.url(*endpoint), "", {}, "HEAD");
    CHECK(reply.status == 501);
    CHECK(reply.body.empty());
}

TEST_CASE("closing an endpoint waits for the call it is running", "[loopback_mcp][platform]") {
    Server running;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false;
    bool release = false;
    const auto endpoint = running.server.open_endpoint(
        Json::array({Json{{"name", "slow"}}}), [&](std::string_view, const Json&, const Json&) {
            std::unique_lock lock(mutex);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return release; });
            return std::pair<std::string, bool>("finished", false);
        });
    auto call = std::async(std::launch::async, [&] {
        return post(running.server.url(*endpoint),
                    rpc("tools/call", 1, {{"name", "slow"}, {"arguments", Json::object()}}),
                    endpoint->token)
            .json();
    });
    {
        std::unique_lock lock(mutex);
        REQUIRE(changed.wait_for(lock, std::chrono::seconds(10), [&] { return entered; }));
    }
    std::atomic<bool> closed{false};
    auto closer = std::async(std::launch::async, [&] {
        running.server.close_endpoint(endpoint);
        closed = true;
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    CHECK_FALSE(closed.load());
    {
        std::lock_guard lock(mutex);
        release = true;
    }
    changed.notify_all();
    closer.get();
    CHECK(closed.load());
    CHECK(call.get().at("result").at("content").at(0).at("text") == "finished");
    const auto later = running.server.answer(fv::net::HttpRequest{
        .method = "POST",
        .path = "/mcp/" + endpoint->token,
        .headers = {{"Authorization", "Bearer " + endpoint->token}, {"Content-Length", "2"}},
        .body = "{}",
        .tls = false});
    CHECK(later.status == 401);
}

TEST_CASE("a stopped server closes every endpoint and its port", "[loopback_mcp][platform]") {
    fv::net::Reactor reactor{2};
    LoopbackMcpServer server{reactor};
    CHECK_FALSE(server.running());
    CHECK(server.port() == 0);
    server.start();
    server.start(); // a second start does nothing
    REQUIRE(server.running());
    const auto port = server.port();
    CHECK(port != 0);
    const auto endpoint = server.open_endpoint(Json::array(), nullptr);
    CHECK(server.url(*endpoint) ==
          "http://127.0.0.1:" + std::to_string(port) + "/mcp/" + endpoint->token);
    CHECK(endpoint->token.size() == 32); // secrets.token_urlsafe(24)
    server.stop();
    CHECK_FALSE(server.running());
    CHECK(server.port() == 0);
    CHECK_FALSE(endpoint->active.load());
}
