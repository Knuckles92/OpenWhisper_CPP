#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "agentapi/history_server.h"
#include "agentapi/mcp_events.h"
#include "automation/event_bus.h"
#include "base/json.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/client.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"

// Shared by the MCP Events tests (the fixtures of tests/test_automation_mcp_events.py).
namespace fv::agentapi::test {

// test_automation_mcp_events.py TOKEN.
inline const std::string kEventsToken = "mcp-events-token-" + std::string(32, 'c');

// One POST a Receiver answered. Affinity: value type.
struct Received {
    std::string path;                           // with its query
    std::map<std::string, std::string> headers; // names in lower case, the first of each
    std::string body;
    Json payload;

    // As signing::verify reads headers.
    std::vector<std::pair<std::string, std::string>> header_pairs() const;
};

// The Receiver of test_automation_mcp_events.py: a webhook endpoint on 127.0.0.1 that records
// every POST. `verification` picks the answer to a challenge: "echo", "wrong" or an HTTP status.
// `statuses` are answered to deliveries in order, then 200. Nothing leaves the loopback
// interface.
// Affinity: thread-safe.
class Receiver {
public:
    Receiver();
    ~Receiver();
    Receiver(const Receiver&) = delete;
    Receiver& operator=(const Receiver&) = delete;

    std::uint16_t port() const { return port_; }
    // A fresh path per subscription keeps verification caching per test.
    std::string url(const std::string& query = "") const;

    void set_verification(std::variant<std::string, int> answer);
    void set_statuses(std::vector<int> statuses);

    std::vector<Received> requests() const;
    std::vector<Received> challenges() const;
    std::vector<Received> deliveries() const;
    // At least `count` deliveries; fails the test when they do not come in time.
    std::vector<Received>
    wait_for_deliveries(std::size_t count,
                        std::chrono::milliseconds timeout = std::chrono::seconds(10)) const;
    void close();

private:
    void answer(const net::HttpRequest& request, std::shared_ptr<net::Responder> responder);

    net::Reactor reactor_;
    std::unique_ptr<net::HttpServer> server_;
    std::uint16_t port_ = 0;
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::vector<Received> requests_;
    std::variant<std::string, int> verification_ = std::string("echo");
    std::vector<int> statuses_;
    bool closed_ = false;
};

// A credential store over an in-memory backend, apart from every other test's.
struct MemoryCredentials {
    std::shared_ptr<MemoryCredentialBackend> backend = std::make_shared<MemoryCredentialBackend>();
    CredentialStore store{backend, identity()};
};

// What the `client` fixture of test_automation_mcp_events.py builds: settings with MCP and MCP
// Events on, a fresh bus, an in-memory credential store, the subscriptions file in a temporary
// folder, McpEvents delivering through the real HTTP client, and McpApp over the History API
// fixture with MCP Events installed.
class EventsRig {
public:
    explicit EventsRig(Json settings = default_settings(), McpEventsOptions options = {},
                       std::vector<std::string> allowed_hosts = {});
    ~EventsRig();

    static Json default_settings();

    // A new McpEvents over the same file, store and bus: an app restart. Delivery is not started.
    void restart(McpEventsOptions options = {});

    // rpc() of the tests: one POST to /mcp from `peer`, with Host `host`; the JSON-RPC body.
    Json rpc(const std::string& method, Json params = Json::object(), bool modern = true,
             const std::string& peer = "127.0.0.1", const std::string& host = "127.0.0.1");

    std::filesystem::path subscriptions_file() const;

    testkit::TempDataRoot root;
    MemorySettingsStore settings;
    std::shared_ptr<automation::EventBus> bus;
    MemoryCredentials credentials;
    std::shared_ptr<http::Client> client;
    std::unique_ptr<McpEvents> events;
    std::unique_ptr<McpApp> app;

private:
    std::filesystem::path database_;
    std::vector<std::string> allowed_hosts_;
};

// The method's own result, without the 2026-07-28 envelope fields the SDK adds; the test fails
// without a result.
Json result_of(const Json& reply);
Json error_of(const Json& reply);
Json subscribe_params(const std::string& name, const std::string& url, const std::string& secret,
                      Json arguments = Json::object());
// deliveryStatus from refreshing, once `predicate` holds or `timeout` passed.
Json status_eventually(EventsRig& rig, const Json& params,
                       const std::function<bool(const Json&)>& predicate,
                       std::chrono::milliseconds timeout = std::chrono::seconds(5));
// Publishes transcription.completed as publish_completed does.
automation::Event publish_completed(automation::EventBus& bus, const std::string& text,
                                    const std::string& mode = "dictation",
                                    const Json& action_id = nullptr);
// Polls `predicate` until it holds or `timeout` passes; its last answer.
bool eventually(const std::function<bool()>& predicate,
                std::chrono::milliseconds timeout = std::chrono::seconds(5));

} // namespace fv::agentapi::test
