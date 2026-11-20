// Golden vectors of meeting.agent.tool_policy (tools/parity/cases/meeting.agent.tool_policy.py):
// every call goes to a SpecHost built from the case's host spec, and the host's calls are part
// of the result.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "golden_support.h"
#include "meeting_agent/tool_policy.h"
#include "spec_host.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent_test;
namespace golden = fv::golden;

namespace {

constexpr const char* kPolicy = "meeting.agent.tool_policy";

ToolScope scope_of(const Json& value) {
    ToolScope scope;
    scope.pass_kind = value.at(0).get<std::string>();
    for (const auto& id : value.at(1)) {
        scope.note_ids.insert(id.get<std::string>());
    }
    for (const auto& id : value.at(2)) {
        scope.citable_ids.push_back(id.get<std::string>());
    }
    return scope;
}

CheckpointPayload payload_of(const Json& fields) {
    CheckpointPayload payload;
    payload.request_id = fields.value("request_id", "");
    payload.state_snapshot = fields.contains("state_snapshot") ? fields["state_snapshot"] : Json();
    if (fields.contains("new_segments")) {
        for (const auto& segment : fields["new_segments"]) {
            payload.new_segments.push_back(segment);
        }
    }
    payload.is_consolidation = fields.value("is_consolidation", false);
    payload.is_polish = fields.value("is_polish", false);
    payload.is_notes = fields.value("is_notes", false);
    return payload;
}

Json rows(const std::vector<fv::meeting_state::OpResult>& results) {
    Json out = Json::array();
    for (const auto& result : results) {
        out.push_back(result_row(result));
    }
    return out;
}

Json strings(const std::vector<std::string>& values) {
    Json out = Json::array();
    for (const auto& value : values) {
        out.push_back(value);
    }
    return out;
}

} // namespace

TEST_CASE("tool_policy: pass_rejection for every pass kind", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "pass_rejection", [](const golden::Case& c) {
        replay(c, [&] {
            const auto reason = pass_rejection(scope_of(c.arg<Json>(0)), c.arg<Json>(1));
            return reason ? Json(*reason) : Json();
        });
    });
}

TEST_CASE("tool_policy: tool_result_text", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "tool_result_text", [](const golden::Case& c) {
        replay(c, [&] { return Json(tool_result_text(c.arg<std::string>(0), c.arg<Json>(1))); });
    });
}

TEST_CASE("tool_policy: op result serialization", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "serialize_op_result", [](const golden::Case& c) {
        replay(c, [&] { return serialize_op_result(op_result_of(c.arg<Json>(0))); });
    });
    golden::for_each(kPolicy, "op_results_payload", [](const golden::Case& c) {
        replay(c, [&] {
            std::vector<fv::meeting_state::OpResult> results;
            for (const auto& fields : c.arg<Json>(0)) {
                results.push_back(op_result_of(fields));
            }
            return op_results_payload(results);
        });
    });
}

TEST_CASE("tool_policy: pass kind, citable ids and the scope of a payload",
          "[tool_policy][golden]") {
    golden::for_each(kPolicy, "pass_kind_for", [](const golden::Case& c) {
        replay(c, [&] { return Json(pass_kind_for(payload_of(c.arg<Json>(0)))); });
    });
    golden::for_each(kPolicy, "citable_segment_ids", [](const golden::Case& c) {
        replay(c, [&] { return strings(citable_segment_ids(payload_of(c.arg<Json>(0)))); });
    });
    golden::for_each(kPolicy, "for_payload", [](const golden::Case& c) {
        replay(c, [&] {
            const auto scope = ToolScope::for_payload(payload_of(c.arg<Json>(0)));
            Json notes = Json::array();
            for (const auto& id : scope.note_ids) {
                notes.push_back(id);
            }
            return Json::array({scope.pass_kind, notes, strings(scope.citable_ids)});
        });
    });
}

TEST_CASE("tool_policy: read tools with absent and present hosts", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "run_read_tool", [](const golden::Case& c) {
        replay(c, [&] {
            SpecHost host(c.arg<Json>(0));
            Json payload = run_read_tool(host, c.arg<std::string>(1), c.arg<Json>(2));
            return Json::array({payload, host.calls()});
        });
    });
}

TEST_CASE("tool_policy: patch_state batches through a recording host", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "apply_patch_ops", [](const golden::Case& c) {
        replay(c, [&] {
            SpecHost host(c.arg<Json>(0));
            const auto results = apply_patch_ops(host, c.arg<Json>(1), scope_of(c.arg<Json>(2)));
            return Json::array({rows(results), host.calls()});
        });
    });
}

TEST_CASE("tool_policy: run_tool over every tool", "[tool_policy][golden]") {
    golden::for_each(kPolicy, "run_tool", [](const golden::Case& c) {
        replay(c, [&] {
            SpecHost host(c.arg<Json>(0));
            auto [payload, results] =
                run_tool(host, c.arg<std::string>(1), c.arg<Json>(2), scope_of(c.arg<Json>(3)));
            return Json::array({payload, rows(results), host.calls()});
        });
    });
}

TEST_CASE("tool_policy: repair_evidence needs a segment check and citable ids",
          "[tool_policy][golden]") {
    golden::for_each(kPolicy, "repair_evidence", [](const golden::Case& c) {
        replay(c, [&] {
            SpecHost host(c.arg<Json>(0));
            std::vector<std::string> citable;
            for (const auto& id : c.arg<Json>(2)) {
                citable.push_back(id.get<std::string>());
            }
            Json repaired = repair_evidence(host, c.arg<Json>(1), citable);
            return Json::array({repaired, host.calls()});
        });
    });
}
