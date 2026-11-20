#include "meeting_agent/tool_policy.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>

#include "base/log.h"
#include "meeting_agent/detail/py_value.h"
#include "meeting_agent/evidence.h"
#include "meeting_state/patches.h"
#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting_agent {

namespace {

constexpr log::Logger logger("meeting.agent.tool_policy");

using detail::get;
using detail::py_truthy;
using meeting_state::OpResult;

constexpr std::int64_t kDefaultReadLimit = 10;

Json optional_text(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json();
}

Json optional_int(const std::optional<std::int64_t>& value) {
    return value ? Json(*value) : Json();
}

OpResult rejected(Json op, std::string_view reason) {
    OpResult result;
    result.ok = false;
    result.op = std::move(op);
    result.reason = std::string(reason);
    return result;
}

// int(value), 10 for Python's TypeError or ValueError; an infinite float raises
// OverflowError as int() does, and an integer past int64 is clamped (the hosts cap at 20).
Json read_limit(const Json& value) {
    const auto limit = py::py_int_checked(value);
    if (limit) {
        return Json(*limit);
    }
    if (limit.error() != py::IntError::Overflow) {
        return Json(kDefaultReadLimit);
    }
    if (value.is_number_float()) {
        throw py::OverflowError("cannot convert float infinity to integer");
    }
    const bool negative = value.is_string() && py::strip(value.get<std::string>()).starts_with('-');
    return Json(negative ? std::numeric_limits<std::int64_t>::min()
                         : std::numeric_limits<std::int64_t>::max());
}

// float(value or 0.0) for a resolve_question confidence.
double confidence_of(const Json& value) {
    if (!py_truthy(value)) {
        return 0.0;
    }
    if (const auto number = py::py_float(value)) {
        return *number;
    }
    if (value.is_string()) {
        throw py::ValueError("could not convert string to float: " + detail::py_repr(value));
    }
    throw py::TypeError("float() argument must be a string or a real number");
}

OpResult answer_question(AgentToolHost& tools, std::string_view name, const Json& args,
                         const ToolScope& scope) {
    const std::string op_name(name);
    if (scope.pass_kind == PASS_POLISH) {
        return rejected(Json{{"op", op_name}}, POLISH_ONLY);
    }
    if (scope.pass_kind == PASS_NOTES) {
        return rejected(Json{{"op", op_name}}, NOTES_ONLY);
    }
    Json evidence = detail::py_list_or_empty(get(args, "evidence"));
    const Json repaired = repair_evidence(
        tools, Json::array({Json{{"op", op_name}, {"evidence", evidence}}}), scope.citable_ids);
    const Json& fixed = get(repaired.at(0), "evidence");
    if (py_truthy(fixed)) {
        evidence = fixed;
    }
    if (name == "ask_question") {
        return tools.ask_question(detail::str_or_empty(get(args, "text")), evidence);
    }
    return tools.resolve_question(detail::str_or_empty(get(args, "question_id")),
                                  detail::str_or_empty(get(args, "answer_text")),
                                  confidence_of(get(args, "confidence")), evidence);
}

} // namespace

bool is_tool_name(std::string_view name) {
    return std::ranges::find(TOOL_NAMES, name) != TOOL_NAMES.end();
}

bool is_read_tool(std::string_view name) {
    return std::ranges::find(READ_TOOLS, name) != READ_TOOLS.end();
}

std::string pass_kind_for(const CheckpointPayload& payload) {
    if (payload.is_consolidation) {
        return std::string(PASS_CONSOLIDATION);
    }
    if (payload.is_polish) {
        return std::string(PASS_POLISH);
    }
    if (payload.is_notes) {
        return std::string(PASS_NOTES);
    }
    return std::string(PASS_CARDS);
}

std::vector<std::string> citable_segment_ids(const CheckpointPayload& payload) {
    std::vector<std::string> ids;
    const auto add = [&ids](const Json& segment) {
        if (segment.is_object() && py_truthy(get(segment, "id"))) {
            ids.push_back(detail::py_str(get(segment, "id")));
        }
    };
    for (const auto& segment : payload.new_segments) {
        add(segment);
    }
    const Json& context = get(payload.state_snapshot, "recent_transcript_context");
    if (context.is_array()) {
        for (const auto& segment : context) {
            add(segment);
        }
    }
    return ids;
}

ToolScope ToolScope::for_payload(const CheckpointPayload& payload) {
    ToolScope scope;
    scope.pass_kind = pass_kind_for(payload);
    if (scope.pass_kind == PASS_NOTES) {
        scope.note_ids = meeting_state::live_note_ids(payload.state_snapshot);
    }
    scope.citable_ids = citable_segment_ids(payload);
    return scope;
}

Json serialize_op_result(const OpResult& result) {
    return Json{{"ok", result.ok},
                {"reason", optional_text(result.reason)},
                {"target_id", optional_text(result.target_id)},
                {"seq", optional_int(result.seq)},
                {"current_revision", optional_int(result.current_revision)}};
}

Json op_results_payload(const std::vector<OpResult>& results) {
    Json listed = Json::array();
    for (const auto& result : results) {
        listed.push_back(serialize_op_result(result));
    }
    return Json{{"results", std::move(listed)}};
}

std::optional<std::string> pass_rejection(const ToolScope& scope, const Json& op) {
    if (scope.pass_kind == PASS_POLISH) {
        if (!op.is_object() || !detail::is_text(get(op, "op"), "revise_segment_text")) {
            return std::string(POLISH_ONLY);
        }
    } else if (scope.pass_kind == PASS_NOTES) {
        if (meeting_state::filter_notes_ops(Json::array({op}), scope.note_ids).empty()) {
            return std::string(NOTES_ONLY);
        }
    }
    return std::nullopt;
}

Json repair_evidence(AgentToolHost& tools, const Json& ops,
                     const std::vector<std::string>& citable_ids) {
    if (!tools.can_check_segments() || citable_ids.empty()) {
        return ops;
    }
    auto [repaired, count] = repair_evidence_ids(
        ops, citable_ids, [&tools](std::string_view id) { return tools.segment_exists(id); });
    if (count != 0) {
        logger.info("Repaired {} mistyped evidence id(s)", count);
    }
    return std::move(repaired);
}

std::vector<OpResult> apply_patch_ops(AgentToolHost& tools, const Json& ops,
                                      const ToolScope& scope) {
    if (!ops.is_array()) {
        throw py::ValueError("'ops' must be a list of op objects");
    }
    std::map<std::size_t, OpResult> denied;
    Json allowed = Json::array();
    for (std::size_t index = 0; index < ops.size(); ++index) {
        if (const auto reason = pass_rejection(scope, ops[index])) {
            denied.emplace(index, rejected(ops[index], *reason));
        } else {
            allowed.push_back(ops[index]);
        }
    }
    std::vector<OpResult> applied;
    if (!allowed.empty()) {
        applied = tools.apply_agent_ops(repair_evidence(tools, allowed, scope.citable_ids));
    }
    if (denied.empty()) {
        return applied;
    }
    std::vector<OpResult> merged;
    merged.reserve(ops.size());
    std::size_t next = 0;
    for (std::size_t index = 0; index < ops.size(); ++index) {
        if (auto found = denied.find(index); found != denied.end()) {
            merged.push_back(std::move(found->second));
            continue;
        }
        if (next >= applied.size()) {
            // next(remaining) past the end: the host answered fewer ops than it was given.
            throw py::PyError("StopIteration", "the tool host returned fewer results than ops");
        }
        merged.push_back(std::move(applied[next++]));
    }
    return merged;
}

Json run_read_tool(AgentToolHost& tools, std::string_view name, const Json& args) {
    if (!is_read_tool(name)) {
        // _READ_TOOLS[name]: any other name is not a search the host could have.
        throw py::PyError("KeyError", py::repr(name));
    }
    const bool past = name == "search_past_meetings";
    const bool available =
        past ? tools.can_search_past_meetings() : tools.can_search_context_files();
    if (!available) {
        return Json{{"ok", false},
                    {"disabled", true},
                    {"text", past ? "Past-meeting recall is not available."
                                  : "Knowledge-folder search is not available."},
                    {"hits", Json::array()}};
    }
    const std::string query = detail::str_or_empty(get(args, "query"));
    const Json limit = read_limit(get(args, "limit"));
    const std::string target =
        py::strip(detail::str_or_empty(get(args, past ? "meeting_id" : "relative_path")));
    Json result = past ? tools.search_past_meetings(query, target, limit)
                       : tools.search_context_files(query, target, limit);
    if (result.is_object()) {
        return result;
    }
    return Json{{"ok", true}, {"text", detail::str_or_empty(result)}, {"hits", Json::array()}};
}

std::pair<Json, std::vector<OpResult>> run_tool(AgentToolHost& tools, std::string_view name,
                                                const Json& args, const ToolScope& scope) {
    if (is_read_tool(name)) {
        return {run_read_tool(tools, name, args), {}};
    }
    if (name == "patch_state") {
        auto results = apply_patch_ops(tools, get(args, "ops"), scope);
        Json payload = op_results_payload(results);
        return {std::move(payload), std::move(results)};
    }
    if (name == "ask_question" || name == "resolve_question") {
        OpResult result = answer_question(tools, name, args, scope);
        Json payload = serialize_op_result(result);
        return {std::move(payload), {std::move(result)}};
    }
    throw py::ValueError("unknown tool: " + std::string(name));
}

std::string tool_result_text(std::string_view name, const Json& payload) {
    if (is_read_tool(name)) {
        const Json& text = get(payload, "text");
        return py_truthy(text) ? detail::py_str(text) : py::dumps(payload);
    }
    return py::dumps(payload);
}

} // namespace fv::meeting_agent
