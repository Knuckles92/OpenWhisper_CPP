#pragma once

#include <mutex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "agentapi/mcp_runtime.h"

namespace fv::agentapi {

// An McpServerControl that answers from fields and records every start and stop, for the API &
// MCP page's tests. start() moves the status to `running` on `port` at once (or to
// `start_error` when it is set); stop() to "stopped".
// Affinity: thread-safe.
class FakeMcpControl final : public McpServerControl {
public:
    struct Call {
        std::string method; // "start" or "stop"
        int port = 0;
        bool flag = false; // tailscale for start, wait for stop

        friend bool operator==(const Call&, const Call&) = default;
    };

    ServerStatus status() override {
        std::lock_guard lock(mutex_);
        return current;
    }
    std::string token() override {
        std::lock_guard lock(mutex_);
        return current.state == "running" ? running_token : std::string();
    }
    void start(int port, bool tailscale = false, std::function<bool()> access = {}) override {
        if (port < 1 || port > 65535) {
            throw std::invalid_argument("MCP port must be between 1 and 65535.");
        }
        std::lock_guard lock(mutex_);
        session_access = std::move(access);
        calls_.push_back({"start", port, tailscale});
        current.port = port;
        if (!start_error.empty()) {
            current.state = "error";
            current.message = start_error;
            current.remote_url.clear();
            return;
        }
        current.state = "running";
        current.message = "Ready for agent connections.";
        current.remote_url = tailscale ? remote_url : std::string();
    }
    void stop(bool wait = false) override {
        std::lock_guard lock(mutex_);
        calls_.push_back({"stop", current.port, wait});
        current.state = "stopped";
        current.message = "MCP is off.";
        current.remote_url.clear();
    }

    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    std::function<bool()> session_access;
    ServerStatus current;
    std::string running_token = "fake-mcp-token";
    std::string start_error;                               // set: start() ends in this error
    std::string remote_url = "http://100.64.0.1:8767/mcp"; // with Tailscale access

private:
    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

} // namespace fv::agentapi
