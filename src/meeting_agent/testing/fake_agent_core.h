#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "meeting_agent/interfaces.h"

namespace fv::meeting_agent::testing {

// An AgentCore for scheduler and engine tests: scripted results per pass kind, an optional
// delay that cancel() cuts short, every payload recorded with its pass kind, and switches for
// health, initialize failures and a cadence of its own.
// A pass answers, in order: "canceled" when cancel() came during its delay; the pass hook when
// one is set (it may apply ops through the tools it was given); the next scripted result for
// its pass kind; else ok with no ops.
// Affinity: thread-safe; passes block their caller for the delay.
class FakeAgentCore final : public AgentCore {
public:
    struct Pass {
        std::string kind; // tool_policy.h's PASS_*
        CheckpointPayload payload;
        bool via_consolidate = false;
    };
    using Hook = std::function<AgentResult(const CheckpointPayload&, AgentToolHost*)>;

    void initialize(const AgentConfig& config, std::shared_ptr<AgentToolHost> tools) override;
    AgentResult checkpoint(const CheckpointPayload& payload) override;
    AgentResult consolidate(const CheckpointPayload& payload) override;
    void cancel() override;
    bool is_healthy() const override;
    void shutdown() override;
    std::optional<CheckpointCadence> cadence() const override;
    void set_progress_callback(std::function<void(const std::string&)> callback) override;
    void set_activity_callback(std::function<void(const AgentActivity&)> callback) override;

    // Scripted results for passes of `pass_kind`, used in order.
    void script(std::string_view pass_kind, AgentResult result);
    // Runs for every pass in place of the scripted results.
    void set_hook(Hook hook);
    // How long each pass waits before it answers (cancel() ends the wait).
    void set_delay(std::chrono::milliseconds delay);
    void set_healthy(bool healthy);
    void set_cadence(std::optional<CheckpointCadence> cadence);
    // initialize() throws std::runtime_error(message) while one is set.
    void fail_initialize(std::optional<std::string> message);

    // Calls the registered callbacks, as a core reporting progress would.
    void emit_progress(const std::string& text);
    void emit_activity(const AgentActivity& activity);

    std::vector<Pass> passes() const;
    std::optional<AgentConfig> config() const;
    std::shared_ptr<AgentToolHost> tools() const;
    int initialize_calls() const;
    int cancel_calls() const;
    int shutdown_calls() const;
    // Blocks until `count` passes have started or `timeout` passed; true when they had.
    bool wait_for_passes(std::size_t count, std::chrono::milliseconds timeout) const;

private:
    AgentResult run(const CheckpointPayload& payload, bool via_consolidate);

    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    std::optional<AgentConfig> config_;
    std::shared_ptr<AgentToolHost> tools_;
    std::map<std::string, std::deque<AgentResult>, std::less<>> scripted_;
    Hook hook_;
    std::chrono::milliseconds delay_{0};
    bool healthy_ = true;
    std::optional<CheckpointCadence> cadence_;
    std::optional<std::string> initialize_failure_;
    std::function<void(const std::string&)> progress_;
    std::function<void(const AgentActivity&)> activity_;
    std::vector<Pass> passes_;
    std::uint64_t cancel_generation_ = 0;
    int initialize_calls_ = 0;
    int cancel_calls_ = 0;
    int shutdown_calls_ = 0;
};

} // namespace fv::meeting_agent::testing
