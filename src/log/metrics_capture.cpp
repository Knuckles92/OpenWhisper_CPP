#include "log/metrics_capture.h"

#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "base/metrics.h"
#include "log/detail/files.h"
#include "pyport/json.h"

namespace fv::log {

namespace {

constexpr double kMaxSample = 1e12;

// Recorded from Python ints in OpenWhisper (services/recorder.py), so their samples are JSON
// integers there.
constexpr std::array<std::string_view, 5> kCounterNames{"captured_frames", "dropped_frames",
                                                        "device_switches", "input_overflows",
                                                        "context_capture_timeouts"};

template <std::size_t N>
bool contains(const std::array<std::string_view, N>& names, std::string_view name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

// type(sample) in (int, float) and math.isfinite(sample) and 0 <= sample <= 1e12.
bool valid_sample(const Json& sample) {
    if (sample.is_number_unsigned()) {
        return sample.get<std::uint64_t>() <= 1'000'000'000'000ULL;
    }
    if (sample.is_number_integer()) {
        const auto value = sample.get<std::int64_t>();
        return value >= 0 && value <= 1'000'000'000'000LL;
    }
    if (sample.is_number_float()) {
        const double value = sample.get<double>();
        return std::isfinite(value) && value >= 0 && value <= kMaxSample;
    }
    return false;
}

double as_double(const Json& sample) {
    return sample.get<double>();
}

} // namespace

Json metric_samples(const Json& value) {
    Json result = Json::object();
    if (!value.is_object()) {
        return result;
    }
    for (const auto& [key, samples] : value.items()) {
        if (!contains(kMetricNames, key) || !samples.is_array()) {
            continue;
        }
        Json kept = Json::array();
        const std::size_t start =
            samples.size() > kMaxMetricSamples ? samples.size() - kMaxMetricSamples : 0;
        for (std::size_t i = start; i < samples.size(); ++i) {
            if (valid_sample(samples[i])) {
                kept.push_back(samples[i]);
            }
        }
        result[key] = std::move(kept);
    }
    return result;
}

Json metric_summary(const Json& value) {
    Json result = Json::object();
    const auto metrics = metric_samples(value);
    for (const auto& [key, samples] : metrics.items()) {
        if (samples.empty()) {
            continue;
        }
        std::vector<Json> ordered(samples.begin(), samples.end());
        std::stable_sort(ordered.begin(), ordered.end(),
                         [](const Json& a, const Json& b) { return as_double(a) < as_double(b); });
        const auto percentile = [&](double fraction) {
            const double position = static_cast<double>(ordered.size() - 1) * fraction;
            const auto lower = static_cast<std::size_t>(std::floor(position));
            const auto upper = static_cast<std::size_t>(std::ceil(position));
            const double low = as_double(ordered[lower]);
            return low +
                   (as_double(ordered[upper]) - low) * (position - static_cast<double>(lower));
        };
        Json summary = Json::object();
        summary["count"] = static_cast<std::int64_t>(samples.size());
        summary["p50"] = percentile(.5);
        summary["p95"] = percentile(.95);
        summary["maximum"] = ordered.back();
        summary["latest"] = samples.back();
        result[key] = std::move(summary);
    }
    return result;
}

struct MetricsCapture::State {
    std::filesystem::path path;
    SnapshotWriter write;
    mutable std::mutex mutex;
    std::condition_variable written;
    Json samples = Json::object();
    std::uint64_t generation = 0;
    std::uint64_t completed_generation = 0;
    std::uint64_t saved_generation = 0;
    bool closing = false;

    // MetricsCapture.record for one value; true when the writer should be woken.
    bool record(std::string_view name, double value) {
        if (!contains(kMetricNames, name) || !std::isfinite(value) || value < 0 ||
            value > kMaxSample) {
            return false;
        }
        Json sample = value;
        if (contains(kCounterNames, name) && value == std::floor(value)) {
            sample = static_cast<std::int64_t>(value);
        }
        std::lock_guard lock(mutex);
        if (closing) {
            return false;
        }
        Json& list = samples[std::string(name)];
        if (!list.is_array()) {
            list = Json::array();
        }
        list.push_back(std::move(sample));
        while (list.size() > kMaxMetricSamples) {
            list.erase(list.begin());
        }
        ++generation;
        return true;
    }

    // One pass of MetricsCapture._run: snapshot under the lock, write outside it.
    void run() {
        std::uint64_t target = 0;
        Json snapshot;
        {
            std::lock_guard lock(mutex);
            if (completed_generation >= generation) {
                return;
            }
            target = generation;
            snapshot = samples;
        }
        bool saved = false;
        try {
            write(path, snapshot);
            saved = true;
        } catch (...) {
            // Best effort only; logging here would recurse during shutdown.
        }
        {
            std::lock_guard lock(mutex);
            completed_generation = target;
            if (saved) {
                saved_generation = target;
            }
        }
        written.notify_all();
    }
};

MetricsCapture::MetricsCapture(std::filesystem::path path, LogWriter& writer, SnapshotWriter write)
    : state_(std::make_shared<State>()),
      job_(writer.coalesced([state = state_] { state->run(); })) {
    state_->path = std::move(path);
    state_->write = write ? std::move(write)
                          : SnapshotWriter([](const std::filesystem::path& file, const Json& data) {
                                detail::write_text_replace(file, py::dumps(data));
                            });
    state_->samples = metric_samples(detail::read_json_file(state_->path));
}

MetricsCapture::~MetricsCapture() {
    uninstall();
}

void MetricsCapture::record(std::string_view name, double value) {
    if (state_->record(name, value)) {
        job_.wake();
    }
}

bool MetricsCapture::flush(std::chrono::milliseconds timeout) {
    std::unique_lock lock(state_->mutex);
    const std::uint64_t target = state_->generation;
    state_->written.wait_for(lock, std::max(timeout, std::chrono::milliseconds(0)),
                             [&] { return state_->completed_generation >= target; });
    return state_->saved_generation >= target;
}

bool MetricsCapture::close(std::chrono::milliseconds timeout) {
    {
        std::lock_guard lock(state_->mutex);
        state_->closing = true;
    }
    job_.wake();
    return flush(timeout);
}

void MetricsCapture::install() {
    fv::metrics::set_sink([state = state_, job = job_](std::string_view name, double value) {
        if (state->record(name, value)) {
            job.wake();
        }
    });
    installed_ = true;
}

void MetricsCapture::uninstall() {
    if (installed_) {
        fv::metrics::set_sink({});
        installed_ = false;
    }
}

Json MetricsCapture::samples() const {
    std::lock_guard lock(state_->mutex);
    return state_->samples;
}

const std::filesystem::path& MetricsCapture::path() const {
    return state_->path;
}

} // namespace fv::log
