#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/json.h"
#include "net/reactor.h"
#include "remote/client.h"
#include "remote/discovery_codec.h"
#include "remote/discovery_transport.h"
#include "remote/testing/fake_discovery_bus.h"

// What the client tests run against: a small TLS remote-engine host on 127.0.0.1 (no C++ host
// exists in this wave; tests/fakes/remote.py's SpeechHost with FakeEngine is what the Python
// tests ran), a discovery responder on the in-memory bus, and a client environment over both.
namespace fv::remote::test {

// Polls `done` every 10 ms for at most `timeout` (tests/fakes/remote.py _wait_for).
bool wait_for(const std::function<bool()>& done,
              std::chrono::milliseconds timeout = std::chrono::milliseconds(5'000));

// tests/fakes/remote.py _tone: `samples` of 0.1 * sin(n / 8), loud enough for the silence gate.
std::vector<float> tone(std::size_t samples);

// A host sharing on 127.0.0.1 (or `bind`) with a self-signed identity of its own. It answers
// probe, code pairing (one code, reusable), a pairing request whose reveal breaks its commitment
// (`broken_reveal`), hello with a token it issued, and transcribe ("heard <n>"), describe and
// recording_begin/recording_end requests; any other op is "Unknown operation: '<op>'". It
// records every hello and request header it read.
// Affinity: thread-safe; stop() and the destructor wait, so not from the reactor.
class MiniHost {
public:
    struct Options {
        std::string name = "devbox";
        std::string code = "246810";
        std::vector<std::string> addresses = {"192.0.2.10"};
        // What `paired` and `ready` say the certificate is; empty: its own.
        std::string advertised_fingerprint;
        // Report its own certificate in lower case.
        bool lowercase_fingerprint = false;
        bool approval = true;
        // What its probe says about pairing over Tailscale (a trusted Tailscale owner).
        bool tailscale_pairing = false;
        bool broken_reveal = false;
        // How long a transcribe takes before its reply.
        std::chrono::milliseconds transcribe_delay{0};
        // Messages of a connection released after this many; 0: every one. Past it the host
        // neither answers nor reads (max_queued_messages 1), so pings go unanswered.
        std::size_t stop_reading_after = 0;
        std::size_t transcribe_busy_count = 0;
        // False: a host too old to say how long a request took (no host_ms in its replies).
        bool timed = true;
        // False: a host from before model switching, whose `ready` lists no models.
        bool list_models = true;
        std::string bind = "127.0.0.1";
        std::uint16_t port = 0;
    };

    MiniHost(net::Reactor& reactor, Options options);
    explicit MiniHost(net::Reactor& reactor) : MiniHost(reactor, Options{}) {}
    ~MiniHost();
    MiniHost(const MiniHost&) = delete;
    MiniHost& operator=(const MiniHost&) = delete;

    // False when the address could not be bound (the reason in error()).
    bool listening() const;
    std::string error() const;
    std::uint16_t port() const;
    std::string fingerprint() const;
    // Upgrades accepted so far.
    std::size_t connections() const;
    std::vector<Json> hellos() const;
    std::vector<Json> requests() const;
    std::size_t busy_replies() const;
    // Every token it issued.
    std::vector<std::string> tokens() const;
    // Closes every connection still open with `code`, as a host that switched engines does.
    void close_all(std::uint16_t code, std::string reason);
    // Closes the listener and every connection at once.
    void stop();
    void restart();

    struct State;

private:
    std::shared_ptr<State> state_;
};

// Answers discovery queries on the bus as a sharing host at `address` does: bound to
// DISCOVERY_PORT, each query from anyone answered with `info` ({name, port, fingerprint,
// approval, engine}), until destroyed.
class BusResponder {
public:
    BusResponder(testing::FakeDiscoveryBus& bus, std::string address, Json info);
    ~BusResponder();
    BusResponder(const BusResponder&) = delete;
    BusResponder& operator=(const BusResponder&) = delete;

    int answered() const { return answered_.load(); }

private:
    std::unique_ptr<DiscoveryTransport> socket_;
    Json info_;
    std::atomic<bool> stop_{false};
    std::atomic<int> answered_{0};
    std::thread thread_;
};

// A reactor, a discovery bus where this computer is 10.0.0.2 with no network card (a search
// reaches 255.255.255.255, that is every computer on the bus), and the client environment over
// them. Its keepalive is the real one unless a test shortens it.
struct ClientRig {
    ClientRig();
    ~ClientRig();

    net::Reactor reactor{2};
    testing::FakeDiscoveryBus bus;
    ClientEnv env;
};

} // namespace fv::remote::test
