#include "base/metrics.h"

#include <memory>
#include <mutex>
#include <utility>

namespace fv::metrics {

namespace {

using Sink = std::function<void(std::string_view, double)>;

struct State {
    std::mutex mutex;
    std::shared_ptr<const Sink> sink;
};

State& state() {
    static State instance;
    return instance;
}

} // namespace

void record(std::string_view name, double value) {
    std::shared_ptr<const Sink> sink;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        sink = s.sink;
    }
    if (sink) {
        (*sink)(name, value);
    }
}

void set_sink(Sink sink) {
    auto next = sink ? std::make_shared<const Sink>(std::move(sink)) : nullptr;
    std::shared_ptr<const Sink> previous;
    {
        auto& s = state();
        std::lock_guard lock(s.mutex);
        previous = std::exchange(s.sink, std::move(next));
    }
}

} // namespace fv::metrics
