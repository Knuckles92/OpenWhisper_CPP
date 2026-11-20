#include "agentapi/mcp_runtime.h"

#include <chrono>
#include <format>
#include <typeinfo>
#include <utility>

#include "agentapi/history_server.h"
#include "agentapi/tailscale_probe.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/ids.h"
#include "base/log.h"
#include "credentials/store.h"
#include "net/address.h"
#include "net/http_server.h"
#include "net/reactor.h"
#include "net/tailscale.h"
#include "settings/keys.h"
#include "settings/store.h"

namespace fv::agentapi {

namespace {

constexpr log::Logger kLog{"services.agent_mcp.runtime"};
constexpr std::chrono::seconds kGracefulShutdown{3}; // uvicorn's timeout_graceful_shutdown
constexpr std::chrono::seconds kStopWait{7};         // stop(wait=True)'s join timeout

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return text;
}

std::string_view build_platform() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#else
    return "linux";
#endif
}

class ListenFailure : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

ServerStatus make_status(std::string state, std::string message, int port,
                         std::string remote_url = {}) {
    ServerStatus status;
    status.state = std::move(state);
    status.message = std::move(message);
    status.port = port;
    status.remote_url = std::move(remote_url);
    return status;
}

} // namespace

std::string credential_name() {
    return std::string(identity().env_prefix) + "MCP_TOKEN";
}

std::string ServerStatus::url() const {
    return std::format("http://127.0.0.1:{}/mcp", port);
}

TailscaleListen tailscale_address(TailscaleProbe& probe) {
    const net::TailscaleStatus status = probe.status();
    const auto parsed = net::parse_address(status.address);
    const std::string address =
        parsed && parsed->version == 4 ? parsed->to_string() : std::string();
    if (!status.running() || address.empty() || !net::is_tailscale_address(address)) {
        throw TailscaleUnavailable();
    }
    std::string dns_name = lower_ascii(status.dns_name);
    while (dns_name.ends_with('.')) {
        dns_name.pop_back();
    }
    return {address, dns_name};
}

std::string token_store_error(CredentialStore& credentials, std::string_view platform) {
    if (platform.empty()) {
        platform = build_platform();
    }
    const CredentialStatus status = credentials.status();
    const std::string reason = lower_ascii(status.reason);
    if (status.available || reason.find("lock") != std::string::npos) {
        return "Could not save the MCP token. Unlock your system credential store, then retry.";
    }
    if (reason.find("did not respond") != std::string::npos) {
        return "Could not save the MCP token. The system credential store did not respond. "
               "Unlock it if it is waiting, then retry.";
    }
    if (platform != "win32" && platform != "darwin") {
        return "Could not save the MCP token. No system credential store was found. Install or "
               "start GNOME Keyring or KWallet, then retry.";
    }
    return "Could not save the MCP token. " + status.backend_name +
           " is not available. Start it, then retry.";
}

McpRuntime::McpRuntime(McpRuntimeDeps deps) : deps_(std::move(deps)) {}

McpRuntime::~McpRuntime() {
    std::thread thread;
    {
        std::lock_guard lock(mutex_);
        stop_->store(true);
        changed_.notify_all();
        thread = std::move(thread_);
    }
    if (thread.joinable()) {
        thread.join();
    }
}

std::uint64_t McpRuntime::set_status(ServerStatus status) {
    // Called with mutex_ held; the signal posts or runs subscribers outside of it.
    status_ = std::move(status);
    return ++version_;
}

void McpRuntime::announce(std::uint64_t version, ServerStatus status) {
    // Two threads may set statuses and emit them in the opposite order (a listener that fails
    // at once while start() is still announcing "starting"). Whoever finds its status replaced
    // after emitting emits the newest one again, so the last emission is the status that holds.
    while (true) {
        status_changed.emit(status);
        std::lock_guard lock(mutex_);
        if (version_ == version) {
            return;
        }
        version = version_;
        status = status_;
    }
}

ServerStatus McpRuntime::status() {
    ServerStatus out;
    std::uint64_t promoted = 0;
    {
        std::lock_guard lock(mutex_);
        if (status_.state == "starting" && started_) {
            promoted = set_status(make_status("running", "Ready for agent connections.",
                                              status_.port, status_.remote_url));
        }
        out = status_;
    }
    if (promoted != 0) {
        announce(promoted, out);
    }
    return out;
}

std::string McpRuntime::token() {
    const ServerStatus current = status();
    std::lock_guard lock(mutex_);
    return current.state == "running" ? token_ : std::string();
}

void McpRuntime::start(int port, bool tailscale, std::function<bool()> access) {
    if (port < 1 || port > 65535) {
        throw std::invalid_argument("MCP port must be between 1 and 65535.");
    }
    std::thread finished;
    ServerStatus announced;
    std::uint64_t version = 0;
    {
        std::lock_guard lock(mutex_);
        if (thread_alive_) {
            return;
        }
        finished = std::move(thread_);
        stop_ = std::make_shared<std::atomic<bool>>(false);
        token_.clear();
        started_ = false;
        version = set_status(make_status("starting", "Starting MCP…", port));
        announced = status_;
        thread_alive_ = true;
        thread_ = std::thread(&McpRuntime::serve, this, port, tailscale, std::move(access));
    }
    if (finished.joinable()) {
        finished.join();
    }
    announce(version, std::move(announced));
}

void McpRuntime::stop(bool wait) {
    ServerStatus announced;
    std::uint64_t version = 0;
    {
        std::unique_lock lock(mutex_);
        stop_->store(true);
        token_.clear();
        version = set_status(thread_alive_ ? make_status("stopping", "Stopping MCP…", status_.port)
                                           : make_status("stopped", "MCP is off.", status_.port));
        announced = status_;
        changed_.notify_all();
        if (wait) {
            changed_.wait_for(lock, kStopWait, [this] { return !thread_alive_; });
        }
    }
    announce(version, std::move(announced));
}

void McpRuntime::restore() {
    if (deps_.settings == nullptr) {
        return;
    }
    const auto saved = deps_.settings->snapshot();
    if (keys::mcp_enabled.read(*saved)) {
        start(static_cast<int>(keys::mcp_port.read(*saved)),
              keys::mcp_tailscale_enabled.read(*saved));
    }
}

void McpRuntime::configure_controls(std::function<void(std::string, Json)> on_change,
                                    std::function<void(std::string, std::string)> meeting_renamer) {
    std::lock_guard lock(mutex_);
    on_change_ = std::move(on_change);
    meeting_renamer_ = std::move(meeting_renamer);
}

void McpRuntime::add_routes(std::function<void(LiveRoutes&)> installer) {
    std::lock_guard lock(mutex_);
    route_installers_.push_back(std::move(installer));
}

void McpRuntime::add_protocol(std::function<void(McpServer&)> installer) {
    std::lock_guard lock(mutex_);
    protocol_installers_.push_back(std::move(installer));
}

void McpRuntime::on_stopped(std::function<void()> callback) {
    std::lock_guard lock(mutex_);
    stopped_callbacks_.push_back(std::move(callback));
}

std::string McpRuntime::saved_token() {
    if (deps_.ephemeral_token) {
        return token_urlsafe(32);
    }
    CredentialStore& credentials = *deps_.credentials;
    // A failed probe is cached so Settings redraws do not re-prompt. Starting is the user
    // asking to look again.
    if (!credentials.status().available) {
        credentials.refresh();
    }
    // An empty saved token is no token (Python's `if not token`): make one.
    if (auto token = credentials.get(credential_name()); token && !token->empty()) {
        return *token;
    }
    std::string token = token_urlsafe(32);
    credentials.set(credential_name(), token);
    return token;
}

void McpRuntime::serve(int port, bool tailscale, std::function<bool()> access) {
    std::shared_ptr<std::atomic<bool>> stop;
    {
        std::lock_guard lock(mutex_);
        stop = stop_;
    }
    std::string error;
    std::unique_ptr<net::HttpServer> server;
    bool served = false;
    try {
        std::vector<std::string> addresses{"127.0.0.1"};
        std::vector<std::string> allowed_hosts;
        std::string remote_url;
        if (tailscale) {
            if (!deps_.resolve_tailscale && deps_.tailscale == nullptr) {
                throw TailscaleUnavailable(); // no probe: Tailscale cannot be asked
            }
            const TailscaleListen listen = deps_.resolve_tailscale
                                               ? deps_.resolve_tailscale(*deps_.tailscale)
                                               : tailscale_address(*deps_.tailscale);
            addresses.push_back(listen.address);
            allowed_hosts.push_back(listen.address);
            if (!listen.dns_name.empty()) {
                allowed_hosts.push_back(listen.dns_name);
            }
            remote_url = std::format("http://{}:{}/mcp", listen.address, port);
        }
        const std::string token = saved_token();
        if (!stop->load()) {
            server = net::HttpServer::create(*deps_.reactor);
            for (const std::string& address : addresses) {
                // Exclusive on Windows, so a second listener cannot share the port.
                if (!server->listen(
                        {address, static_cast<std::uint16_t>(port), std::nullopt, true})) {
                    throw ListenFailure("listen");
                }
            }
            McpAppOptions options;
            options.database = deps_.database;
            options.token = token;
            options.allowed_hosts = allowed_hosts;
            options.enabled = [stop, access = std::move(access)] {
                return !stop->load() && (!access || access());
            };
            options.settings = deps_.settings;
            options.local_history_only = deps_.ephemeral_token;
            options.allow_live_tools = deps_.allow_live_tools && !deps_.ephemeral_token;
            options.clock = deps_.clock;
            options.pinger = deps_.io;
            std::vector<std::function<void(LiveRoutes&)>> routes;
            std::vector<std::function<void(McpServer&)>> protocol;
            {
                std::lock_guard lock(mutex_);
                options.on_change = on_change_;
                options.meeting_renamer = meeting_renamer_;
                routes = route_installers_;
                protocol = protocol_installers_;
            }
            auto app = std::make_shared<McpApp>(std::move(options));
            for (const auto& install : routes) {
                install(app->live_routes());
            }
            for (const auto& install : protocol) {
                install(app->protocol());
            }
            Executor* io = deps_.io;
            server->set_fallback([app, io](const net::HttpRequest& request, bool,
                                           std::shared_ptr<net::Responder> responder) {
                // The request is valid only while this runs on the reactor; the app blocks.
                auto copy = std::make_shared<net::HttpRequest>(request);
                io->post([app, copy, responder = std::move(responder)]() mutable {
                    app->serve(*copy, std::move(responder));
                });
            });
            ServerStatus announced;
            std::uint64_t version = 0;
            {
                std::unique_lock lock(mutex_);
                if (!stop->load()) {
                    token_ = token;
                    started_ = true;
                    served = true;
                    version =
                        set_status(make_status("starting", "Starting MCP…", port, remote_url));
                    announced = status_;
                }
            }
            if (served) {
                announce(version, std::move(announced));
                std::unique_lock lock(mutex_);
                changed_.wait(lock, [&] { return stop->load(); });
            }
            server->stop(kGracefulShutdown);
            server.reset();
        }
    } catch (const CredentialStoreError&) {
        error = deps_.credentials ? token_store_error(*deps_.credentials, deps_.platform)
                                  : "Could not create a temporary MCP session token.";
    } catch (const TailscaleUnavailable&) {
        error = "Tailscale is not connected. Connect Tailscale, or turn off Tailscale access, "
                "then retry.";
    } catch (const ListenFailure&) {
        error = std::format(
            "Could not listen on port {}. Choose another port or close the app using it.", port);
    } catch (const std::exception& failure) {
        // Never display or log credentials, SQL, transcript data or database paths.
        kLog.warning("MCP could not start: {}", typeid(failure).name());
        error = "Could not start MCP. Verify that " + std::string(identity().product) +
                "'s history database is available, then retry.";
    }
    if (server) {
        server->stop(std::chrono::milliseconds(0));
        server.reset();
    }
    std::vector<std::function<void()>> callbacks;
    {
        std::lock_guard lock(mutex_);
        if (served) {
            callbacks = stopped_callbacks_;
        }
    }
    for (const auto& callback : callbacks) {
        try {
            callback();
        } catch (const std::exception& failure) {
            kLog.warning("An MCP stop callback failed: {}", typeid(failure).name());
        }
    }
    ServerStatus announced;
    std::uint64_t version = 0;
    {
        std::lock_guard lock(mutex_);
        token_.clear();
        started_ = false;
        // The status once the listener is gone; a stop that was asked for is never an error.
        version = set_status(!error.empty() && !stop->load()
                                 ? make_status("error", error, port)
                                 : make_status("stopped", "MCP is off.", port));
        announced = status_;
        thread_alive_ = false;
        changed_.notify_all();
    }
    announce(version, std::move(announced));
}

} // namespace fv::agentapi
