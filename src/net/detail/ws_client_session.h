#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "net/ws_client.h"

// ws_connect's machinery. No Boost here.
namespace fv::net::detail {

class ReactorImpl;

// The parts of a ws:// or wss:// URL the client uses.
struct WsUrl {
    bool secure = false;
    std::string host; // without brackets
    std::uint16_t port = 0;
    std::string target; // path and query, "/" at least
    bool host_is_ip = false;
    // websockets' build_host: the Host header, IPv6 in brackets, the port only when it is not
    // the scheme's default.
    std::string host_header() const;
};

// nullopt for anything but ws:// or wss:// with a host and a port in 1..65535 (or none).
std::optional<WsUrl> parse_ws_url(std::string_view url);

std::unique_ptr<WsClientConnection> start_ws_client(const std::shared_ptr<ReactorImpl>& reactor,
                                                    WsClientOptions options);

} // namespace fv::net::detail
