#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "base/signal.h"

// The app-owned MCP listener (services/agent_mcp/runtime.py): loopback, and optionally this
// computer's Tailscale address, on one port, serving McpApp; its token kept in the credential
// store; its state for the API & MCP page.
namespace fv {
class CredentialStore;
class Executor;
class SettingsStore;
namespace net {
class Reactor;
}
} // namespace fv

namespace fv::agentapi {

class LiveRoutes;
class McpServer;
class TailscaleProbe;

inline constexpr int DEFAULT_PORT = 8767;
// The credential the token is kept under: identity().env_prefix + "MCP_TOKEN"
// (OPENWHISPER_CPP_MCP_TOKEN). The Claude Code channel reads it by this name.
std::string credential_name();

// Affinity: value type.
struct ServerStatus {
    std::string state = "stopped"; // stopped, starting, running, stopping or error
    std::string message = "MCP is off.";
    int port = DEFAULT_PORT;
    std::string remote_url; // http://<tailscale ip>:<port>/mcp, or ""
    // Always http://127.0.0.1:<port>/mcp.
    std::string url() const;
    friend bool operator==(const ServerStatus&, const ServerStatus&) = default;
};

// Tailscale is not running, has no IPv4 address, or its address is outside Tailscale's range.
class TailscaleUnavailable : public std::runtime_error {
public:
    TailscaleUnavailable() : std::runtime_error("Tailscale is not available") {}
};

// _tailscale_address: this computer's Tailscale IPv4 address and its MagicDNS name (lower case,
// trailing dot stripped, possibly ""). Throws TailscaleUnavailable; never falls back.
struct TailscaleListen {
    std::string address;
    std::string dns_name;
};
TailscaleListen tailscale_address(TailscaleProbe& probe);

// _token_store_error: the copy shown when the token cannot be read or saved. A missing store is
// not a locked one. `platform` is sys.platform ("win32", "darwin", "linux"); empty is this
// build's.
std::string token_store_error(CredentialStore& credentials, std::string_view platform = {});

// What the API & MCP page needs (M5.21 fakes it with testing/fake_mcp_control.h).
// Affinity: thread-safe.
class McpServerControl {
public:
    virtual ~McpServerControl() = default;
    virtual ServerStatus status() = 0;
    virtual std::string token() = 0; // "" unless running
    // Throws std::invalid_argument("MCP port must be between 1 and 65535.") for another port.
    virtual void start(int port, bool tailscale, std::function<bool()> access = {}) = 0;
    virtual bool session_only() const { return false; }
    virtual void stop(bool wait = false) = 0;
};

// Everything must outlive the runtime.
struct McpRuntimeDeps {
    std::filesystem::path database;
    net::Reactor* reactor = nullptr;
    SettingsStore* settings = nullptr;
    CredentialStore* credentials = nullptr;
    Executor* io = nullptr; // where requests run (they block on SQLite and tools)
    TailscaleProbe* tailscale = nullptr;
    const Clock* clock = nullptr; // null: the system clock
    // Where to listen beside loopback when Tailscale access is on; null is tailscale_address over
    // `tailscale`. Tests listen on a second loopback address through it.
    std::function<TailscaleListen(TailscaleProbe&)> resolve_tailscale;
    // sys.platform for token_store_error's copy; empty is this build's.
    std::string platform;
    bool ephemeral_token = false;
    bool allow_live_tools = true;
};

// Affinity: thread-safe; start, stop and status never block on the listener, except
// stop(wait = true). The listener runs on a thread of its own between start and stop.
class McpRuntime final : public McpServerControl {
public:
    explicit McpRuntime(McpRuntimeDeps deps);
    // Stops the listener and waits for its thread.
    ~McpRuntime() override;
    McpRuntime(const McpRuntime&) = delete;
    McpRuntime& operator=(const McpRuntime&) = delete;

    // Promotes "starting" to "running" once the listener is up.
    ServerStatus status() override;
    std::string token() override;
    // A no-op while the listener's thread lives. The token is read (or made once, with
    // token_urlsafe(32)) from the credential store, whose probe is refreshed first when it
    // failed; a failure is an "error" status, never an exception.
    void start(int port, bool tailscale = false, std::function<bool()> access = {}) override;
    // A stop that was asked for is never an error. With `wait`, joins the listener for up to
    // 7 s; it shuts down gracefully within 3 s.
    void stop(bool wait = false) override;
    // Starts with the saved port and Tailscale access when mcp_enabled is saved on.
    void restore();

    // Desktop handlers for the controls; set before start.
    void
    configure_controls(std::function<void(std::string kind, Json changes)> on_change,
                       std::function<void(std::string id, std::string title)> meeting_renamer = {});
    // Run on each listener as it starts: the live API's routes (M5.16) and MCP Events' methods
    // and capability hook (M5.12).
    void add_routes(std::function<void(LiveRoutes&)> installer);
    void add_protocol(std::function<void(McpServer&)> installer);
    // Called once a listener that served has stopped (dropping the live API's filter leases).
    void on_stopped(std::function<void()> callback);

    // Every status set, on the subscriber's executor. Statuses set at once on several threads
    // may arrive out of order, but the last one to arrive is always the status that holds:
    // whoever announced a status that was replaced meanwhile announces the newest again.
    Signal<ServerStatus> status_changed;

private:
    void serve(int port, bool tailscale, std::function<bool()> access);
    std::string saved_token();
    // With mutex_ held; returns the status' version for announce().
    std::uint64_t set_status(ServerStatus status);
    // Emits `status` (set as `version`), then the newest status again while it has changed.
    // Without mutex_ held.
    void announce(std::uint64_t version, ServerStatus status);

    McpRuntimeDeps deps_;
    std::mutex mutex_;
    std::condition_variable changed_;
    std::thread thread_;
    bool thread_alive_ = false;
    std::shared_ptr<std::atomic<bool>> stop_ = std::make_shared<std::atomic<bool>>(false);
    bool started_ = false;
    std::string token_;
    ServerStatus status_;
    std::uint64_t version_ = 0; // counts set_status calls
    std::function<void(std::string, Json)> on_change_;
    std::function<void(std::string, std::string)> meeting_renamer_;
    std::vector<std::function<void(LiveRoutes&)>> route_installers_;
    std::vector<std::function<void(McpServer&)>> protocol_installers_;
    std::vector<std::function<void()>> stopped_callbacks_;
};

} // namespace fv::agentapi
