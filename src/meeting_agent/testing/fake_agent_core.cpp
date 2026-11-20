#include "meeting_agent/testing/fake_agent_core.h"

#include <stdexcept>
#include <utility>

#include "meeting_agent/tool_policy.h"

namespace fv::meeting_agent::testing {

void FakeAgentCore::initialize(const AgentConfig& config, std::shared_ptr<AgentToolHost> tools) {
    std::lock_guard lock(mutex_);
    ++initialize_calls_;
    if (initialize_failure_) {
        throw std::runtime_error(*initialize_failure_);
    }
    config_ = config;
    tools_ = std::move(tools);
}

AgentResult FakeAgentCore::checkpoint(const CheckpointPayload& payload) {
    return run(payload, false);
}

AgentResult FakeAgentCore::consolidate(const CheckpointPayload& payload) {
    return run(payload, true);
}

AgentResult FakeAgentCore::run(const CheckpointPayload& payload, bool via_consolidate) {
    const std::string kind = pass_kind_for(payload);
    Hook hook;
    // Held for the hook's whole run: a shutdown or a second initialize on another thread may
    // replace tools_ while the hook still applies ops through it.
    std::shared_ptr<AgentToolHost> tools;
    {
        std::unique_lock lock(mutex_);
        passes_.push_back({kind, payload, via_consolidate});
        changed_.notify_all();
        const std::uint64_t generation = cancel_generation_;
        if (delay_.count() > 0) {
            changed_.wait_for(lock, delay_, [&] { return cancel_generation_ != generation; });
        }
        if (cancel_generation_ != generation) {
            AgentResult canceled;
            canceled.error = "canceled";
            return canceled;
        }
        hook = hook_;
        tools = tools_;
        if (!hook) {
            auto found = scripted_.find(kind);
            if (found != scripted_.end() && !found->second.empty()) {
                AgentResult result = std::move(found->second.front());
                found->second.pop_front();
                return result;
            }
            AgentResult ok;
            ok.ok = true;
            return ok;
        }
    }
    return hook(payload, tools.get());
}

void FakeAgentCore::cancel() {
    std::lock_guard lock(mutex_);
    ++cancel_calls_;
    ++cancel_generation_;
    changed_.notify_all();
}

bool FakeAgentCore::is_healthy() const {
    std::lock_guard lock(mutex_);
    return healthy_;
}

void FakeAgentCore::shutdown() {
    std::lock_guard lock(mutex_);
    ++shutdown_calls_;
}

std::optional<CheckpointCadence> FakeAgentCore::cadence() const {
    std::lock_guard lock(mutex_);
    return cadence_;
}

void FakeAgentCore::set_progress_callback(std::function<void(const std::string&)> callback) {
    std::lock_guard lock(mutex_);
    progress_ = std::move(callback);
}

void FakeAgentCore::set_activity_callback(std::function<void(const AgentActivity&)> callback) {
    std::lock_guard lock(mutex_);
    activity_ = std::move(callback);
}

void FakeAgentCore::script(std::string_view pass_kind, AgentResult result) {
    std::lock_guard lock(mutex_);
    auto found = scripted_.find(pass_kind);
    if (found == scripted_.end()) {
        found = scripted_.emplace(std::string(pass_kind), std::deque<AgentResult>{}).first;
    }
    found->second.push_back(std::move(result));
}

void FakeAgentCore::set_hook(Hook hook) {
    std::lock_guard lock(mutex_);
    hook_ = std::move(hook);
}

void FakeAgentCore::set_delay(std::chrono::milliseconds delay) {
    std::lock_guard lock(mutex_);
    delay_ = delay;
}

void FakeAgentCore::set_healthy(bool healthy) {
    std::lock_guard lock(mutex_);
    healthy_ = healthy;
}

void FakeAgentCore::set_cadence(std::optional<CheckpointCadence> cadence) {
    std::lock_guard lock(mutex_);
    cadence_ = cadence;
}

void FakeAgentCore::fail_initialize(std::optional<std::string> message) {
    std::lock_guard lock(mutex_);
    initialize_failure_ = std::move(message);
}

void FakeAgentCore::emit_progress(const std::string& text) {
    std::function<void(const std::string&)> callback;
    {
        std::lock_guard lock(mutex_);
        callback = progress_;
    }
    if (callback) {
        callback(text);
    }
}

void FakeAgentCore::emit_activity(const AgentActivity& activity) {
    std::function<void(const AgentActivity&)> callback;
    {
        std::lock_guard lock(mutex_);
        callback = activity_;
    }
    if (callback) {
        callback(activity);
    }
}

std::vector<FakeAgentCore::Pass> FakeAgentCore::passes() const {
    std::lock_guard lock(mutex_);
    return passes_;
}

std::optional<AgentConfig> FakeAgentCore::config() const {
    std::lock_guard lock(mutex_);
    return config_;
}

std::shared_ptr<AgentToolHost> FakeAgentCore::tools() const {
    std::lock_guard lock(mutex_);
    return tools_;
}

int FakeAgentCore::initialize_calls() const {
    std::lock_guard lock(mutex_);
    return initialize_calls_;
}

int FakeAgentCore::cancel_calls() const {
    std::lock_guard lock(mutex_);
    return cancel_calls_;
}

int FakeAgentCore::shutdown_calls() const {
    std::lock_guard lock(mutex_);
    return shutdown_calls_;
}

bool FakeAgentCore::wait_for_passes(std::size_t count, std::chrono::milliseconds timeout) const {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, timeout, [&] { return passes_.size() >= count; });
}

} // namespace fv::meeting_agent::testing
