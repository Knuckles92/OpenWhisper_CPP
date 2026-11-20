#include "models/component_leases.h"

#include <atomic>
#include <utility>

#include "models/catalog.h"

namespace fv::models {

std::function<void()> ComponentLeases::acquire(std::string_view component_id) {
    {
        std::lock_guard lock(mutex_);
        if (mutating_.contains(component_id)) {
            throw ComponentError("This component is being installed or removed. Try again when "
                                 "Downloads finishes.");
        }
        ++users_[std::string(component_id)];
    }
    auto released = std::make_shared<std::atomic<bool>>(false);
    return [this, released, id = std::string(component_id)] {
        std::lock_guard lock(mutex_);
        if (released->exchange(true)) {
            return;
        }
        const auto it = users_.find(id);
        if (it != users_.end() && --it->second == 0) {
            users_.erase(it);
        }
    };
}

bool ComponentLeases::in_use(std::string_view component_id) const {
    std::lock_guard lock(mutex_);
    const auto it = users_.find(component_id);
    return it != users_.end() && it->second > 0;
}

ComponentLeases::Mutation ComponentLeases::mutation(std::string_view component_id) {
    std::lock_guard lock(mutex_);
    if (in_use(component_id) || mutating_.contains(component_id)) {
        throw ComponentError(
            "This component is in use. Finish the meeting or report job before changing it.");
    }
    mutating_.emplace(component_id);
    return Mutation(this, std::string(component_id));
}

ComponentLeases::Mutation::Mutation(ComponentLeases* owner, std::string component_id)
    : owner_(owner), component_id_(std::move(component_id)) {}

ComponentLeases::Mutation::Mutation(Mutation&& other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)), component_id_(std::move(other.component_id_)) {}

ComponentLeases::Mutation::~Mutation() {
    if (owner_ != nullptr) {
        std::lock_guard lock(owner_->mutex_);
        const auto it = owner_->mutating_.find(component_id_);
        if (it != owner_->mutating_.end()) {
            owner_->mutating_.erase(it);
        }
    }
}

ComponentLeases& component_leases() {
    static ComponentLeases leases;
    return leases;
}

std::function<void()> acquire_component(std::string_view component_id) {
    return component_leases().acquire(component_id);
}

ComponentLeases::Mutation component_mutation(std::string_view component_id) {
    return component_leases().mutation(component_id);
}

} // namespace fv::models
