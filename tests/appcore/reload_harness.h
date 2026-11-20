#pragma once

#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "appcore/engine_reload.h"
#include "base/json.h"
#include "base/testing/manual_executor.h"

namespace fv::appcore::testing {

// The UI executor of a test: a ManualExecutor that also writes each timer it is asked for into
// the trace as ["timer", ms], as the golden reload traces record QTimer.start.
class TracingExecutor final : public Executor {
public:
    TracingExecutor(ManualExecutor& inner, Json& trace) : inner_(inner), trace_(trace) {}
    void post(Task task) override { inner_.post(std::move(task)); }
    TimerHandle post_after(std::chrono::milliseconds delay, Task task) override {
        trace_.push_back(Json::array({"timer", delay.count()}));
        return inner_.post_after(delay, std::move(task));
    }
    bool is_current() const override { return inner_.is_current(); }

private:
    ManualExecutor& inner_;
    Json& trace_;
};

// An EngineReloadMachine on a manual UI executor, with every command it emits written to `trace`
// as the golden vectors spell it: ["busy_changed", true], ["status", "..."], ["start_load"], ...
struct ReloadHarness {
    ManualExecutor executor;
    Json trace = Json::array();
    TracingExecutor ui{executor, trace};
    EngineReloadMachine machine{ui};
    ActivitySnapshot activity;
    std::vector<Subscription> links;

    ReloadHarness() {
        machine.select("parakeet", "parakeet-v3");
        record(machine.start_load, "start_load");
        record(machine.start_warmup, "start_warmup");
        record(machine.discard_load, "discard_load");
        record(machine.stop_preview, "stop_preview");
        record(machine.ensure_model_available, "ensure_model_available");
        record(machine.streaming_setup_due, "streaming_setup_due");
        links.push_back(machine.busy_changed.connect(
            ui, [this](bool busy) { trace.push_back(Json::array({"busy_changed", busy})); }));
        links.push_back(machine.status.connect(ui, [this](const std::string& text) {
            trace.push_back(Json::array({"status", text}));
        }));
        links.push_back(machine.device_info.connect(
            ui, [this](const std::string& info, std::optional<bool> ready) {
                trace.push_back(
                    Json::array({"device_info", info, ready ? Json(*ready) : Json(nullptr)}));
            }));
        links.push_back(machine.missing_runtime.connect(ui, [this](const std::string& model) {
            trace.push_back(Json::array({"missing_runtime", model}));
        }));
    }

    // Runs fn as a task of the UI executor, where a UI actor's methods run.
    template <class Fn> void on_ui(Fn&& fn) {
        executor.post(std::forward<Fn>(fn));
        executor.run_all();
    }

    void set_activity(const ActivitySnapshot& next) {
        activity = next;
        on_ui([this] { machine.on_activity(activity); });
    }
    void fire_timer() { executor.advance(kReloadDebounce); }
    void load_finished(speech::ReadyState state, std::string device_info = {},
                       std::string message = {}) {
        on_ui([&] {
            machine.on_load_finished(
                speech::Readiness{state, std::move(device_info), std::move(message)});
        });
    }
    void load_ready() { load_finished(speech::ReadyState::Ready, "Parakeet TDT 0.6B v3 | cuda"); }

    // Index of the first event named `name` at or after `from`, or npos.
    std::size_t index_of(std::string_view name, std::size_t from = 0) const {
        for (std::size_t i = from; i < trace.size(); ++i) {
            if (trace[i][0].get<std::string>() == name) {
                return i;
            }
        }
        return npos;
    }
    bool contains(const Json& event) const {
        for (const auto& entry : trace) {
            if (entry == event) {
                return true;
            }
        }
        return false;
    }
    std::size_t count(std::string_view name) const {
        std::size_t total = 0;
        for (const auto& entry : trace) {
            total += entry[0].get<std::string>() == name ? 1 : 0;
        }
        return total;
    }

    static constexpr std::size_t npos = static_cast<std::size_t>(-1);

private:
    void record(Signal<>& signal, std::string name) {
        links.push_back(signal.connect(ui, [this, name] { trace.push_back(Json::array({name})); }));
    }
};

} // namespace fv::appcore::testing
