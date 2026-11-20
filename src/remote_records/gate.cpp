#include "remote_records/gate.h"

#include <utility>

namespace fv::remote_records {

RecordGate::Request RecordGate::request() {
    std::lock_guard lock(mutex_);
    if (paused_) {
        throw RecordsBusy("The host is backing up or recovering records. Try again shortly.");
    }
    ++active_;
    return Request(*this);
}

RecordGate::Request::~Request() {
    std::lock_guard lock(gate_.mutex_);
    --gate_.active_;
    gate_.changed_.notify_all();
}

RecordGate::Pause RecordGate::paused(std::chrono::milliseconds timeout) {
    std::unique_lock lock(mutex_);
    if (paused_) {
        throw RecordsBusy("Another backup or record recovery is already running.");
    }
    paused_ = true;
    if (!changed_.wait_for(lock, timeout, [this] { return active_ == 0; })) {
        paused_ = false;
        throw RecordsBusy(
            "Host record transfers are still finishing. Try again after sync finishes.");
    }
    return Pause(*this);
}

RecordGate::Pause::Pause(Pause&& other) noexcept : gate_(std::exchange(other.gate_, nullptr)) {}

RecordGate::Pause::~Pause() {
    if (gate_) {
        std::lock_guard lock(gate_->mutex_);
        gate_->paused_ = false;
    }
}

} // namespace fv::remote_records
