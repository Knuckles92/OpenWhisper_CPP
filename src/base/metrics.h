#pragma once

#include <functional>
#include <string_view>

// The call shape of Python's diagnostics.record_metrics(**values), one value per call. fv_log's
// MetricsCapture installs the sink and owns the allowlist and the limits; until then, and in
// processes that never install it, record is a no-op ("missing metrics stay missing, not zero").
namespace fv::metrics {

// Never call from an audio or hook callback. The sink runs synchronously on the calling thread,
// outside any lock, and must not block.
// Affinity: thread-safe.
void record(std::string_view name, double value);

// An empty function uninstalls the sink.
void set_sink(std::function<void(std::string_view, double)> sink);

} // namespace fv::metrics
