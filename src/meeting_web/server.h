#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "base/json.h"
#include "meeting_state/repository.h"
#include "meeting_web/dashboard_engine.h"
#include "meeting_web/ws_hub.h"
#include "net/http_server.h"
#include "net/reactor.h"

namespace fv::meeting_web {

// What start() reports when the listener cannot be opened.
inline constexpr std::string_view kStartFailed = "Meeting web server failed to start";
// How long stop() lets open responses finish (the recording player streams audio) before they
// are cut.
inline constexpr std::chrono::milliseconds kGracefulShutdown{1000};

// The host part of the dashboard's links: 127.0.0.1 unless bound to the LAN, where it is the
// address `discover` finds (a LAN address before a VPN's, never a bridge), else 127.0.0.1 with
// the warning "Could not resolve a usable LAN address; using 127.0.0.1".
std::string display_host(std::string_view bind,
                         const std::function<std::optional<std::string>()>& discover);

// meeting/web/server.py MeetingWebServer: the dashboard's HTTP and WebSocket server, plain HTTP
// on 127.0.0.1 by default or on every interface when the user shares the meeting on the local
// network, with capability links http://<host>:<port>/m/<token>. It is one net::HttpServer on
// the shared reactor, opened synchronously by start(); the WebSocket hub serves /ws and the
// routes come from the installer given to the constructor (the dashboard API).
//
// Links are read from the meeting row on every call, never cached, and never logged.
// Affinity: thread-safe. start(), host_url() and guest_url() block (the LAN probe, the
// repository); stop() and the destructor wait, so none of them runs on the reactor.
class MeetingWebServer {
public:
    using RouteInstaller = std::function<void(net::HttpServer&, MeetingWebServer&)>;

    // `bind` is "localhost" or "lan"; anything else is "localhost". Port 0 lets the system
    // choose. The engine and the reactor must outlive the server.
    MeetingWebServer(net::Reactor& reactor, DashboardEngine& engine,
                     std::shared_ptr<meeting_state::MeetingRepository> repository,
                     std::string bind = "localhost", std::uint16_t port = 0,
                     RouteInstaller install_routes = {});
    ~MeetingWebServer(); // stop()
    MeetingWebServer(const MeetingWebServer&) = delete;
    MeetingWebServer& operator=(const MeetingWebServer&) = delete;

    // Listens and returns the base URL (http://host:port); a second call while running returns
    // the same URL. kStartFailed when the address cannot be bound.
    std::expected<std::string, std::string> start();
    // Closes the listener, gives open responses kGracefulShutdown, then cuts them; WebSockets
    // close 1012. Idempotent; afterwards base_url(), host_url() and guest_url() are "".
    void stop();
    bool is_running() const;

    // "" before start and after stop.
    std::string base_url() const;
    // <base>/m/<token> from the current meeting row; "" without a base URL, without a meeting,
    // without that token, or when the repository fails (logged).
    std::string host_url() const;
    std::string guest_url() const;

    // Pushes a message to the dashboard sockets (host sockets only with host_only). Any thread.
    void broadcast(const Json& message, bool host_only = false);
    // Closes every socket after link rotation (4401, after 0.25 s), or at once with 4409 so
    // clients reconnect for fresh state (resync).
    void invalidate_connections(bool resync = false);

    WsHub& hub();
    DashboardEngine& engine();
    const std::shared_ptr<meeting_state::MeetingRepository>& repository() const;
    // "localhost" or "lan".
    const std::string& bind() const;

    // The settings mapping the voice command guide reads (typesafe_voice_command_names); empty
    // settings until set.
    void set_settings_source(std::function<Json()> settings);
    // voice_command_guide over the current settings, as /api/session and hello carry it.
    Json voice_commands() const;
    // Where a LAN-bound server finds its advertised address; net::discover_lan_ipv4 by default.
    void set_lan_discovery(std::function<std::optional<std::string>()> discover);

private:
    std::string tokenized_url(std::string_view key) const;

    net::Reactor& reactor_;
    DashboardEngine& engine_;
    std::shared_ptr<meeting_state::MeetingRepository> repository_;
    const std::string bind_;
    const std::uint16_t port_;
    RouteInstaller install_routes_;

    std::mutex lifecycle_mutex_; // start and stop
    std::unique_ptr<net::HttpServer> http_;

    mutable std::mutex state_mutex_;
    std::string base_url_;
    std::function<std::optional<std::string>()> discover_;

    // Last, so it is destroyed first: its worker threads call back into the members above (the
    // guest link) until its destructor has joined them.
    WsHub hub_;
};

} // namespace fv::meeting_web
