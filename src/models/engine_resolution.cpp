#include "models/engine_resolution.h"

#include <algorithm>
#include <cctype>

#include "base/utf8.h"
#include "engine/runtime.h"
#include "models/component_runtime.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "models/speech_model_cache.h"
#include "settings/keys.h"

namespace fv::models {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kBackend = "parakeet";

std::string upper_ascii(std::string_view text) {
    std::string out(text);
    std::ranges::transform(out, out.begin(),
                           [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return out;
}

// _runtime_kind for the Parakeet backend.
std::string_view runtime_kind(std::string_view device) {
    return device == "cuda" || device == "mps" || device == "metal" ? "GPU" : "CPU";
}

bool sentinel_present(const fs::path& components_root, std::string_view component_id) {
    std::error_code error;
    return fs::is_regular_file(
        components_root / fv::base_utf8::path_from_utf8(component_id) / ".installed", error);
}

} // namespace

bool runtime_dir_serves(const fs::path& runtime_dir, std::string_view component_id) {
    try {
        find_runtime_library(runtime_dir);
    } catch (const std::exception&) {
        return false;
    }
    return !is_gpu_runtime(component_id) || runtime_has_gpu_backend(runtime_dir);
}

std::function<bool(std::string_view)> engine_runtime_installed(const Json& settings,
                                                               fs::path components_root) {
    const std::string override_dir = keys::fv_runtime_dir.read(settings);
    if (!override_dir.empty()) {
        return [chosen = fv::base_utf8::path_from_utf8(override_dir)](std::string_view id) {
            return runtime_dir_serves(chosen, id);
        };
    }
    return [root = std::move(components_root)](std::string_view component_id) {
        return sentinel_present(root, component_id);
    };
}

ResolvedEngine resolve_parakeet(const Json& settings, const Paths& paths) {
    return resolve_parakeet(settings, paths, Host::current({}));
}

ResolvedEngine resolve_parakeet(const Json& settings, const Paths& paths, Host host) {
    const fs::path components_root = paths.components_root();
    const std::string override_dir = keys::fv_runtime_dir.read(settings);
    const bool overridden = !override_dir.empty();
    // A chosen folder is whichever runtime the device needs, when it is one: Auto keeps the CPU
    // with a CPU build.
    host.is_installed = engine_runtime_installed(settings, components_root);

    const std::string name(backend(kBackend)->label);
    ResolvedEngine resolved;
    resolved.model_key = selected_model(settings, kBackend);
    resolved.model_label = std::string(model(resolved.model_key)->label);
    resolved.requested_device = selected_device(settings, kBackend);

    const std::string runtime = keys::fv_speech_runtime.read(settings);
    std::string component;
    std::string device;
    if (runtime == "cpu") {
        component = runtime_id(kBackend, "cpu", host);
        device = "cpu";
    } else if (runtime == "cuda") {
        component = runtime_id(kBackend, "cuda", host);
        if (resolved.requested_device == "cpu") {
            device = "cpu";
        } else {
            device = apple_silicon(host) ? "metal" : "cuda";
        }
    } else {
        std::tie(component, device) = resolve_runtime(kBackend, resolved.requested_device, host);
    }
    resolved.device = device;
    resolved.gpu_index = device == "cpu" ? -1 : 0;
    resolved.runtime_id = component;

    if (overridden && !host.installed(component)) {
        // The folder the user chose, not a component, is what is missing: say what is wrong
        // with it.
        const fs::path chosen = fv::base_utf8::path_from_utf8(override_dir);
        resolved.missing_state = EngineMissing::RuntimeMissing;
        try {
            find_runtime_library(chosen);
            resolved.missing_message = override_dir + " holds a CPU build of NeMo-Speech.cpp, so " +
                                       name + " can't use the GPU with it.";
        } catch (const std::exception& error) {
            resolved.missing_message = error.what();
        }
    } else if (!host.installed(component)) {
        // _explain_missing_runtime
        const auto entry = catalog_entry(component, host);
        const auto reason = entry ? check_compatibility(*entry, host) : std::nullopt;
        resolved.missing_state = EngineMissing::RuntimeMissing;
        if (!entry) {
            resolved.runtime_id.clear();
            resolved.missing_message =
                name + "'s " + upper_ascii(device) + " runtime is not available on this platform.";
        } else if (reason) {
            resolved.runtime_id.clear();
            resolved.missing_message =
                name + " is not available on this computer. " + *reason + ".";
        } else {
            resolved.missing_message = "Install " + name + "'s " +
                                       std::string(runtime_kind(device)) + " runtime in Downloads.";
        }
    } else {
        resolved.runtime_dir = overridden
                                   ? fv::base_utf8::path_from_utf8(override_dir)
                                   : components_root / fv::base_utf8::path_from_utf8(component);
    }

    const fs::path model_folder =
        paths.speech_models_root() / fv::base_utf8::path_from_utf8(resolved.model_key);
    const Json& pinned = artifacts(resolved.model_key);
    if (!is_model_folder_complete(model_folder, pinned)) {
        if (resolved.runtime_dir) {
            resolved.missing_state = EngineMissing::ModelMissing;
            resolved.missing_message = "Download " + resolved.model_label + " in Downloads.";
        }
        return resolved;
    }
    resolved.model_path = model_folder;
    for (const Json& file : pinned.at("files")) {
        const auto& file_name = file.at("name").get_ref<const std::string&>();
        if (file_name.ends_with(".gguf")) {
            resolved.model_path = model_folder / fv::base_utf8::path_from_utf8(file_name);
            break;
        }
    }
    return resolved;
}

} // namespace fv::models
