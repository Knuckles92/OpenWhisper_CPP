#pragma once

#include <array>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"
#include "meeting_agent/interfaces.h"
#include "meeting_state/types.h"

// meeting/agent/tool_policy.py: the one tool-answering path of every agent core, so pass
// restrictions, evidence repair, read-tool payloads and op-result serialization cannot drift
// between cores. An op outside the pass's job comes back as a per-op rejection (polish_only,
// notes_only) rather than being dropped, so the model sees why and can correct itself;
// meeting_state/patches.h stays the authority for everything the policy lets through.
// Affinity: thread-safe; the host's own affinity applies to its calls.
namespace fv::meeting_agent {

using Json = fv::Json;

// Pass kinds, in the order a payload's flags are checked.
inline constexpr std::string_view PASS_CARDS = "cards";
inline constexpr std::string_view PASS_NOTES = "notes";
inline constexpr std::string_view PASS_POLISH = "polish";
inline constexpr std::string_view PASS_CONSOLIDATION = "consolidation";

// Rejection reasons the policy adds on top of op validation.
inline constexpr std::string_view POLISH_ONLY = "polish_only";
inline constexpr std::string_view NOTES_ONLY = "notes_only";

// Every meeting tool, and the read-only ones (allowed on every pass and never tallied).
inline constexpr std::array<std::string_view, 5> TOOL_NAMES = {
    "patch_state", "ask_question", "resolve_question", "search_past_meetings",
    "search_context_files"};
inline constexpr std::array<std::string_view, 2> READ_TOOLS = {"search_past_meetings",
                                                               "search_context_files"};
bool is_tool_name(std::string_view name);
bool is_read_tool(std::string_view name);

// The pass a payload runs as: consolidation, polish, notes, else cards.
std::string pass_kind_for(const CheckpointPayload& payload);
// The segment ids the pass's prompt shows (new segments, then recent_transcript_context): the
// only ids evidence repair may target.
std::vector<std::string> citable_segment_ids(const CheckpointPayload& payload);

// What one pass's tool calls may do. note_ids are the live-notes block ids a notes pass may
// update or remove.
struct ToolScope {
    std::string pass_kind = std::string(PASS_CARDS);
    std::set<std::string, std::less<>> note_ids;
    std::vector<std::string> citable_ids;

    static ToolScope for_payload(const CheckpointPayload& payload);
};

// The per-op outcome a model sees: {ok, reason, target_id, seq, current_revision}.
Json serialize_op_result(const meeting_state::OpResult& result);
// {"results": [...]}.
Json op_results_payload(const std::vector<meeting_state::OpResult>& results);

// The reason `op` is outside the pass's job, or nullopt.
std::optional<std::string> pass_rejection(const ToolScope& scope, const Json& op);

// Evidence repair before exact validation, when the host can check segment ids and the scope
// has citable ids; otherwise `ops` unchanged. Logs how many ids it repaired.
Json repair_evidence(AgentToolHost& tools, const Json& ops,
                     const std::vector<std::string>& citable_ids);

// A patch_state batch: ops outside the pass are rejected one by one, the others repaired and
// applied in one apply_agent_ops call; the results line up with `ops`. Throws py::ValueError
// ("'ops' must be a list of op objects") when `ops` is not a list.
std::vector<meeting_state::OpResult> apply_patch_ops(AgentToolHost& tools, const Json& ops,
                                                     const ToolScope& scope);

// A read-only search: the host's answer as an object, or {"ok": false, "disabled": true,
// "text": "<not available>", "hits": []} when the host has no such tool. `limit` is int(limit),
// 10 when it is missing or not a number. Throws py::PyError("KeyError") for a name that is not
// one of READ_TOOLS.
Json run_read_tool(AgentToolHost& tools, std::string_view name, const Json& args);

// Answers one model tool call (name without any transport prefix): the payload for the model
// and the write results to report upward (empty for read tools). Throws py::ValueError for an
// unknown tool ("unknown tool: <name>") or a malformed ops list.
std::pair<Json, std::vector<meeting_state::OpResult>>
run_tool(AgentToolHost& tools, std::string_view name, const Json& args, const ToolScope& scope);

// The tool message a chat model reads: a read tool's text, else json.dumps(payload) with ASCII
// escapes and ", " / ": " separators.
std::string tool_result_text(std::string_view name, const Json& payload);

} // namespace fv::meeting_agent
