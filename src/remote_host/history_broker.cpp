#include "remote_host/history_broker.h"

#include <algorithm>
#include <utility>

#include "base/executor.h"
#include "base/ids.h"
#include "pyport/json.h"
#include "remote/protocol.h"
#include "remote/wire.h"
#include "remote_host/registry.h"

namespace fv::remote_host {
namespace {

constexpr std::size_t kMaxQueries = 8;

void send(const std::shared_ptr<net::WsConnection>& connection, const Json& message) {
    connection->send_text(std::make_shared<const std::string>(remote::dumps_default(message)));
}

} // namespace

HistoryBroker::HistoryBroker(DeviceRegistry& registry, std::chrono::milliseconds timeout)
    : registry_(registry), timeout_(timeout) {}

void HistoryBroker::start() {
    std::lock_guard lock(mutex_);
    accepting_ = true;
}

void HistoryBroker::serve(std::shared_ptr<net::WsConnection> connection,
                          const remote::PairedDevice& device, std::string token, bool enabled) {
    const auto authenticated = registry_.authenticate(token);
    if (!authenticated || authenticated->id != device.id) {
        connection->close(remote::CLOSE_UNAUTHORIZED, "device removed");
        return;
    }
    std::shared_ptr<Peer> previous;
    {
        std::lock_guard lock(mutex_);
        if (!accepting_) {
            connection->close(1001, "host stopped sharing");
            return;
        }
        auto& current = peers_[device.id];
        previous = std::exchange(current, std::make_shared<Peer>(Peer{
                                              connection, std::move(token), enabled, false, {}}));
        if (previous) {
            previous->closed = true;
        }
        // A query may follow ready immediately, so publish both under the same lock.
        send(connection, Json{{"type", "ready"}, {"capabilities", {{"client_history", true}}}});
    }
    if (previous) {
        changed_.notify_all();
        previous->connection->close(1001, "history connection replaced");
    }
}

std::shared_ptr<HistoryBroker::Peer>
HistoryBroker::find_peer(const std::shared_ptr<net::WsConnection>& connection) const {
    const auto found = std::ranges::find_if(
        peers_, [&](const auto& entry) { return entry.second->connection == connection; });
    return found != peers_.end() ? found->second : nullptr;
}

void HistoryBroker::on_text(const std::shared_ptr<net::WsConnection>& connection,
                            std::string_view text) {
    std::shared_ptr<Peer> peer;
    {
        std::lock_guard lock(mutex_);
        peer = find_peer(connection);
    }
    if (!peer) {
        return;
    }
    Json reply;
    try {
        if (text.size() <= remote::MAX_REPLY_BYTES && registry_.authenticate(peer->token)) {
            reply = py::loads(text);
        }
    } catch (const std::exception&) {
        // Neither the failed client's reply nor its credential belongs in the host log.
    }
    if (!reply.is_object() || !reply.contains("type") || reply["type"] != "history_result" ||
        !reply.contains("id") || !reply["id"].is_string()) {
        on_close(connection);
        connection->close(1001, "history connection closed");
        return;
    }
    {
        std::lock_guard lock(mutex_);
        if (peer->closed) {
            return;
        }
        const auto request = peer->pending.find(reply["id"].get_ref<const std::string&>());
        if (request != peer->pending.end() && !request->second) {
            request->second = std::move(reply);
        }
    }
    changed_.notify_all();
}

void HistoryBroker::on_close(const std::shared_ptr<net::WsConnection>& connection) {
    {
        std::lock_guard lock(mutex_);
        const auto found = std::ranges::find_if(
            peers_, [&](const auto& entry) { return entry.second->connection == connection; });
        if (found == peers_.end()) {
            return;
        }
        found->second->closed = true;
        peers_.erase(found);
    }
    changed_.notify_all();
}

void HistoryBroker::remove(std::optional<std::string> device_id) {
    std::vector<std::shared_ptr<Peer>> removed;
    {
        std::lock_guard lock(mutex_);
        if (!device_id) {
            accepting_ = false;
        }
        for (auto at = peers_.begin(); at != peers_.end();) {
            if (!device_id || at->first == *device_id) {
                at->second->closed = true;
                removed.push_back(std::move(at->second));
                at = peers_.erase(at);
            } else {
                ++at;
            }
        }
    }
    changed_.notify_all();
    for (const auto& peer : removed) {
        peer->connection->close(1001, "history connection closed");
    }
}

std::vector<remote::ClientHistoryStatus> HistoryBroker::clients() {
    assert_not_gui_thread();
    const auto devices = registry_.list();
    std::lock_guard lock(mutex_);
    std::vector<remote::ClientHistoryStatus> out;
    for (const auto& device : devices) {
        const auto found = peers_.find(device.id);
        const auto status = found == peers_.end()    ? "unavailable"
                            : found->second->enabled ? "online"
                                                     : "sharing_disabled";
        out.push_back({device.id, device.name, status});
    }
    return out;
}

Json HistoryBroker::query(std::string_view device_id, std::string_view operation,
                          const Json& params) {
    assert_not_gui_thread();
    const auto devices = registry_.list();
    if (std::ranges::find(devices, device_id, &remote::PairedDevice::id) == devices.end()) {
        throw remote::ClientUnavailable();
    }
    const auto id = uuid4_hex();
    std::unique_lock lock(mutex_);
    const auto found = peers_.find(device_id);
    if (found == peers_.end() || found->second->pending.size() >= kMaxQueries) {
        throw remote::ClientUnavailable();
    }
    const auto peer = found->second;
    if (!peer->enabled) {
        throw remote::ClientUnavailable("sharing_disabled");
    }
    const auto request = peer->pending.emplace(id, std::nullopt).first;
    try {
        send(peer->connection, Json{{"type", "history_query"},
                                    {"id", id},
                                    {"operation", operation},
                                    {"params", params}});
    } catch (const std::exception&) {
        peer->pending.erase(request);
        throw remote::ClientUnavailable();
    }
    changed_.wait_for(lock, timeout_, [&] { return peer->closed || request->second.has_value(); });
    auto reply = std::move(request->second);
    peer->pending.erase(request);
    if (peer->closed || !reply) {
        throw remote::ClientUnavailable();
    }
    if (reply->contains("error")) {
        throw remote::ClientUnavailable((*reply)["error"]);
    }
    if (!reply->contains("result")) {
        throw remote::ClientUnavailable();
    }
    return std::move((*reply)["result"]);
}

} // namespace fv::remote_host
