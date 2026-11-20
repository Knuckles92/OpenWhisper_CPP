#include "models/runtime_offer.h"

#include "models/detail/native_library.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"

namespace fv::models {

bool vulkan_loader_present() {
    return detail::open_native_library("libvulkan.so.1").has_value();
}

std::optional<std::string> gpu_runtime_offer(std::string_view model_name, const Json& settings,
                                             const Host& host, const RuntimeOfferProbes& probes) {
    const SpeechModel* model = models::model(model_name);
    if (model == nullptr || (model->backend != "parakeet" && model->backend != "nemotron") ||
        apple_silicon(host) || selected_device(settings, model->backend) != "auto" ||
        host.installed(runtime_id(model->backend, "cpu", host))) {
        return std::nullopt;
    }
    const auto gpu = host.nvidia();
    if (!gpu || auto_device(model->backend, host) != "cuda") {
        return std::nullopt;
    }
    const std::string component = runtime_id(model->backend, "cuda", host);
    if (component == component_id::kAsrNvidiaVulkan) {
        const bool loader =
            probes.vulkan_loader_present ? probes.vulkan_loader_present() : vulkan_loader_present();
        if (!loader) {
            return std::nullopt;
        }
    } else if (!gpu->compute_capability || *gpu->compute_capability < kCudaMinComputeCapability) {
        // NVIDIA's CUDA release runs on Turing and newer; a card of unknown capability is not
        // offered a download it may not run.
        return std::nullopt;
    }
    const Json& catalog = probes.catalog != nullptr ? *probes.catalog : builtin_catalog();
    const std::string tag = host.platform.value_or(std::string());
    if (host.installed(component) || !component_is_published(component, tag, catalog) ||
        check_compatibility(catalog_entry(component, tag, catalog).value_or(Json::object()),
                            host)) {
        return std::nullopt;
    }
    return component;
}

} // namespace fv::models
