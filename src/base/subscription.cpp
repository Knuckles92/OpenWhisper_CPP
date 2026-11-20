#include "base/subscription.h"

#include <algorithm>

namespace fv {

bool CallbackGuard::enter() {
    std::lock_guard lock(mutex_);
    if (closed_) {
        return false;
    }
    running_.push_back(std::this_thread::get_id());
    return true;
}

void CallbackGuard::leave() {
    {
        std::lock_guard lock(mutex_);
        const auto self = std::this_thread::get_id();
        const auto it = std::find(running_.begin(), running_.end(), self);
        if (it != running_.end()) {
            running_.erase(it);
        }
    }
    idle_.notify_all();
}

void CallbackGuard::close() {
    std::unique_lock lock(mutex_);
    closed_ = true;
    const auto self = std::this_thread::get_id();
    idle_.wait(lock, [&] {
        return std::all_of(running_.begin(), running_.end(),
                           [&](const std::thread::id& id) { return id == self; });
    });
}

bool CallbackGuard::closed() const {
    std::lock_guard lock(mutex_);
    return closed_;
}

Subscription::Subscription(std::shared_ptr<CallbackGuard> guard, std::function<void()> unlink)
    : guard_(std::move(guard)), unlink_(std::move(unlink)) {}

Subscription::Subscription(Subscription&& other) noexcept
    : guard_(std::move(other.guard_)), unlink_(std::move(other.unlink_)) {}

Subscription& Subscription::operator=(Subscription&& other) noexcept {
    if (this != &other) {
        disconnect();
        guard_ = std::move(other.guard_);
        unlink_ = std::move(other.unlink_);
    }
    return *this;
}

Subscription::~Subscription() {
    disconnect();
}

void Subscription::disconnect() {
    auto guard = std::move(guard_);
    auto unlink = std::move(unlink_);
    guard_.reset();
    unlink_ = nullptr;
    if (guard) {
        guard->close();
    }
    if (unlink) {
        unlink();
    }
}

bool Subscription::connected() const {
    if (guard_) {
        return !guard_->closed();
    }
    return static_cast<bool>(unlink_);
}

} // namespace fv
