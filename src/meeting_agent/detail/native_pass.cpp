#include "meeting_agent/native_core.h"

#include <algorithm>
#include <cctype>
#include <chrono>

#include "meeting_agent/native_tools.h"
#include "meeting_agent/progress_labels.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/scheduler.h"

namespace fv::meeting_agent {
namespace {

// runner.ts refuses a tool call whose checkpoint ended or was canceled with this message.
constexpr std::string_view kInactive = "Meeting request is no longer active";

bool mentions_signature_failure(std::string_view error) {
    std::string lowered(error);
    std::ranges::transform(lowered, lowered.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return lowered.find("corrupted thought signature") != std::string::npos;
}

// tools.ts: a failed bridge call counts every op it carried (at least one) as rejected, and a
// failed question call one; the read tools count nothing. sidecar.py rebuilt those tallies as
// placeholder results (_op_results_from_counts), which is what reaches the scheduler.
void tally_bridge_failure(std::string_view name, const Json& args,
                          std::vector<meeting_state::OpResult>& results) {
    if (is_read_tool(name)) {
        return;
    }
    std::size_t count = 1;
    if (name == "patch_state") {
        const auto ops = args.find("ops");
        if (ops != args.end() && ops->is_array() && !ops->empty()) {
            count = ops->size();
        }
    }
    for (std::size_t i = 0; i < count; ++i) {
        results.push_back({.ok = false,
                           .op = Json{{"op", "sidecar_tool_call"}},
                           .reason = std::string("rejected")});
    }
}

std::string bridge_error_text(std::string_view name, std::string_view message) {
    if (name == "patch_state") {
        return "Tool bridge error, no ops were applied: " + std::string(message);
    }
    return "Tool bridge error: " + std::string(message);
}

} // namespace

AgentResult NativeAgentCore::checkpoint(const CheckpointPayload& payload) {
    if (payload.is_consolidation) {
        return consolidate(payload);
    }
    return run(payload, {payload.is_polish ? FINAL_POLISH_TIMEOUT_S : 60.0, 45.0});
}

AgentResult NativeAgentCore::consolidate(const CheckpointPayload& payload) {
    auto final = payload;
    final.is_consolidation = true;
    return run(final, {CONSOLIDATION_TIMEOUT_CAP_S, 300.0});
}

AgentResult NativeAgentCore::run(const CheckpointPayload& payload, PassBudget budget) {
    std::shared_ptr<Session> session;
    CancelToken token;
    {
        std::lock_guard state(state_mutex_);
        session = session_;
        token = generation_.token();
    }
    // sidecar.py's values (_refuse_checkpoint, and runner.ts's reply for a pass canceled while
    // queued), the same as InstalledAgentCore's.
    if (!session) {
        return {.error = "agent_unavailable"};
    }
    std::lock_guard pass(pass_mutex_);
    if (token.cancelled()) {
        return {.error = "canceled"};
    }

    const auto started = clock_.steady();
    AgentResult result = run_attempt(*session, payload, budget, token);
    const double used = std::chrono::duration<double>(clock_.steady() - started).count();
    if (payload.is_polish && !result.ok && !token.cancelled() && used < budget.wall_s &&
        mentions_signature_failure(result.error) &&
        std::ranges::none_of(result.op_results, [](const auto& op) { return op.ok; })) {
        budget.wall_s -= used;
        return run_attempt(*session, payload, budget, token);
    }
    return result;
}

AgentResult NativeAgentCore::run_attempt(Session& session, const CheckpointPayload& payload,
                                         PassBudget budget, const CancelToken& token) {
    const ToolScope scope = ToolScope::for_payload(payload);
    const bool notes = scope.pass_kind == PASS_NOTES;
    const auto charter = notes ? build_note_taker_system_prompt() : session.charter;
    const auto prompt =
        notes ? build_notes_user_prompt(payload.state_snapshot, payload.new_segments)
              : build_checkpoint_user_prompt(payload.state_snapshot, payload.new_segments,
                                             payload.is_consolidation, payload.is_polish);
    AgentResult result;
    const auto outcome = session.loop.run_pass(
        charter, prompt, native_tools(),
        [&](std::string_view name, const Json& args) -> std::string {
            std::lock_guard authority(authority_);
            if (token.cancelled()) {
                tally_bridge_failure(name, args, result.op_results);
                return bridge_error_text(name, kInactive);
            }
            if (!is_tool_name(name)) {
                return "Tool bridge error: Unknown meeting tool.";
            }
            try {
                Json normalized = args;
                if (name == "patch_state" &&
                    (!normalized.contains("ops") || !normalized["ops"].is_array())) {
                    normalized["ops"] = Json::array();
                }
                auto [answer, ops] = run_tool(*session.tools, name, normalized, scope);
                result.op_results.insert(result.op_results.end(), ops.begin(), ops.end());
                return native_tool_result(name, answer, ops);
            } catch (const std::exception&) {
                tally_bridge_failure(name, args, result.op_results);
                return bridge_error_text(name, "Meeting tool failed.");
            }
        },
        budget, token,
        [&](const LoopEvent& event) {
            std::function<void(const std::string&)> progress;
            std::function<void(const AgentActivity&)> activity;
            {
                std::lock_guard lock(state_mutex_);
                progress = progress_;
                activity = activity_;
            }
            const std::string label = progress_detail(event.type, event.delta, scope.pass_kind);
            // sidecar.py hands every progress notification to the callback; the loop already
            // throttles the frequent deltas as runner.ts did.
            if (progress) {
                try {
                    progress(label);
                } catch (...) {
                    // The optional presentation callback cannot fail a pass.
                }
            }
            if (activity) {
                try {
                    activity({activity_kind(event.type, event.delta), label, event.tool,
                              scope.pass_kind, utc_now_iso(clock_)});
                } catch (...) {
                    // The optional presentation callback cannot fail a pass.
                }
            }
        });
    if (outcome.canceled) {
        // Pi's bridge refused the calls an abort left ("Meeting request is no longer active")
        // and tools.ts counted them rejected; sidecar.py's canceled reply carried those counts.
        // A timed-out pass returned only the real results (_checkpoint_failure).
        for (const auto& call : outcome.skipped_calls) {
            if (!is_tool_name(call.function.name)) {
                continue; // Pi never offered it to the bridge
            }
            const auto args = call.function.arguments.empty()
                                  ? Json::object()
                                  : Json::parse(call.function.arguments, nullptr, false);
            tally_bridge_failure(call.function.name, args.is_object() ? args : Json::object(),
                                 result.op_results);
        }
    }
    result.ok = outcome.ok;
    result.error = outcome.error;
    result.usage = outcome.usage;
    return result;
}

} // namespace fv::meeting_agent
