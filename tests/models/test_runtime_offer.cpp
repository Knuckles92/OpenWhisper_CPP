// The first-use GPU runtime offer: the gpu_runtime_offer tests of OpenWhisper's
// tests/test_local_asr.py and the gpu_runtime_offer vectors of services.local_asr.catalog.

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <set>
#include <string>

#include "golden/golden.h"
#include "models/runtime_offer.h"
#include "models/runtime_resolver.h"
#include "models_test_support.h"

using namespace fv::models;
using namespace fv::models::test;
using fv::Json;
namespace golden = fv::golden;

namespace {

using Installed = std::set<std::string, std::less<>>;

// fresh_gpu_install: no speech runtime yet, on an RTX 2060, with the Vulkan loader present.
struct FreshGpuInstall {
    std::shared_ptr<Installed> installed = std::make_shared<Installed>();
    std::optional<NvidiaGpu> card = gpu("NVIDIA GeForce RTX 2060", 6144, std::pair{7, 5});
    std::string platform = "win_amd64";
    bool vulkan_loader = true;

    Host host() const {
        Host host = host_for(platform, card);
        host.is_installed = [installed = installed](std::string_view id) {
            return installed->contains(id);
        };
        return host;
    }
    std::optional<std::string> offer(std::string_view model, const Json& settings = Json::object(),
                                     const Json* catalog = nullptr) const {
        RuntimeOfferProbes probes;
        probes.catalog = catalog;
        probes.vulkan_loader_present = [loader = vulkan_loader] {
            return loader;
        };
        return gpu_runtime_offer(model, settings, host(), probes);
    }
    std::optional<std::string> missing(std::string_view model,
                                       const Json& settings = Json::object()) const {
        return missing_runtime(model, settings, host());
    }
};

} // namespace

TEST_CASE("test_first_use_offers_the_gpu_runtime_auto_would_pick", "[test_local_asr]") {
    // from tests/test_local_asr.py; Nemotron is not a OpenWhisper_CPP model.
    for (const std::string platform : {"win_amd64", "linux_x86_64"}) {
        FreshGpuInstall fresh;
        fresh.platform = platform;
        for (const std::string_view model : {"parakeet-v3", "orukeet-v0.1"}) {
            INFO(platform << " " << model);
            CHECK(fresh.offer(model) == "asr-nvidia-cuda");
            // Auto's fallback is still what is missing; the GPU one is the offer.
            CHECK(fresh.missing(model) == "asr-nvidia-cpu");
        }
        for (const std::string_view model :
             {"qwen-0.6b", "moonshine-small", "base", "turbo", "nemotron-3.5"}) {
            CHECK_FALSE(fresh.offer(model));
        }
    }
}

TEST_CASE("test_explicit_devices_keep_their_own_runtime (gpu_runtime_offer half)",
          "[test_local_asr]") {
    FreshGpuInstall fresh;
    const Json cpu = {{"local_asr_devices", {{"parakeet", "cpu"}}}};
    const Json cuda = {{"local_asr_devices", {{"parakeet", "cuda"}}}};
    CHECK_FALSE(fresh.offer("parakeet-v3", cpu));
    CHECK_FALSE(fresh.offer("parakeet-v3", cuda));
}

TEST_CASE("test_gpu_runtime_offer_is_first_use_only", "[test_local_asr]") {
    FreshGpuInstall fresh;
    fresh.installed->insert("asr-nvidia-cpu");
    CHECK_FALSE(fresh.offer("parakeet-v3"));
    CHECK_FALSE(fresh.missing("parakeet-v3"));
    fresh.installed->clear();
    fresh.installed->insert("asr-nvidia-cuda");
    CHECK_FALSE(fresh.offer("parakeet-v3"));
    CHECK_FALSE(fresh.missing("parakeet-v3"));
}

TEST_CASE("test_no_gpu_runtime_offer_where_auto_would_not_use_it", "[test_local_asr]") {
    FreshGpuInstall fresh;
    fresh.card.reset();
    CHECK_FALSE(fresh.offer("parakeet-v3"));
    // OpenWhisper's second half (CTranslate2 cannot count the card) has no OpenWhisper_CPP
    // counterpart: Auto asks NVML, so a card NVML reports is the card Auto uses.
}

TEST_CASE("test_windows_offers_cuda_only_to_a_known_turing_or_newer_card", "[test_local_asr]") {
    // Windows has no Vulkan release, so these cards' runtime is CUDA, which needs Turing.
    for (const auto capability : {std::optional<std::pair<int, int>>(std::pair{6, 1}),
                                  std::optional<std::pair<int, int>>()}) {
        FreshGpuInstall fresh;
        fresh.card = gpu("NVIDIA GeForce GTX 1050 Ti", 4096, capability);
        CHECK_FALSE(fresh.offer("parakeet-v3"));
    }
}

TEST_CASE("test_linux_offers_vulkan_to_an_older_card_with_the_loader", "[test_local_asr]") {
    // OpenWhisper's test asks it of Nemotron; Parakeet takes the same runtime.
    FreshGpuInstall fresh;
    fresh.platform = "linux_x86_64";
    fresh.card = gpu("NVIDIA GeForce GTX 1050 Ti", 4096, std::pair{6, 1});
    CHECK(fresh.offer("parakeet-v3") == "asr-nvidia-vulkan");
    fresh.vulkan_loader = false;
    CHECK_FALSE(fresh.offer("parakeet-v3"));
}

TEST_CASE("test_no_gpu_runtime_offer_this_computer_cannot_load", "[test_local_asr]") {
    FreshGpuInstall fresh;
    // A catalog whose CUDA build is for another version of the app (check_compatibility fails).
    Json catalog = builtin_catalog();
    catalog["components"]["asr-nvidia-cuda"]["platforms"]["win_amd64"]["component_api"] = 99;
    CHECK_FALSE(fresh.offer("parakeet-v3", Json::object(), &catalog));
    // Apple Silicon's Auto is already the Metal runtime.
    fresh.platform = "darwin_arm64";
    CHECK_FALSE(fresh.offer("parakeet-v3"));
}

TEST_CASE("services.local_asr.catalog.gpu_runtime_offer vectors", "[golden]") {
    golden::for_each("services.local_asr.catalog", "gpu_runtime_offer", [](const golden::Case& c) {
        auto installed = std::make_shared<Installed>();
        for (const Json& id : c.args()[3]) {
            installed->insert(id.get<std::string>());
        }
        Host host;
        host.platform = c.arg<std::string>(2);
        host.is_installed = [installed](std::string_view id) {
            return installed->contains(id);
        };
        const Json& card = c.args()[4];
        if (!card.is_null()) {
            std::optional<std::pair<int, int>> capability;
            if (!card.at(2).is_null()) {
                capability = std::pair{card.at(2).at(0).get<int>(), card.at(2).at(1).get<int>()};
            }
            const NvidiaGpu found =
                gpu(card.at(0).get<std::string>(), card.at(1).get<std::int64_t>(), capability);
            host.gpu = [found] {
                return std::optional<NvidiaGpu>(found);
            };
        }
        RuntimeOfferProbes probes;
        probes.vulkan_loader_present = [loader = c.arg<bool>(5)] {
            return loader;
        };
        const auto offer = gpu_runtime_offer(c.arg<std::string>(0), c.args()[1], host, probes);
        CHECK(golden::equal(c.result<Json>(), offer ? Json(*offer) : Json()));
    });
}
