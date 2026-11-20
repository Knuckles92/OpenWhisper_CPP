#include "models/consent_text.h"

#include <vector>

#include "history/format_utils.h"
#include "models/component_details.h"
#include "models/hf_access.h"
#include "models/hf_access_policy.h"
#include "models/model_details.h"
#include "models/runtime_offer.h"
#include "models/runtime_resolver.h"
#include "models/speech_catalog.h"
#include "pyport/text.h"

namespace fv::models {

namespace {

constexpr std::string_view kNoBreakSpace = " ";

constexpr std::string_view kThirdPartyNotice =
    "This installs third-party executable software. Archive integrity checks do not guarantee "
    "security. Keep speech runtimes updated. See the runtime's source and license in Downloads.";

std::string join(const std::vector<std::string>& parts, std::string_view separator) {
    std::string joined;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) {
            joined += separator;
        }
        joined += parts[i];
    }
    return joined;
}

std::string replace_spaces(std::string_view text) {
    std::string replaced;
    for (const char c : text) {
        if (c == ' ') {
            replaced += kNoBreakSpace;
        } else {
            replaced += c;
        }
    }
    return replaced;
}

// _size: keeps "117 MB" on one line.
std::string unbroken_size(std::int64_t size_bytes) {
    return replace_spaces(format_size_bytes(size_bytes));
}

std::string runtime_name(std::string_view component_id) {
    return component_copy(component_id).first;
}

std::string label_of(std::string_view model_name) {
    const SpeechModel* found = model(model_name);
    return found != nullptr ? std::string(found->label) : std::string(model_name);
}

std::string noun_for(std::size_t count) {
    return count == 1 ? "model" : "models";
}

} // namespace

std::string consent_title(std::string_view model_name) {
    if (const auto size = format_download_size(model_name)) {
        return "Download \"" + std::string(model_name) + "\" (about " + py::lstrip(*size, "~") +
               ")?";
    }
    return "Download \"" + std::string(model_name) + "\" model?";
}

RuntimeNeeds runtime_needs(std::string_view model_name, const Json& settings, const Host& host) {
    RuntimeNeeds needs;
    needs.runtime = missing_runtime(model_name, settings, host);
    if (needs.runtime) {
        needs.gpu_runtime = gpu_runtime_offer(model_name, settings, host);
    }
    return needs;
}

std::string consent_body(std::string_view model_name, std::string_view policy, bool env_blocked,
                         const RuntimeNeeds& needs) {
    const ModelDetails* details = find_model_details(model_name);
    // resolve_model_repo: the catalog's repository, an unknown name unchanged.
    const std::string repo = details != nullptr ? details->repository_id : std::string(model_name);
    std::vector<std::string> lines;
    lines.push_back("The speech model \"" + label_of(model_name) + "\" is not on this computer.");
    if (details != nullptr &&
        details->download_hosts != std::vector<std::string>{"huggingface.co"}) {
        lines.push_back("Downloading connects to " + join(details->download_hosts, ", ") +
                        ". Source: " + repo + ".");
    } else {
        lines.push_back("It can be downloaded from Hugging Face (huggingface.co), repository " +
                        repo + ".");
    }

    if (details != nullptr) {
        lines.push_back("Publisher / maintainer: " + details->maintainer + ".");
        lines.push_back(
            "License: " + details->license + ". Terms: " +
            (details->license_url.empty() ? details->origin_url : details->license_url));
        lines.push_back(details->verification + ".");
        if (!details->revision.empty()) {
            lines.push_back("Selected version: " + details->revision + ".");
        }
    }
    lines.push_back(model_security_notice());
    if (details != nullptr && details->verification.find("SHA-256") != std::string::npos) {
        lines.push_back("Integrity checks do not guarantee security or accuracy.");
    }
    // The download size is in the title, where it stays in view.

    if (needs.runtime && needs.gpu_runtime) {
        lines.push_back("A speech runtime is also required to use this model: " +
                        runtime_name(*needs.gpu_runtime) + " for this computer's NVIDIA GPU, or " +
                        runtime_name(*needs.runtime) +
                        ". The model download alone will not enable transcription. After you "
                        "approve the model download, you will be asked which runtime to install.");
    } else if (needs.runtime) {
        lines.push_back(runtime_name(*needs.runtime) +
                        " is also required to use this model. The model download alone will not "
                        "enable transcription. After you approve the model download, you will "
                        "also be prompted to install the required runtime.");
    }

    if (env_blocked) {
        lines.emplace_back("Downloads are currently disabled by the HF_HUB_OFFLINE environment "
                           "variable set outside this application. Unset it and restart to allow "
                           "downloads.");
    } else if (policy == hf_access_policy::kNever) {
        lines.emplace_back("Your settings are set to \"Never connect\" to Hugging Face. You can "
                           "allow this one download, or change the policy in Settings.");
    }
    return join(lines, "\n\n");
}

std::string_view consent_storage_note() {
    return "Model files are stored locally on this computer. Once downloaded, and with its "
           "required runtime installed, this model works fully offline.";
}

RuntimeSizes runtime_sizes(std::string_view component_id, const Host& host) {
    RuntimeSizes sizes;
    const auto entry = catalog_entry(component_id, host);
    if (!entry) {
        return sizes;
    }
    if (const auto archives = entry->find("archives");
        archives != entry->end() && archives->is_array()) {
        for (const Json& archive : *archives) {
            sizes.download += archive.value("size_bytes", std::int64_t{0});
        }
    }
    sizes.on_disk = entry->value("install_bytes", std::int64_t{0});
    return sizes;
}

std::string required_runtime_title(std::string_view component_id) {
    return runtime_name(component_id) + " is required";
}

std::string required_runtime_body(std::string_view model_name, std::string_view component_id,
                                  std::int64_t download_bytes) {
    const std::string runtime = runtime_name(component_id);
    return label_of(model_name) + " needs both its model files and " + runtime +
           " to work. The runtime is the software that runs the model on this computer; "
           "downloading the model alone does not enable transcription.\n\nInstall " +
           runtime + " now? This is a separate " + format_size_bytes(download_bytes) +
           " download, shared by compatible models. After both downloads finish, the selected "
           "model loads automatically.\n\n" +
           std::string(kThirdPartyNotice) +
           "\n\nIf you choose Later, the model will remain unavailable until you install this "
           "runtime in Downloads.";
}

std::string gpu_runtime_title(std::string_view model_name, const std::optional<NvidiaGpu>& gpu) {
    const SpeechModel* found = model(model_name);
    const SpeechBackend* owner = found != nullptr ? backend(found->backend) : nullptr;
    const std::string backend_label =
        owner != nullptr ? std::string(owner->label) : std::string(model_name);
    const std::string card = gpu ? gpu->short_name() : std::string("this GPU");
    return "Run " + backend_label + " on the " + card + "?";
}

std::string gpu_runtime_body(std::string_view model_name, std::string_view gpu_component,
                             std::string_view cpu_component, const std::optional<NvidiaGpu>& gpu,
                             const RuntimeSizes& gpu_sizes, const RuntimeSizes& cpu_sizes) {
    const std::string card = gpu ? gpu->short_name() : std::string("this GPU");
    const std::string api = gpu_component == component_id::kAsrNvidiaVulkan ? "Vulkan" : "CUDA";
    return label_of(model_name) +
           " needs a speech runtime, the software that runs the model on this computer. "
           "Downloading the model alone does not enable transcription.\n\nThis computer's " +
           (gpu ? gpu->name : std::string("NVIDIA GPU")) +
           " can run it:\n\n• Use this GPU: " + runtime_name(gpu_component) +
           " runs the model on the " + replace_spaces(card) + " with " + api +
           ", usually several times faster than the CPU. " + unbroken_size(gpu_sizes.download) +
           " download, " + unbroken_size(gpu_sizes.on_disk) +
           " on disk.\n\n• Use the CPU: " + runtime_name(cpu_component) +
           " runs it on the processor. " + unbroken_size(cpu_sizes.download) + " download, " +
           unbroken_size(cpu_sizes.on_disk) +
           " on disk.\n\nYou can add the other runtime later in Downloads. The model loads "
           "automatically once it and the runtime are both downloaded.\n\n" +
           std::string(kThirdPartyNotice) +
           "\n\nIf you choose Later, the model stays unavailable until you install a runtime in "
           "Downloads.";
}

std::string batch_download_title(std::size_t count) {
    return "Download " + std::to_string(count) + " " + noun_for(count) + "?";
}

std::string batch_model_list(std::span<const std::string> model_names) {
    std::vector<std::string> lines;
    for (const std::string& name : model_names) {
        lines.push_back(name + "  ·  " + format_download_size(name).value_or("size unknown"));
    }
    return join(lines, "\n");
}

std::string batch_total_text(std::span<const std::string> model_names) {
    std::int64_t total_bytes = 0;
    for (const std::string& name : model_names) {
        const auto size_mb = model_download_size_mb(name);
        if (!size_mb) {
            return "Estimated total: unknown";
        }
        total_bytes += *size_mb * 1'000'000;
    }
    return "Estimated total: ~" + format_size_bytes(total_bytes);
}

std::string batch_download_button(std::size_t count) {
    return "Download " + std::to_string(count) + " " + noun_for(count) + "…";
}

std::string_view batch_note() {
    return "Models download one at a time, in the order listed. You can stop after the current "
           "model from Settings → Downloads while the queue runs. Models such as Parakeet "
           "also need a separate runtime to work; you will be prompted to install any missing "
           "required runtime.";
}

Confirmation delete_model_confirmation(std::string_view model_name) {
    return {"Delete Model", "Delete the downloaded files for \"" + std::string(model_name) +
                                "\"?\n\nYou can download the model again later."};
}

Confirmation remove_component_confirmation(std::string_view display_name,
                                           std::int64_t install_bytes) {
    return {"Remove component", "Remove " + std::string(display_name) + " (" +
                                    format_size_bytes(install_bytes) +
                                    ")?\n\nYou can install it again later."};
}

} // namespace fv::models
