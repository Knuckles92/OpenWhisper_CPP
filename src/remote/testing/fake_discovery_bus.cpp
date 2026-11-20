#include "remote/testing/fake_discovery_bus.h"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <set>
#include <utility>

namespace fv::remote::testing {

namespace {

using Clock = std::chrono::steady_clock;

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
    bool bound = false;
    bool closed = false;
    std::deque<std::pair<Clock::time_point, Datagram>> inbox;
};

} // namespace

struct FakeDiscoveryBus::State {
    std::mutex mutex;
    std::condition_variable arrived;
    std::set<std::string> hosts;
    std::map<std::string, std::vector<std::string>> broadcasts;
    std::map<std::pair<std::string, std::uint16_t>, std::shared_ptr<Endpoint>> endpoints;
    DropFilter drop;
    std::chrono::milliseconds delay{0};
    std::map<std::pair<std::string, std::uint16_t>, std::string> bind_failures;
    std::vector<Sent> sent;
    std::uint16_t next_port = 49152;

    std::uint16_t free_port(const std::string& host) {
        while (endpoints.contains({host, next_port})) {
            ++next_port;
        }
        return next_port++;
    }
};

namespace {

class BusTransport final : public DiscoveryTransport {
public:
    BusTransport(std::shared_ptr<FakeDiscoveryBus::State> state, std::string host, bool broadcast)
        : state_(std::move(state)), me_(std::make_shared<Endpoint>()), broadcast_(broadcast) {
        me_->host = std::move(host);
    }
    ~BusTransport() override { close(); }

    std::expected<std::uint16_t, std::string> bind(std::string_view address,
                                                   std::uint16_t port) override {
        std::lock_guard lock(state_->mutex);
        if (me_->closed) {
            return std::unexpected("The socket is closed");
        }
        if (me_->bound) {
            return std::unexpected("The socket is already bound");
        }
        if (!address.empty() && address != "0.0.0.0" && address != me_->host) {
            return std::unexpected("The requested address is not valid in its context");
        }
        const auto failure = state_->bind_failures.find({me_->host, port});
        if (failure != state_->bind_failures.end()) {
            std::string error = std::move(failure->second);
            state_->bind_failures.erase(failure);
            return std::unexpected(std::move(error));
        }
        if (port == 0) {
            port = state_->free_port(me_->host);
        } else if (state_->endpoints.contains({me_->host, port})) {
            return std::unexpected("Only one usage of each socket address "
                                   "(protocol/network address/port) is normally permitted");
        }
        register_locked(port);
        return port;
    }

    bool send_to(std::span<const std::byte> bytes, std::string_view address,
                 std::uint16_t port) override {
        {
            std::lock_guard lock(state_->mutex);
            if (me_->closed) {
                return false;
            }
            if (!me_->bound) {
                register_locked(state_->free_port(me_->host));
            }
            const std::string to(address);
            std::vector<std::string> targets;
            bool broadcast_address = false;
            if (to == FakeDiscoveryBus::ALL_HOSTS) {
                broadcast_address = true;
                targets.assign(state_->hosts.begin(), state_->hosts.end());
            } else if (const auto found = state_->broadcasts.find(to);
                       found != state_->broadcasts.end()) {
                broadcast_address = true;
                targets = found->second;
            } else {
                targets.push_back(to);
            }
            if (broadcast_address && !broadcast_) {
                return false; // a broadcast needs SO_BROADCAST
            }
            FakeDiscoveryBus::Sent sent{
                me_->host, me_->port, to, port, std::vector<std::byte>(bytes.begin(), bytes.end()),
                false};
            const auto due = Clock::now() + state_->delay;
            for (const auto& target : targets) {
                const auto found = state_->endpoints.find({target, port});
                if (found == state_->endpoints.end() || found->second->closed) {
                    continue;
                }
                if (state_->drop && state_->drop(sent, target)) {
                    continue;
                }
                found->second->inbox.emplace_back(due, Datagram{sent.bytes, me_->host, me_->port});
                sent.delivered = true;
            }
            state_->sent.push_back(std::move(sent));
        }
        state_->arrived.notify_all();
        return true;
    }

    std::optional<Datagram> receive(std::chrono::milliseconds timeout) override {
        std::unique_lock lock(state_->mutex);
        const auto deadline = Clock::now() + timeout;
        while (true) {
            if (me_->closed) {
                return std::nullopt;
            }
            const auto now = Clock::now();
            auto wake = deadline;
            for (auto it = me_->inbox.begin(); it != me_->inbox.end(); ++it) {
                if (it->first <= now) {
                    Datagram datagram = std::move(it->second);
                    me_->inbox.erase(it);
                    return datagram;
                }
                wake = std::min(wake, it->first);
            }
            if (now >= deadline) {
                return std::nullopt;
            }
            state_->arrived.wait_until(lock, wake);
        }
    }

    void close() override {
        {
            std::lock_guard lock(state_->mutex);
            if (me_->closed) {
                return;
            }
            me_->closed = true;
            if (me_->bound) {
                const auto found = state_->endpoints.find({me_->host, me_->port});
                if (found != state_->endpoints.end() && found->second == me_) {
                    state_->endpoints.erase(found);
                }
            }
        }
        state_->arrived.notify_all();
    }

private:
    void register_locked(std::uint16_t port) {
        me_->port = port;
        me_->bound = true;
        state_->endpoints[{me_->host, port}] = me_;
    }

    std::shared_ptr<FakeDiscoveryBus::State> state_;
    std::shared_ptr<Endpoint> me_;
    bool broadcast_;
};

} // namespace

FakeDiscoveryBus::FakeDiscoveryBus() : state_(std::make_shared<State>()) {}

FakeDiscoveryBus::~FakeDiscoveryBus() = default;

std::unique_ptr<DiscoveryTransport> FakeDiscoveryBus::transport(std::string host, bool broadcast) {
    {
        std::lock_guard lock(state_->mutex);
        state_->hosts.insert(host);
    }
    return std::make_unique<BusTransport>(state_, std::move(host), broadcast);
}

DiscoveryTransportFactory FakeDiscoveryBus::factory(std::string host) {
    return [this, host = std::move(host)](bool broadcast) {
        return transport(host, broadcast);
    };
}

void FakeDiscoveryBus::add_broadcast(std::string address, std::vector<std::string> members) {
    std::lock_guard lock(state_->mutex);
    for (const auto& member : members) {
        state_->hosts.insert(member);
    }
    state_->broadcasts[std::move(address)] = std::move(members);
}

void FakeDiscoveryBus::set_drop(DropFilter filter) {
    std::lock_guard lock(state_->mutex);
    state_->drop = std::move(filter);
}

void FakeDiscoveryBus::set_delay(std::chrono::milliseconds delay) {
    std::lock_guard lock(state_->mutex);
    state_->delay = delay;
}

void FakeDiscoveryBus::fail_bind(std::string host, std::uint16_t port, std::string error) {
    std::lock_guard lock(state_->mutex);
    state_->bind_failures[{std::move(host), port}] = std::move(error);
}

std::vector<FakeDiscoveryBus::Sent> FakeDiscoveryBus::sent() const {
    std::lock_guard lock(state_->mutex);
    return state_->sent;
}

std::vector<std::string> FakeDiscoveryBus::bound() const {
    std::lock_guard lock(state_->mutex);
    std::vector<std::string> out;
    for (const auto& [key, endpoint] : state_->endpoints) {
        out.push_back(key.first + ":" + std::to_string(key.second));
    }
    return out;
}

} // namespace fv::remote::testing
