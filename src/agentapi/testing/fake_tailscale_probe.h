#pragma once

#include <atomic>
#include <mutex>
#include <utility>

#include "agentapi/tailscale_probe.h"

namespace fv::agentapi {

// A TailscaleProbe that answers what set() gave it ("not_installed" at first) and counts the
// questions.
// Affinity: thread-safe.
class FakeTailscaleProbe final : public TailscaleProbe {
public:
    FakeTailscaleProbe() { answer_.state = "not_installed"; }
    explicit FakeTailscaleProbe(net::TailscaleStatus status) : answer_(std::move(status)) {}

    net::TailscaleStatus status() override {
        ++asked;
        std::lock_guard lock(mutex_);
        return answer_;
    }
    void set(net::TailscaleStatus status) {
        std::lock_guard lock(mutex_);
        answer_ = std::move(status);
    }

    std::atomic<int> asked{0};

private:
    std::mutex mutex_;
    net::TailscaleStatus answer_;
};

} // namespace fv::agentapi
