#include "appkit/feature.h"

#include <algorithm>
#include <exception>
#include <format>
#include <map>
#include <set>
#include <stdexcept>
#include <string>

#include "base/log.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"appkit.features"};

template <class Phase>
void run_phase(std::vector<std::unique_ptr<Feature>>& features, std::string_view phase,
               Phase&& call) {
    for (auto& feature : features) {
        try {
            call(*feature);
        } catch (const std::exception& error) {
            kLog.error_with(error,
                            std::format("The feature {} failed in {}()", feature->id(), phase));
            throw;
        } catch (...) {
            kLog.error("The feature {} failed in {}() with a non-standard exception", feature->id(),
                       phase);
            throw;
        }
    }
}

} // namespace

std::vector<std::unique_ptr<Feature>>
FeatureRegistry::create(std::span<const FeatureFactory> factories) {
    std::vector<std::unique_ptr<Feature>> features;
    features.reserve(factories.size());
    for (const auto& factory : factories) {
        auto feature = factory.make();
        if (!feature) {
            throw std::invalid_argument(
                std::format("The factory of the feature '{}' made no feature.", factory.id));
        }
        // Requirements name the fv_feature() ids, so a class that reports another id would be
        // ordered (and found) under a name its CMakeLists.txt does not declare.
        if (feature->id() != factory.id) {
            throw std::invalid_argument(std::format(
                "The feature registered as '{}' reports the id '{}'.", factory.id, feature->id()));
        }
        features.push_back(std::move(feature));
    }
    order(features);
    return features;
}

void FeatureRegistry::order(std::vector<std::unique_ptr<Feature>>& features) {
    std::map<std::string_view, std::size_t> index;
    for (std::size_t i = 0; i < features.size(); ++i) {
        const auto id = features[i]->id();
        if (!index.emplace(id, i).second) {
            throw std::invalid_argument(std::format("Two features have the id '{}'.", id));
        }
    }

    // Kahn's algorithm, always taking the smallest ready id.
    std::vector<std::set<std::string_view>> waits_for(features.size());
    std::map<std::string_view, std::vector<std::string_view>> required_by;
    for (const auto& feature : features) {
        const auto id = feature->id();
        for (const auto required : feature->required_features()) {
            if (!index.contains(required)) {
                throw std::invalid_argument(
                    std::format("The feature '{}' requires the feature '{}', which is not "
                                "linked into this executable.",
                                id, required));
            }
            if (waits_for[index[id]].insert(required).second) {
                required_by[required].push_back(id);
            }
        }
    }
    std::set<std::string_view> ready;
    for (const auto& [id, i] : index) {
        if (waits_for[i].empty()) {
            ready.insert(id);
        }
    }
    std::vector<std::unique_ptr<Feature>> sorted;
    sorted.reserve(features.size());
    while (!ready.empty()) {
        const auto id = *ready.begin();
        ready.erase(ready.begin());
        sorted.push_back(std::move(features[index[id]]));
        for (const auto dependent : required_by[id]) {
            auto& waits = waits_for[index[dependent]];
            waits.erase(id);
            if (waits.empty()) {
                ready.insert(dependent);
            }
        }
    }
    if (sorted.size() != features.size()) {
        std::string cycle;
        for (const auto& [id, i] : index) {
            if (features[i]) {
                cycle += cycle.empty() ? "" : ", ";
                cycle += id;
            }
        }
        // Back where they were, so the caller still owns every feature.
        for (auto& feature : sorted) {
            features[index[feature->id()]] = std::move(feature);
        }
        throw std::invalid_argument(std::format(
            "The features {} cannot be ordered: their requirements form a cycle.", cycle));
    }
    features = std::move(sorted);
}

void FeatureRegistry::run(std::vector<std::unique_ptr<Feature>>& features, AppContext& context) {
    order(features);
    run_phase(features, "provide", [&](Feature& feature) { feature.provide(context); });
    run_phase(features, "install", [&](Feature& feature) { feature.install(context); });
}

} // namespace fv
