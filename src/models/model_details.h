#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The bundled profile of each downloadable speech model that Downloads, its inspector and the
// consent dialog show without contacting the network (services/model_catalog.py: ModelDetails,
// format_download_mb, MODEL_SECURITY_NOTICE, CUSTOM_MODEL_NOTICE, _speech_model_details and the
// Orukeet overrides). Only the Parakeet family is described; the Whisper, Moonshine, Qwen and MLX
// profiles are not ported.
// Affinity: thread-safe (immutable after first use).
namespace fv::models {

struct ModelDetails {
    std::string model_name;
    std::string description;
    std::string origin_name;
    std::string origin_url;
    std::string repository_id;
    std::string repository_url;
    std::string maintainer;
    std::string family;
    std::string language_support;
    std::string task_support;
    std::string parameter_count;
    std::string relative_performance;
    std::string memory_guidance;
    std::int64_t download_size_mb = 0; // decimal megabytes, rounded half to even
    std::string runtime_format;
    std::string license;
    std::string best_for;
    std::vector<std::string> limitations;
    std::vector<std::string> source_urls;
    std::string license_url;
    std::string revision;
    std::vector<std::string> download_hosts; // sorted, unique
    std::string verification;

    // format_download_mb of the size, or "Unknown" for a size of 0.
    std::string download_size() const;
    // The language support (Python adds "Distilled" for Distil-Whisper only).
    std::string compact_tags() const;
};

// "~145 MB" below 1000 MB, else "~1.6 GB" (decimal, one decimal place).
std::string format_download_mb(double size_mb);

// MODEL_SECURITY_NOTICE, naming identity().product where OpenWhisper names itself.
std::string model_security_notice();
// CUSTOM_MODEL_NOTICE, rebranded the same way. OpenWhisper_CPP has no custom models; the consent
// copy keeps the notice for parity with its callers.
std::string custom_model_notice();

// Every speech model's profile, in the order of models().
std::span<const ModelDetails> model_catalog();
// get_model_details: throws std::out_of_range (Python's KeyError) for a name not in the catalog.
const ModelDetails& get_model_details(std::string_view model_name);
// nullptr where get_model_details throws.
const ModelDetails* find_model_details(std::string_view model_name);

// MODEL_DOWNLOAD_SIZE_MB: the bundled estimate in decimal MB, or nullopt for an unknown model.
std::optional<std::int64_t> model_download_size_mb(std::string_view model_name);

} // namespace fv::models
