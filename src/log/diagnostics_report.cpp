#include "log/diagnostics_report.h"

#include <algorithm>
#include <set>

#include "base/platform_info.h"
#include "log/detail/files.h"
#include "log/failure_journal.h"
#include "log/metrics_capture.h"
#include "pyport/json.h"

namespace fv::log {

namespace {

constexpr std::string_view kDefaultOrCustom = "default_or_custom";

Json engine_block(const ReportInputs& inputs) {
    const Json empty = Json::object();
    const Json& settings =
        inputs.settings && inputs.settings->is_object() ? *inputs.settings : empty;

    Json engine = Json::object();
    const auto selected = settings.find("selected_model");
    if (selected != settings.end() && selected->is_string() &&
        (*selected == "parakeet" || *selected == "api" || *selected == "remote")) {
        engine["selected_model"] = *selected;
    } else {
        engine["selected_model"] = kDefaultOrCustom;
    }
    // Settings of the dropped Whisper engine: their values are never exported.
    for (const char* key : {"whisper_model", "whisper_device", "whisper_compute_type"}) {
        engine[key] = kDefaultOrCustom;
    }

    const auto models = settings.find("local_asr_models");
    if (models == settings.end() || models->is_object()) {
        std::set<std::string> ids;
        if (models != settings.end() && inputs.catalog_model) {
            for (const auto& [key, value] : models->items()) {
                if (value.is_string() &&
                    inputs.catalog_model(value.get_ref<const std::string&>())) {
                    ids.insert(value.get<std::string>());
                }
            }
        }
        Json list = Json::array();
        for (const auto& id : ids) {
            list.push_back(id);
        }
        engine["local_models"] = std::move(list);
    }
    return engine;
}

} // namespace

Json collect_report(const ReportInputs& inputs) {
    const Clock& clock = inputs.clock ? *inputs.clock : system_clock();

    Json gpu = nullptr;
    if (inputs.nvidia_gpu) {
        if (const auto info = inputs.nvidia_gpu()) {
            gpu = Json::object();
            gpu["name"] = info->name;
            gpu["total_mib"] = info->total_mib;
            if (const auto& capability = info->compute_capability) {
                gpu["compute_capability"] = Json::array({capability->first, capability->second});
            } else {
                gpu["compute_capability"] = nullptr;
            }
        }
    }

    Json platform = Json::object();
    platform["system"] = detail::os_system();
    platform["release"] = detail::os_release();
    platform["machine"] = host_machine();
    if (const auto count = detail::cpu_count()) {
        platform["cpu_count"] = *count;
    } else {
        platform["cpu_count"] = nullptr;
    }
    platform["nvidia_gpu"] = std::move(gpu);

    Json components = Json::object();
    for (const auto& [name, version] : inputs.components) {
        components[name] = version ? Json(*version) : Json(nullptr);
    }

    Json report = Json::object();
    report["schema"] = 1;
    report["app_version"] = inputs.app_version;
    report["packaged"] = inputs.packaged;
    report["generated_utc"] = detail::utc_isoformat(clock.utc());
    report["platform"] = std::move(platform);
    report["engine"] = engine_block(inputs);
    report["components"] = std::move(components);
    report["recent_failures"] =
        failure_events(detail::read_json_file(inputs.data_root / std::string(kJournalName)));
    report["recent_metrics"] =
        metric_summary(detail::read_json_file(inputs.data_root / std::string(kMetricsName)));
    report["privacy"] = kReportPrivacy;
    return report;
}

Json read_json_file(const std::filesystem::path& path) {
    return detail::read_json_file(path);
}

void write_report_new(const std::filesystem::path& path, const Json& report) {
    py::DumpOptions options;
    options.indent = 2;
    detail::write_new_file(path, detail::text_mode_bytes(py::dumps(report, options) + "\n"));
}

} // namespace fv::log
