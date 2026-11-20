#include "appcore/shutdown_plan.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#include "base/log.h"
#include "base/metrics.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"services.application_controller"};

void attempt(const std::function<void()>& fn, const std::string& failure) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.warning("{}: {}", failure, error.what());
    } catch (...) {
        kLog.warning("{}: unknown exception", failure);
    }
}

} // namespace

ShutdownPlan::ShutdownPlan(Clock& clock) : clock_(clock) {}

void ShutdownPlan::add(ShutdownStep step, std::string failure_label, std::function<void()> fn) {
    const auto at =
        std::upper_bound(entries_.begin(), entries_.end(), step,
                         [](ShutdownStep value, const Entry& entry) { return value < entry.step; });
    entries_.insert(at, Entry{step, std::move(failure_label), std::move(fn)});
}

void ShutdownPlan::run() {
    if (started_) {
        return;
    }
    started_ = true;
    kLog.info("Starting application cleanup...");
    const auto started = clock_.steady();
    // A copy: a step that adds another must not disturb the walk.
    const auto entries = entries_;
    for (const auto& entry : entries) {
        if (entry.failure_label.empty()) {
            entry.fn();
        } else {
            attempt(entry.fn, entry.failure_label);
        }
    }
    kLog.info("Application controller cleaned up");
    metrics::record("shutdown_s", std::chrono::duration<double>(clock_.steady() - started).count());
}

bool ShutdownPlan::started() const {
    return started_;
}

std::size_t ShutdownPlan::size() const {
    return entries_.size();
}

} // namespace fv
