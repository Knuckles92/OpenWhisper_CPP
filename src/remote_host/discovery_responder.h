#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "remote/discovery_transport.h"

// Answers discovery queries while this computer shares its engine
// (services/remote_asr/discovery.py DiscoveryResponder). The answer says who this is (name, TCP
// port, certificate fingerprint, the probe's subset of the engine, approval); only this
// network's addresses get one, and each of them at most MAX_ANSWERS_PER_SENDER times (and all
// of them MAX_ANSWERS times) per RATE_WINDOW_S.
namespace fv::remote_host {

// Affinity: thread-safe. Its socket is read on a thread of its own ("RemoteEngineDiscovery");
// start() and stop() are not called from that thread or from `info`.
class DiscoveryResponder {
public:
    // `info` returns {name, port, fingerprint, engine, approval}; called for every answer, on
    // the responder's thread. Rates are counted on `clock`'s steady time.
    explicit DiscoveryResponder(std::function<Json()> info, Clock& clock = system_clock());
    ~DiscoveryResponder();
    DiscoveryResponder(const DiscoveryResponder&) = delete;
    DiscoveryResponder& operator=(const DiscoveryResponder&) = delete;

    // Listens for queries on `bind`:`port` (DISCOVERY_PORT when nullopt) through a socket made
    // by `transports`. False, with error() "UDP port <port> is in use (<the system's text>)",
    // when the port cannot be bound; true when it already runs.
    bool start(const remote::DiscoveryTransportFactory& transports, std::optional<int> port,
               std::string_view bind);
    void stop();

    bool running() const;
    // The UDP port it answers on while running.
    std::optional<std::uint16_t> port() const;
    // Why it isn't answering, when it couldn't start; "" once it started.
    std::string error() const;

    // The bytes to send back to `sender` for `data`, or nullopt to stay quiet.
    std::optional<std::vector<std::byte>> answer(std::span<const std::byte> data,
                                                 std::string_view sender);

private:
    bool admit(const std::string& sender);
    void serve(std::shared_ptr<remote::DiscoveryTransport> transport);

    std::function<Json()> info_;
    Clock& clock_;

    mutable std::mutex state_mutex_;
    std::shared_ptr<remote::DiscoveryTransport> transport_;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::optional<std::uint16_t> port_;
    std::string error_;

    std::mutex rate_mutex_;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>> per_sender_;
    std::deque<std::chrono::steady_clock::time_point> all_;
};

} // namespace fv::remote_host
