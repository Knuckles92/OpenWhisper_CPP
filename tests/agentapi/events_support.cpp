#include "events_support.h"

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <thread>
#include <utility>

#include "agentapi/mcp_server.h"
#include "automation/catalog.h"
#include "base/clock.h"
#include "base/ids.h"
#include "mcp_support.h"
#include "pyport/json.h"
#include "support.h"

namespace fv::agentapi::test {

std::vector<std::pair<std::string, std::string>> Received::header_pairs() const {
    return {headers.begin(), headers.end()};
}

Receiver::Receiver() : reactor_(2), server_(net::HttpServer::create(reactor_)) {
    server_->set_fallback(
        [this](const net::HttpRequest& request, bool, std::shared_ptr<net::Responder> responder) {
            answer(request, std::move(responder));
        });
    const auto endpoint = server_->listen({.address = "127.0.0.1", .port = 0});
    if (!endpoint) {
        throw std::runtime_error("the receiver could not listen: " + endpoint.error());
    }
    port_ = endpoint->port;
}

Receiver::~Receiver() {
    close();
}

void Receiver::close() {
    {
        std::lock_guard lock(mutex_);
        if (closed_) {
            return;
        }
        closed_ = true;
    }
    changed_.notify_all();
    server_->stop(std::chrono::milliseconds(200));
    reactor_.stop(std::chrono::milliseconds(200));
}

std::string Receiver::url(const std::string& query) const {
    return "http://127.0.0.1:" + std::to_string(port_) + "/hook/" + uuid4_hex() + query;
}

void Receiver::set_verification(std::variant<std::string, int> answer) {
    std::lock_guard lock(mutex_);
    verification_ = std::move(answer);
}

void Receiver::set_statuses(std::vector<int> statuses) {
    std::lock_guard lock(mutex_);
    statuses_ = std::move(statuses);
}

void Receiver::answer(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder) {
    Received received;
    received.path = request.path;
    if (!request.query.empty()) {
        std::string query;
        for (const auto& [name, value] : request.query) {
            query += (query.empty() ? "?" : "&") + name + "=" + value;
        }
        received.path += query;
    }
    for (const auto& [name, value] : request.headers) {
        std::string lower = name;
        std::ranges::transform(lower, lower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        received.headers.emplace(std::move(lower), value);
    }
    received.body = request.body;
    try {
        received.payload = py::loads(request.body);
    } catch (const std::exception&) {
        received.payload = nullptr;
    }
    int status = 200;
    std::string body = "{}";
    {
        std::lock_guard lock(mutex_);
        const bool verification = received.payload.is_object() &&
                                  received.payload.value("type", Json()) == "verification";
        if (verification) {
            if (const auto* mode = std::get_if<std::string>(&verification_)) {
                if (*mode == "echo") {
                    Json echo = Json::object();
                    echo["challenge"] = received.payload.at("challenge");
                    body = py::dumps(echo);
                } else {
                    body = R"({"challenge": "not it"})";
                }
            } else {
                status = std::get<int>(verification_);
            }
        } else if (!statuses_.empty()) {
            status = statuses_.front();
            statuses_.erase(statuses_.begin());
        }
        requests_.push_back(received);
    }
    changed_.notify_all();
    responder->send(status, {{"Content-Type", "application/json"}}, std::move(body));
}

std::vector<Received> Receiver::requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
}

std::vector<Received> Receiver::challenges() const {
    std::vector<Received> out;
    for (const Received& request : requests()) {
        if (request.payload.is_object() &&
            request.payload.value("type", Json()) == "verification") {
            out.push_back(request);
        }
    }
    return out;
}

std::vector<Received> Receiver::deliveries() const {
    std::vector<Received> out;
    for (const Received& request : requests()) {
        if (!(request.payload.is_object() &&
              request.payload.value("type", Json()) == "verification")) {
            out.push_back(request);
        }
    }
    return out;
}

std::vector<Received> Receiver::wait_for_deliveries(std::size_t count,
                                                    std::chrono::milliseconds timeout) const {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        auto got = deliveries();
        if (got.size() >= count) {
            return got;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            FAIL("expected " << count << " deliveries, got " << got.size());
        }
        std::unique_lock lock(mutex_);
        changed_.wait_for(lock, std::chrono::milliseconds(20));
    }
}

Json EventsRig::default_settings() {
    return Json::parse(R"({"mcp_enabled": true, "mcp_events_enabled": true})");
}

EventsRig::EventsRig(Json initial, McpEventsOptions options, std::vector<std::string> allowed_hosts)
    : settings(initial), bus(std::make_shared<automation::EventBus>()),
      client(http::make_curl_client()), allowed_hosts_(std::move(allowed_hosts)) {
    database_ = copy_history_fixture(root.path(), "history.db");
    restart(std::move(options));
}

EventsRig::~EventsRig() {
    app.reset();
    events.reset();
    bus->close();
}

void EventsRig::restart(McpEventsOptions options) {
    app.reset();
    events.reset();
    events = std::make_unique<McpEvents>(settings, credentials.store, *bus, subscriptions_file(),
                                         client, system_clock(), std::move(options));
    McpAppOptions app_options;
    app_options.database = database_;
    app_options.token = kEventsToken;
    app_options.allowed_hosts = allowed_hosts_;
    app_options.enabled = [] {
        return true;
    };
    app_options.settings = &settings;
    app = std::make_unique<McpApp>(std::move(app_options));
    events->install(app->protocol());
}

std::filesystem::path EventsRig::subscriptions_file() const {
    return root.path() / "mcp_event_subscriptions.json";
}

Json EventsRig::rpc(const std::string& method, Json params, bool modern, const std::string& peer,
                    const std::string& host) {
    net::HttpRequest request = mcp_post(kEventsToken, method, std::move(params), modern, 7);
    request.peer = {peer, 50000};
    for (auto& [name, value] : request.headers) {
        if (name == "host") {
            value = host;
        }
    }
    const McpHttpAnswer answer = app->answer(request);
    INFO(answer.response.body);
    return py::loads(answer.response.body);
}

Json result_of(const Json& reply) {
    INFO(py::dumps(reply));
    REQUIRE(reply.contains("result"));
    Json out = Json::object();
    for (const auto& [key, value] : reply.at("result").items()) {
        if (key != "resultType" && key != "_meta") {
            out[key] = value;
        }
    }
    return out;
}

Json error_of(const Json& reply) {
    INFO(py::dumps(reply));
    REQUIRE(reply.contains("error"));
    return reply.at("error");
}

Json subscribe_params(const std::string& name, const std::string& url, const std::string& secret,
                      Json arguments) {
    Json delivery = Json::object();
    delivery["mode"] = "webhook";
    delivery["url"] = url;
    delivery["secret"] = secret;
    Json params = Json::object();
    params["name"] = name;
    params["arguments"] = std::move(arguments);
    params["delivery"] = std::move(delivery);
    params["cursor"] = nullptr;
    return params;
}

Json status_eventually(EventsRig& rig, const Json& params,
                       const std::function<bool(const Json&)>& predicate,
                       std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        const Json status = result_of(rig.rpc("events/subscribe", params)).at("deliveryStatus");
        if (predicate(status) || std::chrono::steady_clock::now() > deadline) {
            return status;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
}

automation::Event publish_completed(automation::EventBus& bus, const std::string& text,
                                    const std::string& mode, const Json& action_id) {
    Json data = Json::object();
    data["job_id"] = random_hex(16);
    data["mode"] = mode;
    data["action_id"] = action_id;
    data["source"] = "hotkey";
    data["routed"] = false;
    data["text"] = text;
    data["raw_text"] = nullptr;
    data["language"] = "en";
    data["app"] = nullptr;
    data["cleanup"] = nullptr;
    data["audio_s"] = 1.5;
    return bus.publish_event(automation::catalog::TRANSCRIPTION_COMPLETED, std::move(data));
}

bool eventually(const std::function<bool()>& predicate, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return predicate();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return true;
}

} // namespace fv::agentapi::test
