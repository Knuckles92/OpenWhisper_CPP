#pragma once

#include "meeting/intelligence.h"
#include "meeting_agent/interfaces.h"
#include "meeting_agent/route.h"

namespace fv::meeting_intel {
struct RecallDeps;
}

namespace fv::meeting {

using AgentCoreFactory = std::function<std::unique_ptr<meeting_agent::AgentCore>(std::string_view)>;

// Stored-meeting polish and consolidation. The owner supplies the current destination grant;
// an absent or refused grant prevents the route from being initialized or receiving text.
class MeetingIntelligence final : public IntelligenceProvider {
public:
    using Authorize = std::function<bool(const meeting_agent::MeetingAgentRoute&)>;

    MeetingIntelligence(AgentCoreFactory, std::function<Json()> settings, Authorize,
                        meeting_intel::RecallDeps* recall = nullptr);
    bool available() const override;
    std::optional<meeting_agent::MeetingAgentRoute> live_destination() const override;
    bool authorize_live_destination(const meeting_agent::MeetingAgentRoute& route) const override;
    std::shared_ptr<meeting_agent::AgentCore> create_live_core(const RerunContext&,
                                                               const CancelToken&) override;
    Json polish(const RerunContext&, const CancelToken&) override;
    Json consolidate(const RerunContext&, const CancelToken&) override;

private:
    Json run(const RerunContext&, const CancelToken&, bool polish);

    AgentCoreFactory factory_;
    std::function<Json()> settings_;
    Authorize authorize_;
    meeting_intel::RecallDeps* recall_;
};

} // namespace fv::meeting
