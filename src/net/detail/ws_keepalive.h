#pragma once

#include <chrono>

// The keepalive of every WebSocket the server accepts: uvicorn's ws_ping_interval and
// ws_ping_timeout (20 s each). No Boost here, so that tests can shorten them.
namespace fv::net::detail {

struct WsKeepalive {
    // From the open (and from each answered pong, less its round trip) to the next ping.
    std::chrono::milliseconds interval{20'000};
    // A ping not answered this long after it was sent closes 1011 "keepalive ping timeout".
    std::chrono::milliseconds timeout{20'000};
};

// What connections opened from now on use where their route sets no ping_interval or
// ping_timeout of its own. Process-wide: tests set it and put the default back.
// Affinity: thread-safe.
WsKeepalive ws_keepalive();
void set_ws_keepalive(WsKeepalive keepalive);

} // namespace fv::net::detail
