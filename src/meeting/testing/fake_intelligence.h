#pragma once

#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "meeting/intelligence.h"
#include "meeting_agent/testing/fake_agent_core.h"

namespace fv::meeting {

// An IntelligenceProvider for tests: available as told, each pass records its context and
// returns the next scripted outcome (completed "" once the script is empty). A scripted outcome
// {"throw": "<what>"} throws std::runtime_error instead, {"value_error": "<what>"}
// py::ValueError.
// A meeting with AI insights on comes online through a healthy FakeAgentCore made for each live
// start, as Python's engine fixture gives every cloud meeting a core; End runs its passes only
// while that live agent is online. set_live(false) makes the live start fail instead.
// Affinity: thread-safe.
class FakeIntelligence final : public IntelligenceProvider {
public:
    explicit FakeIntelligence(bool available = true) : available_(available) {}

    void set_available(bool available);
    void set_live(bool live);
    void script_polish(Json outcome);
    void script_consolidate(Json outcome);

    // The meeting ids the passes ran for, in order.
    std::vector<std::string> polished() const;
    std::vector<std::string> consolidated() const;
    // The context of the last pass of either kind.
    RerunContext last_context() const;
    // The live cores handed out, oldest first.
    std::vector<std::shared_ptr<meeting_agent::testing::FakeAgentCore>> live_cores() const;

    bool available() const override;
    std::shared_ptr<meeting_agent::AgentCore> create_live_core(const RerunContext& context,
                                                               const CancelToken& cancel) override;
    Json polish(const RerunContext& context, const CancelToken& cancel) override;
    Json consolidate(const RerunContext& context, const CancelToken& cancel) override;

private:
    Json next(std::deque<Json>& script);

    mutable std::mutex mutex_;
    bool available_;
    bool live_ = true;
    std::vector<std::shared_ptr<meeting_agent::testing::FakeAgentCore>> live_cores_;
    std::deque<Json> polish_script_;
    std::deque<Json> consolidate_script_;
    std::vector<std::string> polished_;
    std::vector<std::string> consolidated_;
    RerunContext last_context_;
};

} // namespace fv::meeting
