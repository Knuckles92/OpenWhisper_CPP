#include "meeting_agent/progress_labels.h"

#include <array>

#include "meeting_agent/interfaces.h"
#include "meeting_agent/tool_policy.h"

namespace fv::meeting_agent {

namespace {

struct Entry {
    std::string_view key;
    std::string_view value;
};

template <std::size_t N>
std::optional<std::string_view> lookup(const std::array<Entry, N>& table, std::string_view key) {
    for (const auto& entry : table) {
        if (entry.key == key) {
            return entry.value;
        }
    }
    return std::nullopt;
}

// _PROGRESS_DETAILS.
constexpr std::array<Entry, 16> kDetails{{
    {"thinking_start", "Model is thinking through the transcript…"},
    {"thinking_delta", "Model is thinking through the transcript…"},
    {"thinking_end", "Model finished thinking…"},
    {"text_delta", "Model is writing the final report…"},
    {"toolcall_start", "Preparing a dashboard update…"},
    {"toolcall_delta", "Preparing a dashboard update…"},
    {"tool_execution_start", "Updating the dashboard…"},
    {"tool_execution_end", "Updating the dashboard…"},
    {"turn_start", "Starting the next model turn…"},
    {"turn_end", "Model finished a turn…"},
    {"agent_start", "Preparing final report…"},
    {"agent_end", "Final insights pass is wrapping up…"},
    {"agent_settled", "Final insights pass is wrapping up…"},
    {"auto_retry_start", "Retrying a dropped model request…"},
    {"compaction_start", "Compressing long context…"},
    {"auto_compaction_start", "Compressing long context…"},
}};

// _PASS_PROGRESS_DETAILS: only what would otherwise say "final report" during a rolling pass.
constexpr std::array<Entry, 4> kCardsDetails{{
    {"text_delta", "Model is writing dashboard updates…"},
    {"agent_start", "Reviewing the last few minutes…"},
    {"agent_end", "Checkpoint is wrapping up…"},
    {"agent_settled", "Checkpoint is wrapping up…"},
}};
constexpr std::array<Entry, 6> kNotesDetails{{
    {"text_delta", "Model is writing the meeting notes…"},
    {"agent_start", "Updating the meeting notes…"},
    {"agent_end", "Notes pass is wrapping up…"},
    {"agent_settled", "Notes pass is wrapping up…"},
    {"tool_execution_start", "Updating the meeting notes…"},
    {"tool_execution_end", "Updating the meeting notes…"},
}};
constexpr std::array<Entry, 6> kPolishDetails{{
    {"text_delta", "Model is cleaning up the transcript…"},
    {"agent_start", "Cleaning up the transcript…"},
    {"agent_end", "Transcript cleanup is wrapping up…"},
    {"agent_settled", "Transcript cleanup is wrapping up…"},
    {"tool_execution_start", "Applying transcript fixes…"},
    {"tool_execution_end", "Applying transcript fixes…"},
}};

// _PASS_FALLBACK_DETAILS and _DEFAULT_FALLBACK_DETAIL.
constexpr std::array<Entry, 4> kFallbacks{{
    {PASS_CARDS, "Reviewing the last few minutes…"},
    {PASS_NOTES, "Updating the meeting notes…"},
    {PASS_POLISH, "Cleaning up the transcript…"},
    {PASS_CONSOLIDATION, "Preparing final report…"},
}};
constexpr std::string_view kDefaultFallback = "Preparing final report…";

// _DELTA_ACTIVITY_KINDS and _EVENT_ACTIVITY_KINDS.
constexpr std::array<Entry, 6> kDeltaKinds{{
    {"thinking_start", "thinking"},
    {"thinking_delta", "thinking"},
    {"thinking_end", "thinking"},
    {"text_delta", "writing"},
    {"toolcall_start", "tool"},
    {"toolcall_delta", "tool"},
}};
constexpr std::array<Entry, 14> kEventKinds{{
    {"tool_execution_start", "tool"},
    {"tool_execution_update", "tool"},
    {"tool_execution_end", "tool"},
    {"turn_start", "turn"},
    {"turn_end", "turn"},
    {"agent_start", "start"},
    {"agent_end", "settled"},
    {"agent_settled", "settled"},
    {"auto_retry_start", "retry"},
    {"auto_retry_end", "retry"},
    {"compaction_start", "compaction"},
    {"compaction_end", "compaction"},
    {"auto_compaction_start", "compaction"},
    {"auto_compaction_end", "compaction"},
}};

struct PiEvent {
    std::string_view kind;
    std::string_view event;
    std::string_view delta;
};
constexpr std::array<PiEvent, 7> kPiEvents{{
    {"start", "agent_start", ""},
    {"thinking", "message_update", "thinking_delta"},
    {"writing", "message_update", "text_delta"},
    {"tool", "tool_execution_start", ""},
    {"turn", "turn_start", ""},
    {"retry", "auto_retry_start", ""},
    {"settled", "agent_end", ""},
}};

std::optional<std::string_view> pass_override(std::string_view pass_kind, std::string_view key) {
    if (pass_kind == PASS_CARDS) {
        return lookup(kCardsDetails, key);
    }
    if (pass_kind == PASS_NOTES) {
        return lookup(kNotesDetails, key);
    }
    if (pass_kind == PASS_POLISH) {
        return lookup(kPolishDetails, key);
    }
    return std::nullopt;
}

} // namespace

std::string progress_detail(std::string_view event, std::string_view delta,
                            std::string_view pass_kind) {
    const std::string_view fallback = lookup(kFallbacks, pass_kind).value_or(kDefaultFallback);
    if (!delta.empty()) {
        return std::string(
            pass_override(pass_kind, delta).value_or(lookup(kDetails, delta).value_or(fallback)));
    }
    if (event.find("tool") != std::string_view::npos) {
        return std::string(
            pass_override(pass_kind, event).value_or(*lookup(kDetails, "tool_execution_start")));
    }
    return std::string(
        pass_override(pass_kind, event).value_or(lookup(kDetails, event).value_or(fallback)));
}

std::string activity_kind(std::string_view event, std::string_view delta) {
    auto kind = lookup(kDeltaKinds, delta);
    if (!kind) {
        kind = lookup(kEventKinds, event);
    }
    if (!kind && event.find("tool") != std::string_view::npos) {
        kind = "tool";
    }
    return kind && is_activity_kind(*kind) ? std::string(*kind) : std::string("update");
}

std::optional<std::pair<std::string_view, std::string_view>> pi_event(std::string_view kind) {
    for (const auto& entry : kPiEvents) {
        if (entry.kind == kind) {
            return std::pair{entry.event, entry.delta};
        }
    }
    return std::nullopt;
}

} // namespace fv::meeting_agent
