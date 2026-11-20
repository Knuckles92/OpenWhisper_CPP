#pragma once

#include <mutex>

#include "meeting_agent/agent_loop.h"
#include "meeting_agent/interfaces.h"

namespace fv {
class ProcessLauncher;
namespace net {
class Reactor;
}
} // namespace fv

namespace fv::meeting_agent {

struct CoreDeps {
    std::shared_ptr<http::Client> http;
    llm::KeySources keys;
    std::function<Json()> settings;
    net::Reactor* reactor = nullptr;
    std::shared_ptr<ProcessLauncher> launcher;
    const Clock* clock = nullptr;
};

class NativeAgentCore final : public AgentCore {
public:
    explicit NativeAgentCore(CoreDeps);
    ~NativeAgentCore() override;

    void initialize(const AgentConfig&, std::shared_ptr<AgentToolHost>) override;
    AgentResult checkpoint(const CheckpointPayload&) override;
    AgentResult consolidate(const CheckpointPayload&) override;
    void cancel() override;
    bool is_healthy() const override;
    void shutdown() override;
    void set_progress_callback(std::function<void(const std::string&)>) override;
    void set_activity_callback(std::function<void(const AgentActivity&)>) override;

private:
    struct Session {
        AgentLoop loop;
        std::shared_ptr<AgentToolHost> tools;
        std::string charter;
    };

    AgentResult run(const CheckpointPayload&, PassBudget);
    AgentResult run_attempt(Session&, const CheckpointPayload&, PassBudget, const CancelToken&);

    CoreDeps deps_;
    const Clock& clock_;
    mutable std::mutex state_mutex_;
    std::shared_ptr<Session> session_;
    CancelSource generation_;
    std::function<void(const std::string&)> progress_;
    std::function<void(const AgentActivity&)> activity_;
    std::mutex pass_mutex_;
    // Cancellation waits for any host operation already admitted. Reentrant cancellation from
    // within the host (for example, on a state-changed callback) revokes subsequent operations.
    std::recursive_mutex authority_;
};

} // namespace fv::meeting_agent
