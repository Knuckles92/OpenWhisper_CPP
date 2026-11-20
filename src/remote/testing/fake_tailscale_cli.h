#pragma once

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "remote/tailscale_cli.h"

namespace fv::remote::testing {

// A Tailscale CLI with scripted answers: status() returns what set_status gave ("not_installed"
// until then), whois(address) the identity set for that address (nullopt otherwise), and both
// record their calls. whois of an address that is not Tailscale's answers nullopt without being
// recorded as asked, as the real one never runs the CLI for it.
// Affinity: thread-safe.
class FakeTailscaleCli final : public TailscaleCli {
public:
    FakeTailscaleCli() { status_.state = "not_installed"; }

    void set_status(net::TailscaleStatus status) {
        std::lock_guard lock(mutex_);
        status_ = std::move(status);
    }
    void set_identity(std::string address, std::optional<PeerIdentity> identity) {
        std::lock_guard lock(mutex_);
        identities_[std::move(address)] = std::move(identity);
    }

    net::TailscaleStatus status() override {
        std::lock_guard lock(mutex_);
        ++status_calls_;
        return status_;
    }
    std::optional<PeerIdentity> whois(std::string_view address) override {
        if (!net::is_tailscale_address(address)) {
            return std::nullopt;
        }
        std::lock_guard lock(mutex_);
        looked_up_.emplace_back(address);
        const auto found = identities_.find(std::string(address));
        return found == identities_.end() ? std::nullopt : found->second;
    }

    int status_calls() const {
        std::lock_guard lock(mutex_);
        return status_calls_;
    }
    std::vector<std::string> looked_up() const {
        std::lock_guard lock(mutex_);
        return looked_up_;
    }

private:
    mutable std::mutex mutex_;
    net::TailscaleStatus status_;
    std::map<std::string, std::optional<PeerIdentity>> identities_;
    std::vector<std::string> looked_up_;
    int status_calls_ = 0;
};

} // namespace fv::remote::testing
