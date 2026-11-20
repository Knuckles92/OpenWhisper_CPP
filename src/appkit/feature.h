#pragma once

#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace fv {

class AppContext;

// One feature library's entry point, declared with fv_feature(ID ... CLASS ...) in its
// CMakeLists.txt and constructed by the generated registry.
// Affinity: UI actor.
class Feature {
public:
    virtual ~Feature() = default;
    virtual std::string_view id() const = 0;
    // The ids of the features whose provide() must run first. ("requires" is a C++20 keyword.)
    virtual std::vector<std::string_view> required_features() const { return {}; }
    virtual void provide(AppContext&) {}   // phase 1: construct services
    virtual void install(AppContext&) = 0; // phase 2: wire, register UI
};

struct FeatureFactory {
    std::string_view id;
    std::unique_ptr<Feature> (*make)();
};

// Every feature linked into the executable, sorted by id. Defined in the registry.cpp that
// CMake generates from the fv_feature() calls (the fv_feature_registry library).
std::span<const FeatureFactory> registered_features();

// Builds the features and runs their two phases in a fixed order: a feature comes after every
// feature it requires, and otherwise by id. Nothing registers itself at static-init time.
// Affinity: UI actor.
class FeatureRegistry {
public:
    // Every feature of the generated registry, constructed and put in order. Inline, so the
    // reference to registered_features() is in the executable's own object and resolves against
    // fv_feature_registry whatever the static-library order on the link line.
    static std::vector<std::unique_ptr<Feature>> createAll() {
        return create(registered_features());
    }
    // The same from any table: constructs each factory's feature, then order(). Throws
    // std::invalid_argument when a factory makes nothing or a feature whose id() is not the
    // factory's id.
    static std::vector<std::unique_ptr<Feature>> create(std::span<const FeatureFactory>);
    // Sorts in place. Throws std::invalid_argument naming the feature and the id when a
    // requirement is not among the features, when two features share an id, and naming the
    // features involved when requirements form a cycle.
    static void order(std::vector<std::unique_ptr<Feature>>&);
    // order(), then provide() of every feature, then install() of every feature, in that order.
    // An exception from a feature is logged naming it and rethrown.
    static void run(std::vector<std::unique_ptr<Feature>>&, AppContext&);
};

} // namespace fv
