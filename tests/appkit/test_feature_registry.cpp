// FeatureRegistry: features constructed from the registry table, ordered by required_features()
// then id, provide() of all before install() of any.

#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include "appkit/app_context.h"
#include "appkit/feature.h"

namespace {

std::vector<std::string>& journal() {
    static std::vector<std::string> entries;
    return entries;
}

// What "beta" provides and "alpha" uses during install.
struct BetaService {
    int value = 42;
};

class TestFeature : public fv::Feature {
public:
    TestFeature(std::string_view id, std::vector<std::string_view> required)
        : id_(id), required_(std::move(required)) {}
    std::string_view id() const override { return id_; }
    std::vector<std::string_view> required_features() const override { return required_; }
    void provide(fv::AppContext&) override { journal().push_back("provide " + std::string(id_)); }
    void install(fv::AppContext&) override { journal().push_back("install " + std::string(id_)); }

private:
    std::string_view id_;
    std::vector<std::string_view> required_;
};

class AardvarkFeature final : public TestFeature {
public:
    AardvarkFeature() : TestFeature("aardvark", {}) {}
};

class BetaFeature final : public TestFeature {
public:
    BetaFeature() : TestFeature("beta", {}) {}
    void provide(fv::AppContext& context) override {
        TestFeature::provide(context);
        context.provide(std::make_shared<BetaService>());
    }
};

class AlphaFeature final : public TestFeature {
public:
    AlphaFeature() : TestFeature("alpha", {"beta", "beta"}) {}
    void install(fv::AppContext& context) override {
        TestFeature::install(context);
        journal().push_back("alpha sees " + std::to_string(context.service<BetaService>().value));
    }
};

template <class T> std::unique_ptr<fv::Feature> make() {
    return std::make_unique<T>();
}

std::unique_ptr<fv::Feature> make_feature(std::string_view id,
                                          std::vector<std::string_view> required) {
    return std::make_unique<TestFeature>(id, std::move(required));
}

std::vector<std::string> ids(const std::vector<std::unique_ptr<fv::Feature>>& features) {
    std::vector<std::string> found;
    for (const auto& feature : features) {
        found.emplace_back(feature->id());
    }
    return found;
}

} // namespace

// This executable's registry table, standing in for the generated registry.cpp: sorted by id,
// as CMake writes it.
namespace fv {
std::span<const FeatureFactory> registered_features() {
    static const std::array<FeatureFactory, 3> table{{
        {"aardvark", &make<AardvarkFeature>},
        {"alpha", &make<AlphaFeature>},
        {"beta", &make<BetaFeature>},
    }};
    return table;
}
} // namespace fv

TEST_CASE("createAll constructs the registry's features and install runs after every provide",
          "[widget]") {
    journal().clear();
    auto features = fv::FeatureRegistry::createAll();
    // alpha requires beta; otherwise by id.
    CHECK(ids(features) == std::vector<std::string>{"aardvark", "beta", "alpha"});

    fv::AppContext context(fv::AppServices{});
    fv::FeatureRegistry::run(features, context);
    CHECK(journal() == std::vector<std::string>{"provide aardvark", "provide beta", "provide alpha",
                                                "install aardvark", "install beta", "install alpha",
                                                "alpha sees 42"});
}

TEST_CASE("FeatureRegistry orders by required_features then id") {
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(make_feature("zeta", {}));
    features.push_back(make_feature("delta", {"zeta"}));
    features.push_back(make_feature("gamma", {"delta", "alpha"}));
    features.push_back(make_feature("alpha", {}));
    features.push_back(make_feature("beta", {"zeta"}));
    fv::FeatureRegistry::order(features);
    CHECK(ids(features) == std::vector<std::string>{"alpha", "zeta", "beta", "delta", "gamma"});

    std::vector<std::unique_ptr<fv::Feature>> none;
    fv::FeatureRegistry::order(none);
    CHECK(none.empty());
}

TEST_CASE("FeatureRegistry fails naming a missing requirement") {
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(make_feature("alpha", {"beta"}));
    features.push_back(make_feature("meeting", {"history"}));
    features.push_back(make_feature("beta", {}));
    CHECK_THROWS_MATCHES(fv::FeatureRegistry::order(features), std::invalid_argument,
                         Catch::Matchers::Message("The feature 'meeting' requires the feature "
                                                  "'history', which is not linked into this "
                                                  "executable."));
}

TEST_CASE("FeatureRegistry rejects duplicate ids and requirement cycles") {
    std::vector<std::unique_ptr<fv::Feature>> twice;
    twice.push_back(make_feature("alpha", {}));
    twice.push_back(make_feature("alpha", {}));
    CHECK_THROWS_MATCHES(fv::FeatureRegistry::order(twice), std::invalid_argument,
                         Catch::Matchers::Message("Two features have the id 'alpha'."));

    std::vector<std::unique_ptr<fv::Feature>> cycle;
    cycle.push_back(make_feature("free", {}));
    cycle.push_back(make_feature("b", {"c"}));
    cycle.push_back(make_feature("a", {"b"}));
    cycle.push_back(make_feature("c", {"a"}));
    CHECK_THROWS_MATCHES(fv::FeatureRegistry::order(cycle), std::invalid_argument,
                         Catch::Matchers::Message("The features a, b, c cannot be ordered: their "
                                                  "requirements form a cycle."));
    // The caller still owns every feature.
    for (const auto& feature : cycle) {
        CHECK(feature != nullptr);
    }

    std::vector<std::unique_ptr<fv::Feature>> self;
    self.push_back(make_feature("self", {"self"}));
    CHECK_THROWS_AS(fv::FeatureRegistry::order(self), std::invalid_argument);
}

TEST_CASE("create checks each factory's feature against its registered id") {
    static const std::array<fv::FeatureFactory, 2> table{{
        {"alpha", &make<AlphaFeature>},
        {"beta_typo", &make<BetaFeature>},
    }};
    CHECK_THROWS_MATCHES(fv::FeatureRegistry::create(table), std::invalid_argument,
                         Catch::Matchers::Message("The feature registered as 'beta_typo' reports "
                                                  "the id 'beta'."));

    static const std::array<fv::FeatureFactory, 1> empty{{
        {"nothing",
         [] {
             return std::unique_ptr<fv::Feature>();
         }},
    }};
    CHECK_THROWS_MATCHES(
        fv::FeatureRegistry::create(empty), std::invalid_argument,
        Catch::Matchers::Message("The factory of the feature 'nothing' made no feature."));
}

TEST_CASE("a failing feature stops the run", "[widget]") {
    class Failing final : public TestFeature {
    public:
        Failing() : TestFeature("broken", {}) {}
        void provide(fv::AppContext&) override { throw std::runtime_error("no device"); }
    };
    journal().clear();
    std::vector<std::unique_ptr<fv::Feature>> features;
    features.push_back(make_feature("after", {"broken"}));
    features.push_back(std::make_unique<Failing>());
    fv::AppContext context(fv::AppServices{});
    CHECK_THROWS_AS(fv::FeatureRegistry::run(features, context), std::runtime_error);
    CHECK(journal().empty());
}
