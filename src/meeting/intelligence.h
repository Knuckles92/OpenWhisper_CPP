#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/json.h"
#include "meeting/rerun_options.h"
#include "meeting_agent/route.h"

// The seam the meeting AI plugs into: agent cores, transcript polish, consolidation, re-run
// insights, final highlights and insight review. Meetings without it are transcript-only: no AI
// panel, cloud_enabled forced false, and finalization reads "AI insights are off for this
// meeting." The null provider below is what a build without meeting intelligence installs.
namespace fv::meeting_agent {
class AgentCore;
}

namespace fv::meeting_state {
class MeetingRepository;
class MeetingStateStore;
} // namespace fv::meeting_state

namespace fv::meeting {

// The sentence every step of the null provider fails with.
inline constexpr std::string_view kIntelligenceUnavailableMessage =
    "Meeting intelligence is not available yet.";

// What a polish or consolidation pass over a stored meeting works from.
struct RerunContext {
    std::string meeting_id;
    // The meeting state snapshot the pass starts from.
    Json state;
    // The stored transcript, in timeline order.
    std::vector<Json> segments;
    RerunOptions options;
    // (detail, current, total): the polish block or step being worked on.
    std::function<void(const std::string& detail, int current, int total)> progress;
    // The stored row chooses the recorded endpoint. The same store used by the caller owns
    // validated writes throughout this synchronous pass.
    Json meeting;
    std::optional<meeting_agent::MeetingAgentRoute> approved_route;
    // Saved-meeting retries carry a one-use permit in their request, never a live grant.
    std::function<bool()> saved_destination_allowed;
    std::shared_ptr<meeting_state::MeetingStateStore> store;
    std::shared_ptr<meeting_state::MeetingRepository> repository;
    // The live engine may independently revoke writes during shutdown.
    std::function<bool()> writes_allowed;
};

// Affinity: thread-safe; passes block and run on the caller's worker thread.
class IntelligenceProvider {
public:
    virtual ~IntelligenceProvider() = default;

    // Whether meeting intelligence exists in this build. False hides the AI panel and forces a
    // meeting's cloud_enabled off.
    virtual bool available() const = 0;
    virtual std::optional<meeting_agent::MeetingAgentRoute> live_destination() const { return {}; }
    virtual bool authorize_live_destination(const meeting_agent::MeetingAgentRoute&) const {
        return false;
    }

    virtual std::shared_ptr<meeting_agent::AgentCore> create_live_core(const RerunContext&,
                                                                       const CancelToken&) {
        return {};
    }

    // Transcript polish and consolidation over a stored meeting. Each returns the step's outcome
    // {"status": "completed" | "unavailable" | "failed", "message": "..."}.
    virtual Json polish(const RerunContext& context, const CancelToken& cancel) = 0;
    virtual Json consolidate(const RerunContext& context, const CancelToken& cancel) = 0;
};

// The provider of a build without meeting intelligence: unavailable, and every step fails with
// kIntelligenceUnavailableMessage. One shared instance.
std::shared_ptr<IntelligenceProvider> null_intelligence();

} // namespace fv::meeting
