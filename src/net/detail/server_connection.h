#pragma once

// No Boost here, so that tests can reach a connection as stop() does.
namespace fv::net::detail {

// A connection as the server holds it for stop(): an HTTP session or an upgraded WebSocket.
// Affinity: thread-safe.
class ServerConnection {
public:
    virtual ~ServerConnection() = default;
    // HttpServer::stop: begin closing, politely.
    virtual void server_stopping() = 0;
    // After the grace: close the socket now.
    virtual void force_close() = 0;
};

} // namespace fv::net::detail
