#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>

#include "base/json.h"
#include "log/writer.h"

namespace fv::log {

// diagnostics.METRICS_NAME, MAX_METRIC_SAMPLES and METRIC_NAMES.
inline constexpr std::string_view kMetricsName = "diagnostic-metrics.json";
inline constexpr std::size_t kMaxMetricSamples = 100;
inline constexpr std::array<std::string_view, 12> kMetricNames{
    "startup_to_window_s", "stop_to_result_s",         "cancel_s",
    "shutdown_s",          "captured_frames",          "dropped_frames",
    "device_switches",     "input_overflows",          "peak_rss_mb",
    "context_capture_ms",  "context_capture_timeouts", "mouse_hook_callback_ms",
};

// diagnostics._metric_samples: {name: [samples]} for allowlisted names whose value is a list,
// each list cut to its last 100 entries and then to the int or float (never bool) samples that
// are finite and within 0..1e12. Anything but an object gives {}.
Json metric_samples(const Json& value);

// diagnostics._metric_summary: per metric with samples, {count, p50, p95, maximum, latest};
// percentiles interpolate linearly over the sorted samples, latest is the last one recorded.
Json metric_summary(const Json& value);

// diagnostics.MetricsCapture: bounded numeric samples, written to diagnostic-metrics.json
// (json.dumps with default separators, through the ".tmp" sibling) by fv-log. record() only
// updates the in-memory samples and wakes the writer: there is no queue of writes to grow when
// storage stalls, and nothing touches the disk under the lock. Missing metrics stay missing.
// Affinity: thread-safe.
class MetricsCapture {
public:
    // Writes one snapshot to the file, throwing on failure; replaceable for tests.
    using SnapshotWriter = std::function<void(const std::filesystem::path&, const Json&)>;

    // Loads and revalidates the samples the file already holds.
    MetricsCapture(std::filesystem::path path, LogWriter& writer, SnapshotWriter write = {});
    // Uninstalls the facade sink if this capture installed it.
    ~MetricsCapture();
    MetricsCapture(const MetricsCapture&) = delete;
    MetricsCapture& operator=(const MetricsCapture&) = delete;

    // One value of an allowlisted metric; others, and values that are not finite or are outside
    // 0..1e12, are ignored, as is everything after close(). The counters (captured_frames,
    // dropped_frames, device_switches, input_overflows, context_capture_timeouts) are stored as
    // integers when the value is whole, as OpenWhisper records them from Python ints.
    void record(std::string_view name, double value);

    // Waits at most `timeout` for the samples recorded so far to be written; true when the last
    // write that covers them succeeded. Never retries I/O.
    bool flush(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));
    // Stops accepting samples, wakes the writer and flushes.
    bool close(std::chrono::milliseconds timeout = std::chrono::milliseconds(100));

    // Routes fv::metrics::record here (diagnostics._metrics = this capture).
    void install();
    void uninstall();

    Json samples() const;
    const std::filesystem::path& path() const;

    struct State;

private:
    std::shared_ptr<State> state_;
    CoalescedJob job_;
    bool installed_ = false;
};

} // namespace fv::log
