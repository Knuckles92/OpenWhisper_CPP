#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "automation/control.h"

namespace fv::automation {

// An AppControl that answers from fields and records every call, for the live API and MCP
// tests. Set `refusal` to make every call but state() and list_actions() throw it.
// Affinity: thread-safe.
class FakeAppControl final : public AppControl {
public:
    struct Call {
        std::string method; // "run_action", "start_recording", "stop_recording", "cancel", "notify"
        std::vector<std::string> args;

        friend bool operator==(const Call&, const Call&) = default;
    };

    LiveState state() override {
        std::lock_guard lock(mutex_);
        return live_state;
    }
    Json list_actions() override {
        std::lock_guard lock(mutex_);
        return actions;
    }
    std::string run_action(std::string_view action_id, std::string_view text,
                           std::string_view source) override {
        return record("run_action",
                      {std::string(action_id), std::string(text), std::string(source)});
    }
    std::string start_recording(std::optional<std::string> action_id,
                                std::string_view source) override {
        return record("start_recording", {action_id.value_or(""), std::string(source)});
    }
    void stop_recording() override { record("stop_recording", {}); }
    void cancel() override { record("cancel", {}); }
    void notify(std::string_view text, std::string_view title) override {
        record("notify", {std::string(text), std::string(title)});
    }

    std::vector<Call> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }

    LiveState live_state;
    Json actions = Json::array();
    // The job id run_action and start_recording answer with.
    std::string job_id = "job1";
    std::optional<ControlError> refusal;

private:
    std::string record(std::string method, std::vector<std::string> args) {
        std::lock_guard lock(mutex_);
        calls_.push_back({std::move(method), std::move(args)});
        if (refusal) {
            throw *refusal;
        }
        return job_id;
    }

    mutable std::mutex mutex_;
    std::vector<Call> calls_;
};

} // namespace fv::automation
