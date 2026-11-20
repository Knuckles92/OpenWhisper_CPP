#pragma once

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "meeting_state/types.h"

// meeting/interfaces.py's agent contracts (AgentConfig, CheckpointPayload, AgentResult,
// AgentToolHost, AgentCore) and meeting/agent/sidecar.py's AgentActivity: what the meeting
// engine, the checkpoint scheduler and every agent core (the built-in loop and the installed
// coding agents) exchange.
namespace fv::meeting_agent {

using Json = fv::Json;

// Handed to a core at meeting start. api_key is empty when no key is needed or none was found;
// endpoint is the meeting's text-endpoint snapshot, or null.
struct AgentConfig {
    std::string meeting_id;
    std::string provider;
    std::string model;
    std::optional<std::string> api_key;
    std::string system_prompt;
    Json extra = Json::object();
    Json endpoint;
};

// One rolling checkpoint, polish, notes or consolidation pass. state_snapshot is a
// MeetingState::to_json() document (with the scheduler's extra keys such as
// recent_transcript_context); new_segments are repository segment rows, for consolidation the
// whole final transcript.
struct CheckpointPayload {
    std::string request_id;
    Json state_snapshot = Json::object();
    std::vector<Json> new_segments;
    bool is_consolidation = false;
    bool is_polish = false;
    bool is_notes = false;
};

// A pass's outcome. error is empty where Python's is None; usage is merge_usage's total.
struct AgentResult {
    bool ok = false;
    std::vector<meeting_state::OpResult> op_results;
    std::string error;
    Json usage = Json::object();
};

// The activity strip's categories; "update" is the catch-all for an unclassified event.
inline constexpr std::array<std::string_view, 9> ACTIVITY_KINDS = {
    "thinking", "writing", "tool", "turn", "retry", "compaction", "start", "settled", "update"};
bool is_activity_kind(std::string_view kind);

// One ephemeral tick for the host-only activity strip.
struct AgentActivity {
    std::string kind;      // one of ACTIVITY_KINDS
    std::string label;     // the sentence the strip shows
    std::string tool;      // the tool's name for tool activity, else ""
    std::string pass_kind; // the pass in flight (tool_policy.h's PASS_*), else ""
    std::string ts;        // ISO 8601 UTC, utc_now_iso()

    // {"kind", "label", "tool", "pass_kind", "ts"}, without a message type.
    Json to_json() const;
};

// datetime.now(timezone.utc).isoformat(): "2026-11-17T09:30:00.123456+00:00", the fraction
// left out when it is zero, as Python does.
std::string utc_now_iso(const Clock& clock = system_clock());

// The pass cadence a core asks the scheduler for (seconds; the counts are successful card
// checkpoints). The installed coding agents ask for a slower one than the built-in loop.
struct CheckpointCadence {
    double base_s = 0.0;
    double min_s = 0.0;
    double max_s = 0.0;
    int polish_every = 0;
    double polish_min_s = 0.0;
    int notes_every = 0;
    double notes_min_s = 0.0;

    friend bool operator==(const CheckpointCadence&, const CheckpointCadence&) = default;
};

// What an agent core may do to the meeting: every write goes through the state-patch
// validation of apply_agent_ops, whatever the agent asked for. Implemented by the meeting
// engine and by M8.5's StoreToolHost.
// A Python host could lack segment_exists or a search tool, and the tool policy then skips
// evidence repair or answers "not available"; the can_* members say whether this host has them.
// Affinity: thread-safe.
class AgentToolHost {
public:
    virtual ~AgentToolHost() = default;

    // Validates and applies agent ops (a JSON list), one result per op.
    virtual std::vector<meeting_state::OpResult> apply_agent_ops(const Json& ops) = 0;
    // Exact-match check of a stored transcript segment id.
    virtual bool segment_exists(std::string_view segment_id) = 0;
    virtual meeting_state::OpResult ask_question(std::string_view text, const Json& evidence) = 0;
    virtual meeting_state::OpResult resolve_question(std::string_view question_id,
                                                     std::string_view answer, double confidence,
                                                     const Json& evidence) = 0;
    // The consent-gated read tools. An empty meeting_id or relative_path is Python's None;
    // limit is the integer the tool policy read from the call (10 by default).
    virtual Json search_past_meetings(std::string_view query, std::string_view meeting_id,
                                      const Json& limit) = 0;
    virtual Json search_context_files(std::string_view query, std::string_view relative_path,
                                      const Json& limit) = 0;

    virtual bool can_check_segments() const { return true; }
    virtual bool can_search_past_meetings() const { return true; }
    virtual bool can_search_context_files() const { return true; }
};

// An agent that turns meeting state and new transcript into ops. Passes block their caller
// (the scheduler's worker or the end-of-meeting thread); cancel() comes from another thread.
// Affinity: thread-safe; passes block their caller.
class AgentCore {
public:
    virtual ~AgentCore() = default;

    // Prepares the core for one meeting. Throws an exception whose what() is a full sentence
    // the user can read when the core cannot run.
    virtual void initialize(const AgentConfig& config, std::shared_ptr<AgentToolHost> tools) = 0;
    // One rolling pass (cards, notes or polish, by the payload's flags).
    virtual AgentResult checkpoint(const CheckpointPayload& payload) = 0;
    // The end-of-meeting pass.
    virtual AgentResult consolidate(const CheckpointPayload& payload) = 0;
    // Cancels every pass in flight.
    virtual void cancel() = 0;
    virtual bool is_healthy() const = 0;
    virtual void shutdown() = 0;

    // A cadence of its own (Python's optional `cadence` attribute), or the scheduler's.
    virtual std::optional<CheckpointCadence> cadence() const { return std::nullopt; }
    // Progress sentences for the finalization card, and activity ticks for the strip. Either
    // may be called on any thread.
    virtual void set_progress_callback(std::function<void(const std::string&)>) {}
    virtual void set_activity_callback(std::function<void(const AgentActivity&)>) {}
};

} // namespace fv::meeting_agent
