#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>

// Use counts against changes of a component's files (services/component_leases.py). A speech
// runtime that has been loaded holds a lease for the rest of the process, since it can never be
// unloaded: its folder is then never replaced while the app runs (architecture 5.7), and
// ComponentStore stages the change for the next start instead.
namespace fv::models {

// Affinity: thread-safe.
class ComponentLeases {
public:
    // A shared use of `component_id`'s files; releasing it is idempotent (the returned function
    // may be called any number of times, from any thread). Throws ComponentError "This component
    // is being installed or removed. Try again when Downloads finishes." while a mutation holds
    // it.
    std::function<void()> acquire(std::string_view component_id);

    // Whether anything holds a lease on the component.
    bool in_use(std::string_view component_id) const;

    // Held while the component's files change. Throws ComponentError "This component is in use.
    // Finish the meeting or report job before changing it." when it is leased or already
    // changing. Move-only; the mutation ends when it is destroyed.
    class Mutation {
    public:
        Mutation(Mutation&& other) noexcept;
        Mutation& operator=(Mutation&&) = delete;
        Mutation(const Mutation&) = delete;
        Mutation& operator=(const Mutation&) = delete;
        ~Mutation();

    private:
        friend class ComponentLeases;
        Mutation(ComponentLeases* owner, std::string component_id);
        ComponentLeases* owner_;
        std::string component_id_;
    };
    Mutation mutation(std::string_view component_id);

private:
    mutable std::recursive_mutex mutex_;
    std::map<std::string, int, std::less<>> users_;
    std::set<std::string, std::less<>> mutating_;
};

// The process's leases (component_leases.py's module state).
ComponentLeases& component_leases();

// acquire_component and component_mutation over component_leases().
std::function<void()> acquire_component(std::string_view component_id);
ComponentLeases::Mutation component_mutation(std::string_view component_id);

} // namespace fv::models
