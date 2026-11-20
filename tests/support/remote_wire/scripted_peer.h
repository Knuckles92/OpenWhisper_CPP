#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "net/reactor.h"
#include "net/types.h"
#include "remote_wire/transcript.h"

namespace fv::testing::remote_wire {

// The remote engine's WebSocket path (protocol.py PATH), spelled here so the testkit needs no
// remote/ header.
inline constexpr std::string_view kEnginePath = "/openwhisper/v1/engine";

// Plays the host half of a transcript to a client under test: a TLS WebSocket server on
// 127.0.0.1 (port 0) over M9.1's HttpServer, configured as OpenWhisper's host serves (no
// permessage-deflate, 8 MiB messages, 10 s opening handshake, a ping every 20 s answered within
// 20 s, reading paused once more than 2 messages are unreleased, as websockets' max_queue=2
// pauses). The k-th connection it accepts plays
// recorded connection k: host frames go out as soon as every client frame recorded before them
// arrived, each client frame is compared with the record (compare, bindings shared across
// connections, product names as recorded), and a refused upgrade is answered with the recorded
// status and body. Placeholders in host frames are filled with new values (fingerprints with
// this peer's own), so a token it issues must come back in the next hello.
// Affinity: thread-safe; the destructor and stop() wait, so not from the reactor's threads.
class ScriptedPeer {
public:
    struct Options {
        // Upgrades to this path play WebSocket connections; any other path plays a refused one.
        std::string path = std::string(kEnginePath);
        // Release only a connection's first this many client frames, as a host that stopped
        // reading: the frames after them stay unreleased, so the socket is no longer read once
        // three of them have arrived. 0 releases every frame.
        std::size_t stop_reading_after = 0;
        std::chrono::milliseconds ping_interval{20'000};
        std::chrono::milliseconds ping_timeout{20'000};
    };

    ScriptedPeer(net::Reactor&, net::TlsIdentity, const Transcript&);
    ScriptedPeer(net::Reactor&, net::TlsIdentity, const Transcript&, Options);
    ~ScriptedPeer();
    ScriptedPeer(const ScriptedPeer&) = delete;
    ScriptedPeer& operator=(const ScriptedPeer&) = delete;

    std::uint16_t port() const;
    // The uppercase hex SHA-256 of this peer's certificate, which its frames report.
    std::string fingerprint() const;
    // Client frames that differ from the record, connections that ended early or were not
    // recorded, frames past the end: each a readable sentence. Empty when the client matched.
    std::vector<std::string> mismatches() const;
    // Every frame and close the clients sent, in arrival order (dir "c2s", conn the recorded
    // connection it played).
    std::vector<WireFrame> received() const;
    // Connections accepted so far, refused upgrades included.
    std::size_t connections() const;
    // Until every recorded connection has been played to its end.
    bool wait_finished(std::chrono::milliseconds timeout) const;
    Json bindings() const;
    // Closes the listener and every connection at once.
    void stop();

private:
    struct State;
    std::shared_ptr<State> state_;
};

// The other half: plays the client frames of recorded connection `conn` to a server on
// 127.0.0.1:`port` with M9.1's ws_connect and compares what the server sends (compare, with
// `bindings` carried from connection to connection and placeholders in client frames filled
// from them). A refused upgrade is compared by status only (ws_connect reports no body).
// Returns the differences, empty when the server matched. Blocks; not on a reactor thread.
struct ReplayOptions {
    std::chrono::milliseconds timeout{10'000}; // for each frame the server should send
    // What the server writes where OpenWhisper wrote its own name; empty: identity().product,
    // as a FioriVoce host writes.
    std::string product;
};
std::vector<std::string> play_client(net::Reactor&, std::uint16_t port, const Transcript&, int conn,
                                     Json& bindings, const ReplayOptions& options = {});

} // namespace fv::testing::remote_wire
