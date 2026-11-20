#include "models/component_details.h"

#include <array>
#include <format>
#include <stdexcept>

#include "base/identity.h"
#include "models/catalog.h"

namespace fv::models {

namespace {

constexpr std::string_view kSourceNote =
    "Download URLs and SHA-256 pins ship with the app. Archives are verified before extract, and "
    "an interruption leaves either the previous install or the new one — never a mixture.";

constexpr std::string_view kNvidiaSpeechSource = "https://github.com/NVIDIA/NeMo-Speech.cpp";

struct RuntimeRow {
    std::string_view id;
    std::string_view name;
    std::string_view license;
    std::string_view summary;
    std::string_view requires_text;
};

// _SPEECH_RUNTIMES and _SPEECH_RUNTIME_REQUIRES for the NVIDIA rows. OpenWhisper's summaries say
// "shared by Parakeet and Nemotron" and its CPU and CUDA licences add "Python PSF" for the
// portable Python those archives carried there.
constexpr std::array<RuntimeRow, 4> kRows{{
    {component_id::kAsrNvidiaCpu, "NVIDIA Speech CPU", "Apache-2.0",
     "CPU runtime shared by Parakeet models.",
     "Windows x64, Linux x86_64 (glibc 2.31+), or macOS (Apple Silicon or Intel)"},
    {component_id::kAsrNvidiaCuda, "NVIDIA Speech GPU", "Apache-2.0; NVIDIA CUDA",
     "NVIDIA GPU runtime shared by Parakeet models.",
     "Windows x64 or Linux x86_64 (glibc 2.31+), and an NVIDIA GPU (Turing or newer) with its "
     "driver"},
    {component_id::kAsrNvidiaVulkan, "NVIDIA Speech GPU (Vulkan)", "Apache-2.0",
     "Vulkan GPU runtime shared by Parakeet models, for NVIDIA GPUs older than Turing, such as "
     "the GTX 10 series.",
     "Linux x86_64 (glibc 2.31+), and an NVIDIA GPU older than Turing with its driver and the "
     "Vulkan loader"},
    {component_id::kAsrNvidiaMetal, "NVIDIA Speech GPU (Metal)", "Apache-2.0",
     "Apple GPU runtime shared by Parakeet models on Apple Silicon Macs. Auto uses it once "
     "installed.",
     "Apple Silicon Mac, macOS 14 or newer"},
}};

const std::vector<ComponentDetails>& catalog() {
    static const std::vector<ComponentDetails> built = [] {
        const std::string product(identity().product);
        std::vector<ComponentDetails> rows;
        for (const RuntimeRow& row : kRows) {
            ComponentDetails details;
            details.component_id = std::string(row.id);
            details.display_name = std::string(row.name);
            details.summary = std::string(row.summary);
            // OpenWhisper: "... Runs in a dedicated process." Here the libraries load in process.
            details.description = std::string(row.summary) +
                                  " Models download separately. Runs inside " + product + ".";
            details.origin_name = std::string(row.name);
            details.origin_url = std::string(kNvidiaSpeechSource);
            details.origin_label = "Project ↗";
            details.source_name = "Pinned upstream archives";
            details.source_url = std::string(kNvidiaSpeechSource);
            details.source_label = "Project ↗";
            details.maintainer = "Upstream publishers; packaged by " + product;
            details.family = "Speech runtime";
            details.requires_text = std::string(row.requires_text);
            details.payload = "Verified native libraries";
            // OpenWhisper: "Isolated worker process".
            details.local_format = "Native libraries loaded by " + product;
            details.license = std::string(row.license);
            details.best_for = std::string(row.summary);
            details.limitations = {"Model weights are a separate download."};
            details.compact_tags = "Local speech";
            details.source_note = speech_runtime_source_note();
            details.source_urls = {std::string(kNvidiaSpeechSource)};
            rows.push_back(std::move(details));
        }
        return rows;
    }();
    return built;
}

} // namespace

std::string_view component_source_note() {
    return kSourceNote;
}

std::string speech_runtime_source_note() {
    return std::string(kSourceNote) +
           " Speech runtimes install third-party executable software. Integrity checks do not "
           "guarantee security; keep runtimes updated.";
}

std::span<const ComponentDetails> component_catalog() {
    return catalog();
}

const ComponentDetails* find_component_details(std::string_view component_id) {
    for (const ComponentDetails& details : catalog()) {
        if (details.component_id == component_id) {
            return &details;
        }
    }
    return nullptr;
}

const ComponentDetails& get_component_details(std::string_view component_id) {
    if (const ComponentDetails* details = find_component_details(component_id)) {
        return *details;
    }
    throw std::out_of_range(std::string(component_id));
}

std::pair<std::string, std::string> component_copy(std::string_view component_id) {
    if (const ComponentDetails* details = find_component_details(component_id)) {
        return {details->display_name, details->summary};
    }
    return {std::string(component_id), std::string()};
}

} // namespace fv::models
