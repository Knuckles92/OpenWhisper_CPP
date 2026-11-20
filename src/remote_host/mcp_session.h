#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include "base/executor.h"
#include "base/json.h"

namespace fv::remote_host {

// One host-operator authorization, used by one authenticated connection. The backend is an
// independently owned fixed loopback MCP listener with read-only access and a session token.
class PeerMcpSession : public std::enable_shared_from_this<PeerMcpSession> {
public:
    static constexpr int kPort = 8768;

    struct Listener {
        std::function<void()> start;
        std::function<void()> stop;
        std::function<Json()> status;
        std::function<std::string()> local_token;
    };

    PeerMcpSession(Executor& timers, Listener listener);
    bool grant(std::string device);
    bool allowed(std::string_view device) const;
    void revoke();
    void disconnect(std::string_view connection);
    Json request(std::string_view operation, const Json& header, std::string_view device,
                 std::string_view connection);
    Json local_status() const;
    std::string local_token() const;

private:
    void revoke_locked();
    void expire(std::uint64_t generation);

    Executor& timers_;
    Listener listener_;
    mutable std::mutex mutex_;
    std::string peer_;
    std::string connection_;
    std::chrono::steady_clock::time_point expires_;
    std::uint64_t generation_ = 0;
    TimerHandle timeout_;
};

} // namespace fv::remote_host
