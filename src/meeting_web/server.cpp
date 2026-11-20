#include "meeting_web/server.h"

#include <format>
#include <string>
#include <utility>

#include "base/log.h"
#include "meeting_web/detail/py_value.h"
#include "meeting_web/voice_help.h"
#include "net/lan_address.h"

namespace fv::meeting_web {

namespace {

constexpr log::Logger logger("meeting.web.server");

std::string normalized_bind(std::string bind) {
    return bind == "lan" ? std::move(bind) : std::string("localhost");
}

} // namespace

std::string display_host(std::string_view bind,
                         const std::function<std::optional<std::string>()>& discover) {
    if (bind != "lan") {
        return "127.0.0.1";
    }
    if (discover) {
        if (auto host = discover(); host && !host->empty()) {
            return std::move(*host);
        }
    }
    logger.warning("Could not resolve a usable LAN address; using 127.0.0.1");
    return "127.0.0.1";
}

MeetingWebServer::MeetingWebServer(net::Reactor& reactor, DashboardEngine& engine,
                                   std::shared_ptr<meeting_state::MeetingRepository> repository,
                                   std::string bind, std::uint16_t port,
                                   RouteInstaller install_routes)
    : reactor_(reactor), engine_(engine), repository_(std::move(repository)),
      bind_(normalized_bind(std::move(bind))), port_(port),
      install_routes_(std::move(install_routes)),
      discover_([] { return net::discover_lan_ipv4(); }), hub_(reactor, engine, repository_) {
    hub_.set_guest_url_provider([this] { return guest_url(); });
}

MeetingWebServer::~MeetingWebServer() {
    stop();
}

std::expected<std::string, std::string> MeetingWebServer::start() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (http_) {
        return base_url();
    }
    auto server = net::HttpServer::create(reactor_);
    server->websocket("/ws", hub_.handlers());
    if (install_routes_) {
        install_routes_(*server, *this);
    }
    hub_.on_startup();
    const std::string listen_host = bind_ == "localhost" ? "127.0.0.1" : "0.0.0.0";
    const auto endpoint = server->listen(net::Listen{.address = listen_host, .port = port_});
    if (!endpoint) {
        logger.error("Meeting web server could not listen on {}:{}: {}", listen_host, port_,
                     endpoint.error());
        server->stop(std::chrono::milliseconds(0));
        hub_.on_shutdown();
        return std::unexpected(std::string(kStartFailed));
    }
    std::function<std::optional<std::string>()> discover;
    {
        std::lock_guard lock(state_mutex_);
        discover = discover_;
    }
    std::string url = std::format("http://{}:{}", display_host(bind_, discover), endpoint->port);
    {
        std::lock_guard lock(state_mutex_);
        base_url_ = url;
    }
    http_ = std::move(server);
    logger.info("Meeting web server listening at {} (bind={})", url, bind_);
    return url;
}

void MeetingWebServer::stop() {
    std::lock_guard lifecycle(lifecycle_mutex_);
    if (http_) {
        http_->stop(kGracefulShutdown);
        hub_.on_shutdown();
        http_.reset();
    }
    std::lock_guard lock(state_mutex_);
    base_url_.clear();
}

bool MeetingWebServer::is_running() const {
    std::lock_guard lock(state_mutex_);
    return !base_url_.empty();
}

std::string MeetingWebServer::base_url() const {
    std::lock_guard lock(state_mutex_);
    return base_url_;
}

std::string MeetingWebServer::host_url() const {
    return tokenized_url("host_token");
}

std::string MeetingWebServer::guest_url() const {
    return tokenized_url("guest_token");
}

std::string MeetingWebServer::tokenized_url(std::string_view key) const {
    const std::string base = base_url();
    if (base.empty()) {
        return {};
    }
    const std::string meeting_id = engine_.meeting_id();
    if (meeting_id.empty()) {
        return {};
    }
    std::optional<Json> meeting;
    try {
        meeting = repository_->get_meeting(meeting_id);
    } catch (const std::exception& error) {
        logger.error_with(error, std::format("Failed to read meeting {} for {}", meeting_id, key));
        return {};
    }
    if (!meeting) {
        return {};
    }
    const Json& token = detail::get(*meeting, key);
    if (!detail::truthy(token)) {
        return {};
    }
    return base + "/m/" + detail::py_str(token);
}

void MeetingWebServer::broadcast(const Json& message, bool host_only) {
    hub_.schedule_broadcast(message, host_only);
}

void MeetingWebServer::invalidate_connections(bool resync) {
    hub_.schedule_invalidate_connections(resync);
}

WsHub& MeetingWebServer::hub() {
    return hub_;
}

DashboardEngine& MeetingWebServer::engine() {
    return engine_;
}

const std::shared_ptr<meeting_state::MeetingRepository>& MeetingWebServer::repository() const {
    return repository_;
}

const std::string& MeetingWebServer::bind() const {
    return bind_;
}

void MeetingWebServer::set_settings_source(std::function<Json()> settings) {
    hub_.set_voice_commands([settings = std::move(settings)] {
        return voice_command_guide(settings ? settings() : Json::object());
    });
}

Json MeetingWebServer::voice_commands() const {
    return hub_.voice_commands();
}

void MeetingWebServer::set_lan_discovery(std::function<std::optional<std::string>()> discover) {
    std::lock_guard lock(state_mutex_);
    discover_ = std::move(discover);
}

} // namespace fv::meeting_web
