#include "remote_host/mcp_session.h"

#include <chrono>
#include <utility>

#include "pyport/errors.h"

namespace fv::remote_host {
namespace {
using namespace std::chrono_literals;
}

PeerMcpSession::PeerMcpSession(Executor& timers, Listener listener)
    : timers_(timers), listener_(std::move(listener)) {}

bool PeerMcpSession::grant(std::string device) {
    std::lock_guard lock(mutex_);
    if (device.empty() || !peer_.empty()) {
        return false;
    }
    peer_ = std::move(device);
    expires_ = std::chrono::steady_clock::now() + 15min;
    const auto generation = ++generation_;
    timeout_ = timers_.post_after(15min, [self = weak_from_this(), generation] {
        if (auto session = self.lock()) {
            session->expire(generation);
        }
    });
    if (!timeout_.active()) {
        peer_.clear();
        return false;
    }
    return true;
}

bool PeerMcpSession::allowed(std::string_view device) const {
    std::lock_guard lock(mutex_);
    return !peer_.empty() && peer_ == device && std::chrono::steady_clock::now() < expires_;
}

void PeerMcpSession::revoke_locked() {
    timeout_.cancel();
    peer_.clear();
    if (!connection_.empty()) {
        connection_.clear();
        listener_.stop();
    }
}

void PeerMcpSession::expire(std::uint64_t generation) {
    std::lock_guard lock(mutex_);
    if (generation_ == generation) {
        revoke_locked();
    }
}

void PeerMcpSession::revoke() {
    std::lock_guard lock(mutex_);
    revoke_locked();
}

void PeerMcpSession::disconnect(std::string_view connection) {
    std::lock_guard lock(mutex_);
    if (!connection_.empty() && connection_ == connection) {
        revoke_locked();
    }
}

Json PeerMcpSession::request(std::string_view operation, const Json& header,
                             std::string_view device, std::string_view connection) {
    std::lock_guard lock(mutex_);
    if (peer_.empty() || peer_ != device || std::chrono::steady_clock::now() >= expires_) {
        throw py::ValueError("forbidden: The host has not granted this computer MCP control.");
    }
    const auto public_state = [this] {
        const auto state = listener_.status();
        return Json{{"state", state.value("state", std::string("stopped"))},
                    {"port", kPort},
                    {"tailscale", false}};
    };
    if (operation == "mcp_state") {
        return public_state();
    }
    const auto settings = header.find("settings");
    if (operation != "mcp_configure" || settings == header.end() || !settings->is_object() ||
        settings->size() != 1 || !settings->contains("enabled") ||
        !settings->at("enabled").is_boolean()) {
        throw py::ValueError("bad_request: This grant only controls the fixed loopback session.");
    }
    if (!connection_.empty() && connection_ != connection) {
        throw py::ValueError("forbidden: The MCP grant belongs to a different connection.");
    }
    if (settings->at("enabled") == true) {
        if (connection_.empty()) {
            if (connection.empty()) {
                throw py::ValueError("forbidden: This request has no authenticated connection.");
            }
            listener_.start();
            connection_ = connection;
        }
    } else {
        revoke_locked();
    }
    return public_state();
}

Json PeerMcpSession::local_status() const {
    std::lock_guard lock(mutex_);
    return listener_.status();
}

std::string PeerMcpSession::local_token() const {
    std::lock_guard lock(mutex_);
    return connection_.empty() || std::chrono::steady_clock::now() >= expires_
               ? std::string()
               : listener_.local_token();
}

} // namespace fv::remote_host
