// The app-owned MCP listener (tests/test_agent_mcp.py's runtime tests) and the listener's JSON
// 404 and 405 (tests/test_automation_fix_api_not_found.py). Listeners bind 127.0.0.1 (and
// 127.0.0.2 standing in for the Tailscale address) on free ports; nothing leaves this computer.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "agentapi/history_server.h"
#include "agentapi/mcp_runtime.h"
#include "agentapi/testing/fake_tailscale_probe.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/subscription.h"
#include "base/testing/manual_executor.h"
#include "credentials/store.h"
#include "credentials/testing/memory_backend.h"
#include "http/client.h"
#include "mcp_support.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"
#include "support.h"
#include "testkit/testkit.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;
using namespace std::chrono_literals;

const std::string kMcpToken = "mcp-test-token-" + std::string(32, 'a');

// A port nothing listens on now: the one a fresh listener got, released again.
int free_port(fv::net::Reactor& reactor, const std::string& address = "127.0.0.1") {
    auto server = fv::net::HttpServer::create(reactor);
    const auto endpoint = server->listen({address, 0, std::nullopt, true});
    REQUIRE(endpoint);
    const int port = endpoint->port;
    server->stop(0ms);
    return port;
}

bool can_listen(fv::net::Reactor& reactor, const std::string& address, int port) {
    auto server = fv::net::HttpServer::create(reactor);
    const bool ok =
        server->listen({address, static_cast<std::uint16_t>(port), std::nullopt, true}).has_value();
    server->stop(0ms);
    return ok;
}

ServerStatus wait_for(McpRuntime& runtime, const std::string& state) {
    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (std::chrono::steady_clock::now() < deadline) {
        const ServerStatus status = runtime.status();
        if (status.state == state) {
            return status;
        }
        if (status.state == "error" && state != "error") {
            FAIL(status.message);
        }
        std::this_thread::sleep_for(20ms);
    }
    FAIL("Expected " << state << ", got " << runtime.status().state);
    return {};
}

int get_status(const std::string& host, int port, const std::string& token) {
    auto client = fv::http::make_curl_client();
    fv::http::Request request;
    request.url = "http://" + host + ":" + std::to_string(port) + "/v1/status";
    request.bypass_proxy = true;
    request.connect_timeout = 5s;
    if (!token.empty()) {
        request.headers.emplace_back("Authorization", "Bearer " + token);
    }
    const auto response = client->send(request, {});
    REQUIRE(response);
    return response->status;
}

// A credential store whose probe fails with `reason` until `available` is set; then it is
// "Secret Service", keeping values in memory.
class FlakyBackend final : public fv::CredentialBackend {
public:
    explicit FlakyBackend(std::string reason) : reason_(std::move(reason)) {}

    std::optional<std::string> get(std::string_view service, std::string_view name) override {
        check();
        const auto found = values.find(std::string(service) + "/" + std::string(name));
        return found == values.end() ? std::nullopt : std::optional(found->second);
    }
    void set(std::string_view service, std::string_view name, std::string_view value) override {
        check();
        values[std::string(service) + "/" + std::string(name)] = std::string(value);
    }
    bool remove(std::string_view, std::string_view) override {
        check();
        return false;
    }
    fv::CredentialStatus probe() override {
        check();
        return {true, "Secret Service", ""};
    }

    std::atomic<bool> available{false};
    std::map<std::string, std::string> values;

private:
    void check() const {
        if (!available) {
            throw std::runtime_error(reason_);
        }
    }
    std::string reason_;
};

struct Fixture {
    Fixture() : database(copy_history_fixture(root.path(), "mcp history.db")) {}

    McpRuntimeDeps deps(fv::CredentialStore& store, std::filesystem::path path = {}) {
        McpRuntimeDeps out;
        out.database = path.empty() ? database : path;
        out.reactor = &reactor;
        out.settings = &settings;
        out.credentials = &store;
        out.io = &io;
        out.tailscale = &probe;
        return out;
    }

    fv::testkit::TempDataRoot root;
    std::filesystem::path database;
    fv::net::Reactor reactor{2};
    fv::ThreadPool pool{"mcp-test", 4};
    fv::Strand io{pool, "mcp-io", 4, fv::ShutdownMode::CancelQueued};
    fv::MemorySettingsStore settings;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    FakeTailscaleProbe probe;
};

} // namespace

TEST_CASE("test_runtime_restart_uses_saved_token_and_releases_port", "[platform][test_agent_mcp]") {
    Fixture f;
    McpRuntime runtime(f.deps(f.credentials));
    runtime.start(free_port(f.reactor));
    const ServerStatus running = wait_for(runtime, "running");
    const std::string token = runtime.token();
    CHECK(token.size() == 43);
    CHECK(f.credentials.get(credential_name()) == token);
    CHECK(running.url() == "http://127.0.0.1:" + std::to_string(running.port) + "/mcp");
    CHECK(get_status("127.0.0.1", running.port, token) == 200);
    runtime.stop(true);
    CHECK(runtime.status().state == "stopped");
    CHECK(runtime.token().empty());
    CHECK(can_listen(f.reactor, "127.0.0.1", running.port));
    runtime.start(running.port);
    wait_for(runtime, "running");
    CHECK(runtime.token() == token);
    runtime.stop(true);
}

TEST_CASE("test_desktop_restore_uses_default_credentials_and_restarts",
          "[platform][test_agent_mcp]") {
    for (const bool saved : {false, true}) {
        DYNAMIC_SECTION("saved token " << saved) {
            Fixture f;
            std::optional<std::string> saved_token;
            if (saved) {
                f.credentials.set(credential_name(), kMcpToken);
                saved_token = kMcpToken;
            }
            const int port = free_port(f.reactor);
            Json values = Json::object();
            values["mcp_enabled"] = true;
            values["mcp_port"] = port;
            f.settings.set_contents(values);
            for (int round = 0; round < 2; ++round) {
                McpRuntime runtime(f.deps(f.credentials));
                runtime.restore();
                wait_for(runtime, "running");
                const std::string token = runtime.token();
                REQUIRE_FALSE(token.empty());
                CHECK(token == f.credentials.get(credential_name()));
                if (saved_token) {
                    CHECK(token == *saved_token);
                }
                saved_token = token;
                CHECK(get_status("127.0.0.1", port, "") == 401);
                CHECK(get_status("127.0.0.1", port, token) == 200);
                runtime.stop(true);
                CHECK(runtime.status().state == "stopped");
                CHECK(runtime.token().empty());
            }
        }
    }
}

TEST_CASE("restore leaves MCP off unless it was turned on", "[platform][test_agent_mcp]") {
    Fixture f;
    McpRuntime runtime(f.deps(f.credentials));
    runtime.restore();
    CHECK(runtime.status() == ServerStatus{});
    CHECK(runtime.status().message == "MCP is off.");
    CHECK_THROWS_AS(runtime.start(0), std::invalid_argument);
    CHECK_THROWS_AS(runtime.start(65536), std::invalid_argument);
}

TEST_CASE("test_occupied_port_and_missing_database_are_recoverable", "[platform][test_agent_mcp]") {
    Fixture f;
    {
        auto occupied = fv::net::HttpServer::create(f.reactor);
        const auto endpoint = occupied->listen({"127.0.0.1", 0, std::nullopt, true});
        REQUIRE(endpoint);
        McpRuntime runtime(f.deps(f.credentials));
        runtime.start(endpoint->port);
        const ServerStatus failed = wait_for(runtime, "error");
        CHECK(failed.message == "Could not listen on port " + std::to_string(endpoint->port) +
                                    ". Choose another port or close the app using it.");
        CHECK(runtime.token().empty());
        occupied->stop(0ms);
        runtime.start(free_port(f.reactor));
        wait_for(runtime, "running");
        runtime.stop(true);
    }
    McpRuntime missing(f.deps(f.credentials, f.root.path() / "missing.db"));
    missing.start(free_port(f.reactor));
    CHECK(wait_for(missing, "error").message ==
          "Could not start MCP. Verify that " + std::string(fv::identity().product) +
              "'s history database is available, then retry.");
}

TEST_CASE("test_token_store_error_names_a_missing_store_not_an_unlock", "[test_agent_mcp]") {
    fv::CredentialStore missing(std::make_shared<FlakyBackend>("Secret Service not available"),
                                fv::identity());
    CHECK(token_store_error(missing, "linux").find("No system credential store was found") !=
          std::string::npos);
    CHECK(token_store_error(missing, "linux").find("Unlock") == std::string::npos);
    fv::CredentialStore hung(std::make_shared<FlakyBackend>("the system keyring did not respond"),
                             fv::identity());
    CHECK(token_store_error(hung, "linux").find("did not respond") != std::string::npos);
    fv::CredentialStore locked(std::make_shared<FlakyBackend>("The collection is locked"),
                               fv::identity());
    CHECK(token_store_error(locked, "linux").find("Unlock your system credential store") !=
          std::string::npos);
    fv::CredentialStore memory(std::make_shared<fv::MemoryCredentialBackend>(), fv::identity());
    CHECK(token_store_error(memory, "linux").find("Unlock") != std::string::npos);
    // Elsewhere a missing store is named.
    CHECK(token_store_error(missing, "win32") == "Could not save the MCP token. " +
                                                     missing.status().backend_name +
                                                     " is not available. Start it, then retry.");
}

TEST_CASE("test_failed_credentials_do_not_start_listener", "[platform][test_agent_mcp]") {
    Fixture f;
    fv::CredentialStore missing(std::make_shared<FlakyBackend>("Secret Service not available"),
                                fv::identity());
    McpRuntimeDeps deps = f.deps(missing);
    deps.platform = "linux";
    McpRuntime runtime(deps);
    const int port = free_port(f.reactor);
    runtime.start(port);
    const std::string message = wait_for(runtime, "error").message;
    CHECK(message.find("No system credential store was found") != std::string::npos);
    CHECK(message.find("Unlock") == std::string::npos);
    CHECK(runtime.token().empty());
    CHECK(can_listen(f.reactor, "127.0.0.1", port));
}

TEST_CASE("test_locked_credential_store_asks_to_unlock", "[platform][test_agent_mcp]") {
    Fixture f;
    fv::CredentialStore locked(std::make_shared<FlakyBackend>("The collection is locked"),
                               fv::identity());
    McpRuntime runtime(f.deps(locked));
    runtime.start(free_port(f.reactor));
    CHECK(wait_for(runtime, "error").message.find("Unlock your system credential store") !=
          std::string::npos);
    CHECK(runtime.token().empty());
}

TEST_CASE("test_retry_starts_after_the_credential_store_appears", "[platform][test_agent_mcp]") {
    Fixture f;
    auto backend = std::make_shared<FlakyBackend>("Secret Service not available");
    backend->values[std::string(fv::identity().credential_service) + "/" + credential_name()] =
        kMcpToken;
    fv::CredentialStore credentials(backend, fv::identity());
    McpRuntimeDeps deps = f.deps(credentials);
    deps.platform = "linux";
    McpRuntime runtime(deps);
    const int port = free_port(f.reactor);
    runtime.start(port);
    CHECK(wait_for(runtime, "error").message.find("No system credential store was found") !=
          std::string::npos);
    CHECK(runtime.token().empty());
    backend->available = true;
    runtime.start(port);
    wait_for(runtime, "running");
    CHECK(runtime.token() == kMcpToken);
    runtime.stop(true);
}

TEST_CASE("test_tailscale_host_checks_auth_origin_and_peer", "[test_agent_mcp]") {
    fv::testkit::TempDataRoot root;
    const auto database = copy_history_fixture(root.path(), "mcp history.db");
    const auto make = [&](std::vector<std::string> allowed) {
        McpAppOptions options;
        options.database = database;
        options.token = kMcpToken;
        options.allowed_hosts = std::move(allowed);
        return std::make_unique<McpApp>(std::move(options));
    };
    const std::string host = "100.82.22.3";
    const auto status = [&](McpApp& app, std::string peer,
                            std::vector<std::pair<std::string, std::string>> extra,
                            bool authorized = true) {
        fv::net::HttpRequest request;
        request.method = "GET";
        request.path = "/v1/status";
        request.tls = false;
        request.peer = {std::move(peer), 1234};
        request.headers = {{"host", host}};
        if (authorized) {
            request.headers.emplace_back("authorization", "Bearer " + kMcpToken);
        }
        for (auto& [name, value] : extra) {
            bool replaced = false;
            for (auto& header : request.headers) {
                if (header.first == name) {
                    header.second = value;
                    replaced = true;
                }
            }
            if (!replaced) {
                request.headers.emplace_back(name, value);
            }
        }
        return app.answer(request).response.status;
    };
    auto tailscale = make({host, "jed.example.ts.net"});
    CHECK(status(*tailscale, "100.65.148.53", {}, false) == 401);
    CHECK(status(*tailscale, "100.65.148.53", {}) == 200);
    CHECK(status(*tailscale, "100.65.148.53", {{"host", "100.82.22.4"}}) == 403);
    CHECK(status(*tailscale, "100.65.148.53", {{"origin", "http://" + host}}) == 403);
    fv::net::HttpRequest initialize =
        mcp_post(kMcpToken, "initialize",
                 Json::parse(R"({"protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "remote-test", "version": "1"}})"),
                 false);
    initialize.headers[0].second = host;
    initialize.peer = {"100.65.148.53", 1234};
    CHECK(tailscale->answer(initialize).response.status == 200);

    auto one_host = make({host});
    CHECK(status(*one_host, "192.168.1.10", {}) == 403);
    auto local = make({});
    CHECK(status(*local, "127.0.0.1", {}) == 403);
}

TEST_CASE("test_tailscale_listener_keeps_local_access_and_releases_both_ports",
          "[platform][test_agent_mcp]") {
    Fixture f;
    const int port = free_port(f.reactor);
    REQUIRE(can_listen(f.reactor, "127.0.0.2", port));
    Json values = Json::object();
    values["mcp_enabled"] = true;
    values["mcp_port"] = port;
    values["mcp_tailscale_enabled"] = true;
    f.settings.set_contents(values);
    McpRuntimeDeps deps = f.deps(f.credentials);
    // A second loopback address exercises the two real sockets without a VPN.
    deps.resolve_tailscale = [](TailscaleProbe&) {
        return TailscaleListen{"127.0.0.2", "host.example.ts.net"};
    };
    McpRuntime runtime(deps);
    runtime.restore();
    const ServerStatus status = wait_for(runtime, "running");
    CHECK(status.remote_url == "http://127.0.0.2:" + std::to_string(port) + "/mcp");
    for (const std::string host : {"127.0.0.1", "127.0.0.2"}) {
        CHECK(get_status(host, port, runtime.token()) == 200);
    }
    runtime.stop(true);
    CHECK(runtime.status().remote_url.empty());
    for (const std::string host : {"127.0.0.1", "127.0.0.2"}) {
        CHECK(can_listen(f.reactor, host, port));
    }
}

TEST_CASE("test_tailscale_bind_failure_releases_loopback_socket", "[platform][test_agent_mcp]") {
    Fixture f;
    McpRuntimeDeps deps = f.deps(f.credentials);
    // TEST-NET-1 is on no interface, so the second bind fails the way it does when the Tailscale
    // address vanishes between status and listen.
    deps.resolve_tailscale = [](TailscaleProbe&) {
        return TailscaleListen{"192.0.2.1", "jed.example"};
    };
    McpRuntime runtime(deps);
    const int port = free_port(f.reactor);
    runtime.start(port, true);
    CHECK(wait_for(runtime, "error").message.find("port") != std::string::npos);
    runtime.stop();
    CHECK(can_listen(f.reactor, "127.0.0.1", port));
}

TEST_CASE("test_unavailable_tailscale_does_not_start_or_fall_back", "[platform][test_agent_mcp]") {
    Fixture f;
    fv::net::TailscaleStatus stopped;
    stopped.state = "stopped";
    fv::net::TailscaleStatus outside;
    outside.state = "running";
    outside.address = "192.168.1.3";
    for (const auto& answer : {stopped, outside}) {
        f.probe.set(answer);
        McpRuntime runtime(f.deps(f.credentials));
        const int port = free_port(f.reactor);
        runtime.start(port, true);
        CHECK(wait_for(runtime, "error").message ==
              "Tailscale is not connected. Connect Tailscale, or turn off Tailscale access, then "
              "retry.");
        CHECK(runtime.token().empty());
        CHECK(runtime.status().remote_url.empty());
        CHECK(can_listen(f.reactor, "127.0.0.1", port));
    }
}

TEST_CASE("tailscale_address takes a running Tailscale IPv4 address and its MagicDNS name",
          "[test_agent_mcp]") {
    fv::net::TailscaleStatus status;
    status.state = "running";
    status.address = "100.82.22.3";
    status.dns_name = "Jed.Example.TS.net.";
    FakeTailscaleProbe probe(status);
    const TailscaleListen listen = tailscale_address(probe);
    CHECK(listen.address == "100.82.22.3");
    CHECK(listen.dns_name == "jed.example.ts.net");
    status.address = "";
    probe.set(status);
    CHECK_THROWS_AS(tailscale_address(probe), TailscaleUnavailable);
    status.address = "fd7a:115c:a1e0::1";
    probe.set(status);
    CHECK_THROWS_AS(tailscale_address(probe), TailscaleUnavailable);
}

TEST_CASE("a stop lets requests in progress finish and is never an error",
          "[platform][test_agent_mcp]") {
    Fixture f;
    McpRuntime runtime(f.deps(f.credentials));
    // A live route that holds its request until released, so the stop happens mid-request.
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    runtime.add_routes([&](LiveRoutes& routes) {
        routes.route(
            "GET", "/v1/live/slow",
            [&, released](const fv::net::HttpRequest&, std::shared_ptr<fv::net::Responder> r) {
                entered.set_value();
                released.wait();
                r->send(200, {{"content-type", "application/json"}}, "{}");
            });
    });
    bool stopped_called = false;
    runtime.on_stopped([&] { stopped_called = true; });
    const int port = free_port(f.reactor);
    runtime.start(port);
    wait_for(runtime, "running");
    const std::string token = runtime.token();
    int slow_status = 0;
    std::thread slow([&] {
        auto client = fv::http::make_curl_client();
        fv::http::Request request;
        request.url = "http://127.0.0.1:" + std::to_string(port) + "/v1/live/slow";
        request.bypass_proxy = true;
        request.headers.emplace_back("Authorization", "Bearer " + token);
        const auto response = client->send(request, {});
        slow_status = response ? response->status : -1;
    });
    entered.get_future().wait();
    runtime.stop();
    CHECK(runtime.status().state == "stopping");
    CHECK(runtime.status().message == "Stopping MCP…");
    release.set_value();
    slow.join();
    CHECK(slow_status == 200);
    runtime.stop(true);
    CHECK(runtime.status().state == "stopped");
    CHECK(stopped_called);
}

namespace {

// An executor whose first post waits until `open` holds (at most 10 s), so a status announced on
// another thread meanwhile is queued before it; run_all() runs what was posted, in order.
class GatedExecutor final : public fv::Executor {
public:
    explicit GatedExecutor(std::function<bool()> open) : open_(std::move(open)) {}

    void post(fv::Task task) override {
        bool first = false;
        {
            std::lock_guard lock(mutex_);
            first = !gated_;
            gated_ = true;
        }
        if (first) {
            const auto deadline = std::chrono::steady_clock::now() + 10s;
            while (!open_() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(5ms);
            }
        }
        std::lock_guard lock(mutex_);
        tasks_.push_back(std::move(task));
    }
    fv::TimerHandle post_after(std::chrono::milliseconds, fv::Task) override { return {}; }
    bool is_current() const override { return false; }

    void run_all() {
        std::vector<fv::Task> tasks;
        {
            std::lock_guard lock(mutex_);
            tasks.swap(tasks_);
        }
        for (auto& task : tasks) {
            task();
        }
    }

private:
    std::function<bool()> open_;
    std::mutex mutex_;
    bool gated_ = false;
    std::vector<fv::Task> tasks_;
};

} // namespace

TEST_CASE("the last status announced after a stop that waits is the stopped one",
          "[platform][test_agent_mcp]") {
    Fixture f;
    fv::ManualExecutor ui;
    McpRuntime runtime(f.deps(f.credentials));
    std::vector<std::string> heard;
    fv::Subscription link = runtime.status_changed.connect(
        ui, [&](const ServerStatus& status) { heard.push_back(status.state); });
    runtime.start(free_port(f.reactor));
    wait_for(runtime, "running");
    // The listener's thread announces "stopped" while stop() waits for it; stop()'s own
    // "stopping" must not be the last word.
    runtime.stop(true);
    ui.run_all();
    REQUIRE_FALSE(heard.empty());
    CHECK(heard.back() == "stopped");
    CHECK(runtime.status().state == "stopped");
}

TEST_CASE("a listener that fails while start() announces it ends on the error",
          "[platform][test_agent_mcp]") {
    Fixture f;
    auto occupied = fv::net::HttpServer::create(f.reactor);
    const auto endpoint = occupied->listen({"127.0.0.1", 0, std::nullopt, true});
    REQUIRE(endpoint);
    McpRuntime* watched = nullptr;
    // start()'s "starting" is held at the executor until the listener has failed, so the
    // listener's "error" is queued first.
    GatedExecutor ui([&] { return watched != nullptr && watched->status().state == "error"; });
    McpRuntime runtime(f.deps(f.credentials));
    watched = &runtime;
    std::vector<std::string> heard;
    fv::Subscription link = runtime.status_changed.connect(
        ui, [&](const ServerStatus& status) { heard.push_back(status.state); });
    runtime.start(endpoint->port);
    wait_for(runtime, "error");
    ui.run_all();
    REQUIRE_FALSE(heard.empty());
    CHECK(heard.back() == "error");
    occupied->stop(0ms);
}

TEST_CASE("an empty saved token is replaced by a new one", "[platform][test_agent_mcp]") {
    Fixture f;
    // The store refuses to save an empty value; another writer can still leave one behind.
    auto backend = std::make_shared<fv::MemoryCredentialBackend>();
    backend->set(fv::identity().credential_service, credential_name(), "");
    fv::CredentialStore credentials(backend, fv::identity());
    McpRuntime runtime(f.deps(credentials));
    runtime.start(free_port(f.reactor));
    wait_for(runtime, "running");
    const std::string token = runtime.token();
    CHECK(token.size() == 43);
    CHECK(credentials.get(credential_name()) == token);
    runtime.stop(true);
}

TEST_CASE("Tailscale access without a probe is unavailable, not a crash",
          "[platform][test_agent_mcp]") {
    Fixture f;
    McpRuntimeDeps deps = f.deps(f.credentials);
    deps.tailscale = nullptr;
    McpRuntime runtime(deps);
    runtime.start(free_port(f.reactor), true);
    CHECK(wait_for(runtime, "error").message ==
          "Tailscale is not connected. Connect Tailscale, or turn off Tailscale access, then "
          "retry.");
}

// --- tests/test_automation_fix_api_not_found.py -------------------------------------------------

namespace {

std::unique_ptr<McpApp> not_found_app(const std::filesystem::path& database) {
    McpAppOptions options;
    options.database = database;
    options.token = "not-found-token-" + std::string(32, 'n');
    options.enabled = [] {
        return true;
    };
    auto app = std::make_unique<McpApp>(std::move(options));
    // The live API's routes as M5.16 declares them, answering nothing here.
    const auto unused = [](const fv::net::HttpRequest&, std::shared_ptr<fv::net::Responder> r) {
        r->send(200, {}, "");
    };
    LiveRoutes& routes = app->live_routes();
    routes.route("POST", "/v1/actions/{action_id}/run", unused);
    routes.route("GET", "/v1/actions", unused);
    routes.route("GET", "/v1/live/state", unused);
    routes.route("DELETE", "/v1/filters/{lease_id}", unused);
    return app;
}

fv::net::HttpRequest not_found_request(std::string method, std::string path, bool authorized) {
    fv::net::HttpRequest request;
    request.method = std::move(method);
    request.path = std::move(path);
    request.tls = false;
    request.peer = {"127.0.0.1", 50000};
    request.headers = {{"host", "127.0.0.1"}};
    if (authorized) {
        request.headers.emplace_back("authorization",
                                     "Bearer not-found-token-" + std::string(32, 'n'));
    }
    return request;
}

std::optional<std::string> header_of(const ApiResponse& response, std::string_view name) {
    for (const auto& [key, value] : response.headers) {
        if (key == name) {
            return value;
        }
    }
    return std::nullopt;
}

} // namespace

TEST_CASE("test_unknown_paths_are_json_404s", "[test_automation_fix_api_not_found]") {
    fv::testkit::TempDataRoot root;
    auto app = not_found_app(copy_history_fixture(root.path(), "live.db"));
    for (const auto& [method, path] :
         std::vector<std::pair<std::string, std::string>>{{"GET", "/v1/nope"},
                                                          {"POST", "/v1/nope/run"},
                                                          {"GET", "/v1"},
                                                          {"PUT", "/v1/transcriptions/x/y/z"}}) {
        const ApiResponse response = app->answer(not_found_request(method, path, true)).response;
        CHECK(response.status == 404);
        CHECK(header_of(response, "content-type") == "application/json");
        CHECK(fv::py::loads(response.body).at("error").at("code") == "not_found");
    }
}

TEST_CASE("test_known_paths_with_the_wrong_method_are_json_405s",
          "[test_automation_fix_api_not_found]") {
    fv::testkit::TempDataRoot root;
    auto app = not_found_app(copy_history_fixture(root.path(), "live.db"));
    for (const auto& [method, path, allowed] :
         std::vector<std::tuple<std::string, std::string, std::string>>{
             {"GET", "/v1/actions/a1/run", "POST"},
             {"POST", "/v1/actions", "GET"},
             {"DELETE", "/v1/live/state", "GET"},
             {"PUT", "/v1/filters/l1", "DELETE"},
             {"POST", "/v1/status", "GET"}}) {
        const ApiResponse response = app->answer(not_found_request(method, path, true)).response;
        CHECK(response.status == 405);
        CHECK(header_of(response, "content-type") == "application/json");
        CHECK(fv::py::loads(response.body).at("error").at("code") == "method_not_allowed");
        CHECK(header_of(response, "allow") == allowed);
    }
}

TEST_CASE("test_the_token_is_still_checked_first", "[test_automation_fix_api_not_found]") {
    fv::testkit::TempDataRoot root;
    auto app = not_found_app(copy_history_fixture(root.path(), "live.db"));
    const ApiResponse response = app->answer(not_found_request("GET", "/v1/nope", false)).response;
    CHECK(response.status == 401);
    CHECK(fv::py::loads(response.body).at("error").at("code") == "unauthorized");
}

TEST_CASE("test_mcp_and_real_routes_are_untouched", "[test_automation_fix_api_not_found]") {
    fv::testkit::TempDataRoot root;
    auto app = not_found_app(copy_history_fixture(root.path(), "live.db"));
    CHECK(app->answer(not_found_request("GET", "/v1/status", true)).response.status == 200);
    const McpHttpAnswer answer =
        app->answer(mcp_post("not-found-token-" + std::string(32, 'n'), "initialize",
                             Json::parse(R"({"protocolVersion": "2025-11-25", "capabilities": {},
                        "clientInfo": {"name": "t", "version": "1"}})"),
                             false));
    REQUIRE(answer.response.status == 200);
    CHECK(fv::py::loads(answer.response.body).at("result").at("serverInfo").at("name") ==
          std::string(fv::identity().product));
}
