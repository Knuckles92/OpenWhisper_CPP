#include "remote_host/runtime_state.h"

#include <algorithm>
#include <array>
#include <string>

#include "models/component_details.h"
#include "models/consent_text.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "speech/languages.h"

namespace fv::remote_host {
namespace {

const Json& member(const Json& object, std::string_view key) {
    static const Json missing;
    const auto at = object.find(std::string(key));
    return at == object.end() ? missing : *at;
}

bool equals_text(const Json& value, std::string_view wanted) {
    return value.is_string() && value.get_ref<const std::string&>() == wanted;
}

constexpr std::string_view kFamily = "parakeet";

} // namespace

Json runtime_dependencies(const models::Host& host) {
    Json result = Json::array();
    const auto gpu = host.nvidia();
    const std::array<std::string_view, 2> devices =
        models::apple_silicon(host) ? std::array<std::string_view, 2>{"auto", "cpu"}
                                    : std::array<std::string_view, 2>{"cpu", "cuda"};
    for (const auto device : devices) {
        const auto component = models::runtime_id(kFamily, device, host);
        const bool installed = !component.empty() && host.installed(component);
        std::string reason;
        if (!models::catalog_entry(component, host)) {
            reason = "This runtime is not available for the host's platform.";
        } else if (device == "cuda" && !gpu) {
            reason = "No NVIDIA GPU detected on the host. Check its NVIDIA driver, or use CPU.";
        } else if (component == models::component_id::kAsrNvidiaCuda && gpu &&
                   gpu->compute_capability &&
                   *gpu->compute_capability < models::kCudaMinComputeCapability) {
            reason = "This speech GPU runtime requires an NVIDIA Turing or newer GPU. Use CPU on "
                     "this host.";
        }
        result.push_back({
            {"device", device},
            {"component", component},
            {"label", models::component_copy(component).first},
            {"ready", installed && reason.empty()},
            {"installed", installed},
            {"installable", !component.empty() && !installed && reason.empty()},
            {"reason", reason},
            {"download_bytes", models::runtime_sizes(component, host).download},
        });
    }
    return result;
}

Json runtime_state(const Json& engine, const Json& settings, const models::Host& host) {
    if (!equals_text(member(engine, "family"), kFamily)) {
        return Json::object();
    }
    const auto dependencies = runtime_dependencies(host);
    Json devices = Json::array();
    if (std::ranges::any_of(dependencies,
                            [](const Json& entry) { return entry["ready"] == true; })) {
        devices.push_back("auto");
    }
    for (const auto& entry : dependencies) {
        if (entry["ready"] == true && entry["device"] != "auto") {
            devices.push_back(entry["device"]);
        }
    }
    const auto language = keys::local_asr_language.read(settings);
    Json languages = Json::array();
    for (const auto code : speech::language_choices(kFamily)) {
        languages.push_back(code);
    }
    const auto gpu = host.nvidia();
    return {
        {"family", kFamily},
        {"model", member(engine, "model").is_string() ? member(engine, "model") : Json("")},
        {"selected",
         {{"device", models::selected_device(settings, kFamily)},
          {"language", speech::selected_language(kFamily, std::string_view(language))}}},
        {"devices", std::move(devices)},
        {"compute_types", Json::object()},
        {"languages", std::move(languages)},
        {"gpu", gpu ? Json{{"name", gpu->name}, {"total_mib", gpu->total_mib}} : Json::object()},
        {"dependencies", dependencies},
    };
}

Json validate_runtime(const Json& engine, const Json& settings, const models::Host& host,
                      std::string_view family, std::string_view model, const Json& changes) {
    if (!equals_text(member(engine, "family"), family) ||
        !equals_text(member(engine, "model"), model)) {
        throw py::ValueError("The host changed engines. Reconnect before changing its runtime.");
    }
    if (!changes.is_object() || changes.empty()) {
        throw py::ValueError("Choose a device or precision supported by the host.");
    }
    for (const auto& item : changes.items()) {
        if (item.key() != "device" && item.key() != "compute_type" && item.key() != "language") {
            throw py::ValueError("Choose a device or precision supported by the host.");
        }
    }
    const auto state = runtime_state(engine, settings, host);
    if (state.empty()) {
        throw py::ValueError("Select a local speech engine on the host first.");
    }
    const auto& selected = state["selected"];
    const Json& device = changes.contains("device") ? changes["device"] : selected["device"];
    if (!device.is_string() ||
        std::ranges::find(state["devices"], device) == state["devices"].end()) {
        throw py::ValueError("That device isn't ready on the host. Check its hardware and "
                             "installed runtimes.");
    }
    if (changes.contains("compute_type")) {
        throw py::ValueError("This engine manages its own precision on the host.");
    }
    const Json& language =
        changes.contains("language") ? changes["language"] : selected["language"];
    if (std::ranges::find(state["languages"], language) == state["languages"].end()) {
        throw py::ValueError("Choose a language supported by this engine on the host.");
    }
    auto devices = keys::local_asr_devices.read(settings);
    devices[std::string(family)] = device;
    return {{std::string(keys::local_asr_devices.key()), std::move(devices)},
            {std::string(keys::local_asr_language.key()), language}};
}

} // namespace fv::remote_host
