#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/clock.h"
#include "base/json.h"

namespace fv::log {

// The report's fixed privacy sentence.
inline constexpr std::string_view kReportPrivacy =
    "No transcript, audio, log messages, paths, credentials, hostnames or pairing data included.";

struct GpuInfo {
    std::string name;
    std::int64_t total_mib = 0;
    // (major, minor), written as [major, minor]; nullopt is JSON null (NVML did not say).
    std::optional<std::pair<int, int>> compute_capability;
};

// What collect_report cannot find out by itself. The executables fill it.
struct ReportInputs {
    std::string app_version;
    bool packaged = false;
    const Json* settings = nullptr; // snapshot or nullptr
    std::filesystem::path data_root;
    // Reported runtime versions in this order ("qt", "nemo_speech_runtime", "sqlite",
    // "openssl"); nullopt is JSON null (not installed or not loaded).
    std::vector<std::pair<std::string, std::optional<std::string>>> components;
    std::function<std::optional<GpuInfo>()> nvidia_gpu; // empty until M1.11 provides NVML
    // Whether a local_asr_models value is a speech-model catalog id (public constants; custom
    // model paths are never exported). Empty: no id is, so local_models is [].
    std::function<bool(std::string_view)> catalog_model;
    const Clock* clock = nullptr; // generated_utc; nullptr is system_clock()
};

// diagnostics.collect_report, schema 1: allowlisted fields only. The engine block reports
// selected_model when it is "parakeet", "api" or "remote" and "default_or_custom" otherwise;
// whisper_model, whisper_device and whisper_compute_type are settings of a dropped engine and are
// always "default_or_custom"; local_models (present when local_asr_models is an object or
// missing) lists the sorted unique catalog ids among its values. recent_failures and
// recent_metrics are the revalidated journal and the metric summary from data_root.
Json collect_report(const ReportInputs&);

// diagnostics._read_json: JSON null for a file over 1 MiB, a missing or unreadable file, text
// that is not UTF-8, or text json.loads refuses. For callers that read the settings file
// themselves to fill ReportInputs::settings.
Json read_json_file(const std::filesystem::path& path);

// diagnostics.main's output: json.dump(report, indent=2) and a newline, written as Python's text
// mode writes it, to a file that must not exist yet (open(path, "x")). Throws
// std::filesystem::filesystem_error (errc::file_exists when it exists) and then leaves an
// existing file unchanged.
void write_report_new(const std::filesystem::path& path, const Json& report);

} // namespace fv::log
