#pragma once

#include <chrono>
#include <condition_variable>
#include <map>
#include <mutex>

#include "remote/client_history_source.h"
#include "remote_host/history_channel.h"

namespace fv::remote_host {

class DeviceRegistry;

// Direct history from paired clients that explicitly share it. One channel per device and at
// most eight concurrent queries per channel. A replacement or revocation wakes every waiter.
class HistoryBroker final : public HistoryChannel, public remote::ClientHistorySource {
public:
    explicit HistoryBroker(DeviceRegistry& registry,
                           std::chrono::milliseconds timeout = std::chrono::seconds(5));

    void start() override;
    void serve(std::shared_ptr<net::WsConnection> connection, const remote::PairedDevice& device,
               std::string token, bool enabled) override;
    void on_text(const std::shared_ptr<net::WsConnection>& connection,
                 std::string_view text) override;
    void on_close(const std::shared_ptr<net::WsConnection>& connection) override;
    void remove(std::optional<std::string> device_id) override;

    std::vector<remote::ClientHistoryStatus> clients() override;
    Json query(std::string_view device_id, std::string_view operation, const Json& params) override;

private:
    struct Peer {
        std::shared_ptr<net::WsConnection> connection;
        std::string token;
        bool enabled;
        bool closed = false;
        std::map<std::string, std::optional<Json>, std::less<>> pending;
    };

    std::shared_ptr<Peer> find_peer(const std::shared_ptr<net::WsConnection>& connection) const;

    DeviceRegistry& registry_;
    std::chrono::milliseconds timeout_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool accepting_ = false;
    std::map<std::string, std::shared_ptr<Peer>, std::less<>> peers_;
};

} // namespace fv::remote_host
