#include "models/model_details.h"

#include <algorithm>
#include <format>
#include <stdexcept>

#include "base/identity.h"
#include "base/json.h"
#include "models/speech_catalog.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::models {

namespace {

constexpr std::string_view kNemoSpeechSource = "https://github.com/NVIDIA/NeMo-Speech.cpp";
constexpr std::string_view kOrukeetLicense =
    "https://huggingface.co/oruk/orukeet/blob/main/NOTICE.md";

// _SPEECH_PARAMETER_COUNTS has no Parakeet entry: the default stands for both models until the
// Orukeet override.
constexpr std::string_view kDefaultParameterCount = "600 million";

// The runtime and device notes of the Parakeet backend (_SPEECH_RUNTIME_NOTES and the default of
// _SPEECH_DEVICE_NOTES).
constexpr std::string_view kParakeetRuntimeNote =
    "Requires its optional runtime from Downloads (Windows x64, Linux x86_64, or Apple Silicon "
    "Mac CPU).";
constexpr std::string_view kDefaultDeviceNote =
    "CPU and NVIDIA GPU use separately installed runtimes.";

// urlparse(url).hostname: the host between "//" and the path, without credentials or port,
// lower-cased.
std::string url_hostname(std::string_view url) {
    const auto scheme = url.find("://");
    std::string_view rest = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    rest = rest.substr(0, rest.find_first_of("/?#"));
    if (const auto at = rest.rfind('@'); at != std::string_view::npos) {
        rest = rest.substr(at + 1);
    }
    if (const auto colon = rest.find(':'); colon != std::string_view::npos) {
        rest = rest.substr(0, colon);
    }
    return py::lower(rest);
}

ModelDetails speech_model_details(const SpeechModel& model) {
    const Json& spec = artifacts(model.key);
    const std::string repo = spec.at("repo").get<std::string>();
    const std::string source = "https://huggingface.co/" + repo;
    std::int64_t total_bytes = 0;
    std::vector<std::string> hosts;
    for (const Json& file : spec.at("files")) {
        total_bytes += file.at("size_bytes").get<std::int64_t>();
        hosts.push_back(url_hostname(file.at("url").get<std::string>()));
    }
    std::ranges::sort(hosts);
    hosts.erase(std::unique(hosts.begin(), hosts.end()), hosts.end());

    ModelDetails details;
    details.model_name = std::string(model.key);
    details.description = std::string(model.purpose);
    details.origin_name = std::string(model.label);
    details.origin_url = source;
    details.repository_id = repo;
    details.repository_url = source;
    details.maintainer = repo.substr(0, repo.find('/'));
    details.family = std::string(model.backend);
    details.language_support = std::string(model.languages);
    details.task_support = "Speech transcription";
    details.parameter_count = std::string(kDefaultParameterCount);
    details.relative_performance =
        "Measure on your hardware; published throughput is not dictation latency.";
    details.memory_guidance = "CPU RAM / GPU memory use depends on model and audio length.";
    details.download_size_mb = static_cast<std::int64_t>(
        py::round_half_even(static_cast<double>(total_bytes) / 1'000'000.0));
    details.runtime_format = "GGUF Q8";
    details.license = std::string(model.license);
    details.best_for = std::string(model.purpose);
    details.limitations = {std::string(kParakeetRuntimeNote), std::string(kDefaultDeviceNote)};
    details.license_url =
        model.key == "orukeet-v0.1" ? std::string(kOrukeetLicense) : source + "#license";
    details.revision = spec.at("revision").get<std::string>();
    details.download_hosts = std::move(hosts);
    details.verification = "Pinned version; size and SHA-256 checked during download";
    details.source_urls = {source, std::string(kNemoSpeechSource)};

    if (model.key == "orukeet-v0.1") {
        details.maintainer = "Oruk AI (adaptation); NVIDIA (base model)";
        details.parameter_count = "627 million";
        details.origin_name = "Orukeet, based on NVIDIA Parakeet TDT v3";
        details.relative_performance =
            "Publisher reports improvements over Parakeet; results depend on datasets and "
            "decoding.";
        details.limitations.insert(
            details.limitations.end(),
            {"Community model; independently evaluate quality on your recordings.",
             "Validated on Windows/Linux CPU and CUDA; Orukeet on Apple Silicon has not been "
             "tested.",
             "One reported benchmark split was also used for training and checkpoint selection.",
             "CC BY-SA 4.0 weights retain attribution and applicable ShareAlike terms."});
    }
    return details;
}

const std::vector<ModelDetails>& catalog() {
    static const std::vector<ModelDetails> built = [] {
        std::vector<ModelDetails> details;
        for (const SpeechModel& model : models()) {
            details.push_back(speech_model_details(model));
        }
        return details;
    }();
    return built;
}

} // namespace

std::string ModelDetails::download_size() const {
    if (download_size_mb == 0) {
        return "Unknown";
    }
    return format_download_mb(static_cast<double>(download_size_mb));
}

std::string ModelDetails::compact_tags() const {
    return language_support;
}

std::string format_download_mb(double size_mb) {
    if (size_mb >= 1000) {
        return "~" + py::format_fixed(size_mb / 1000, 1) + " GB";
    }
    return "~" + py::format_fixed(size_mb, 0) + " MB";
}

std::string model_security_notice() {
    return std::format("Models are provided by third-party publishers. Model files and speech "
                       "runtimes can have security vulnerabilities. Keep {} and its runtimes "
                       "updated, and review important transcripts for errors.",
                       identity().product);
}

std::string custom_model_notice() {
    return std::format(
        "This source has not been reviewed by {}. Only load models from publishers you trust.",
        identity().product);
}

std::span<const ModelDetails> model_catalog() {
    return catalog();
}

const ModelDetails* find_model_details(std::string_view model_name) {
    for (const ModelDetails& details : catalog()) {
        if (details.model_name == model_name) {
            return &details;
        }
    }
    return nullptr;
}

const ModelDetails& get_model_details(std::string_view model_name) {
    if (const ModelDetails* details = find_model_details(model_name)) {
        return *details;
    }
    throw std::out_of_range(std::string(model_name));
}

std::optional<std::int64_t> model_download_size_mb(std::string_view model_name) {
    if (const ModelDetails* details = find_model_details(model_name)) {
        return details->download_size_mb;
    }
    return std::nullopt;
}

} // namespace fv::models
