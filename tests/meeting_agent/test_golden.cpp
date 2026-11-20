// Golden vectors of meeting.agent.prompts, meeting.agent.evidence, meeting.agent.tool_specs,
// meeting.agent.sidecar, meeting.agent.base and meeting.corrections' guidance_prompt
// (tools/parity/cases/<module>.py). Strings are compared rebranded.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "golden_support.h"
#include "meeting_agent/evidence.h"
#include "meeting_agent/guidance.h"
#include "meeting_agent/interfaces.h"
#include "meeting_agent/progress_labels.h"
#include "meeting_agent/prompts.h"
#include "meeting_agent/tool_specs.h"
#include "meeting_agent/usage.h"

using fv::Json;
using namespace fv::meeting_agent;
using fv::meeting_agent_test::arg_or_kwarg;
using fv::meeting_agent_test::replay;
namespace golden = fv::golden;

namespace {

constexpr const char* kPrompts = "meeting.agent.prompts";

std::vector<Json> segments_of(const Json& value) {
    std::vector<Json> out;
    if (value.is_array()) {
        for (const auto& item : value) {
            out.push_back(item);
        }
    }
    return out;
}

void replay_state(const char* function, std::string (*fn)(const Json&)) {
    golden::for_each(kPrompts, function, [fn](const golden::Case& c) {
        replay(c, [&] { return Json(fn(c.arg<Json>(0))); });
    });
}

std::vector<std::string> strings_of(const Json& value) {
    std::vector<std::string> out;
    for (const auto& item : value) {
        out.push_back(item.get<std::string>());
    }
    return out;
}

} // namespace

TEST_CASE("prompts: the two system prompts match OpenWhisper's", "[prompts][golden]") {
    golden::for_each(kPrompts, "build_system_prompt", [](const golden::Case& c) {
        CHECK(golden::equal_rebranded(c.result<std::string>(), build_system_prompt()));
    });
    golden::for_each(kPrompts, "build_note_taker_system_prompt", [](const golden::Case& c) {
        CHECK(golden::equal_rebranded(c.result<std::string>(), build_note_taker_system_prompt()));
    });
}

TEST_CASE("prompts: consolidation instructions for every set of report views",
          "[prompts][golden]") {
    golden::for_each(kPrompts, "build_consolidation_instructions", [](const golden::Case& c) {
        replay(c, [&] { return Json(build_consolidation_instructions(c.arg<Json>(0))); });
    });
}

TEST_CASE("prompts: checkpoint, polish and consolidation user prompts", "[prompts][golden]") {
    golden::for_each(kPrompts, "build_checkpoint_user_prompt", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(build_checkpoint_user_prompt(
                c.arg<Json>(0), segments_of(c.arg<Json>(1)),
                arg_or_kwarg(c, 2, "is_consolidation", false).get<bool>(),
                arg_or_kwarg(c, 3, "is_polish", false).get<bool>()));
        });
    });
}

TEST_CASE("prompts: note-taker user prompts", "[prompts][golden]") {
    golden::for_each(kPrompts, "build_notes_user_prompt", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(build_notes_user_prompt(c.arg<Json>(0), segments_of(c.arg<Json>(1))));
        });
    });
}

TEST_CASE("prompts: brief, compact state and notes page renderings", "[prompts][golden]") {
    replay_state("intent_prompt", &intent_prompt);
    replay_state("render_state_compact", &render_state_compact);
    replay_state("render_notes_page", &render_notes_page);
}

TEST_CASE("prompts: segment lines", "[prompts][golden]") {
    golden::for_each(kPrompts, "format_segment_line", [](const golden::Case& c) {
        replay(c, [&] { return Json(format_segment_line(c.arg<Json>(0), c.arg<Json>(1))); });
    });
}

TEST_CASE("prompts: Top Insights selection", "[prompts][golden]") {
    golden::for_each(kPrompts, "select_spotlight_items", [](const golden::Case& c) {
        replay(c, [&] {
            const int limit = static_cast<int>(arg_or_kwarg(c, 1, "limit", 3).get<std::int64_t>());
            Json out = Json::array();
            for (auto& item : select_spotlight_items(c.arg<Json>(0), limit)) {
                out.push_back(std::move(item));
            }
            return out;
        });
    });
}

TEST_CASE("evidence: repair_evidence_ids and the single-id repair", "[evidence][golden]") {
    golden::for_each("meeting.agent.evidence", "repair_evidence_ids", [](const golden::Case& c) {
        replay(c, [&] {
            const Json ops = c.arg<Json>(0);
            const auto citable = strings_of(c.arg<Json>(1));
            const auto existing = strings_of(c.arg<Json>(2));
            const auto [repaired, count] =
                repair_evidence_ids(ops, citable, [&](std::string_view id) {
                    return std::ranges::find(existing, id) != existing.end();
                });
            CHECK(ops == c.arg<Json>(0));
            return Json::array({repaired, count});
        });
    });
    golden::for_each("meeting.agent.evidence", "_repair_one", [](const golden::Case& c) {
        replay(c, [&] {
            const auto fixed = repair_one(c.arg<std::string>(0), strings_of(c.arg<Json>(1)));
            return fixed ? Json(*fixed) : Json();
        });
    });
}

TEST_CASE("tool_specs: the meeting tools as data", "[tool_specs][golden]") {
    const char* module = "meeting.agent.tool_specs";
    golden::for_each(module, "MEETING_TOOLS",
                     [](const golden::Case& c) { replay(c, [] { return meeting_tools(); }); });
    golden::for_each(module, "NOOP_TOOL",
                     [](const golden::Case& c) { replay(c, [] { return noop_tool(); }); });
    golden::for_each(module, "mcp_tool_definitions", [](const golden::Case& c) {
        replay(c, [] { return mcp_tool_definitions(); });
    });
}

TEST_CASE("progress_labels: progress sentences and activity kinds", "[progress][golden]") {
    const char* module = "meeting.agent.sidecar";
    golden::for_each(module, "_progress_detail", [](const golden::Case& c) {
        replay(c, [&] {
            return Json(progress_detail(c.arg<std::string>(0), c.arg<std::string>(1),
                                        c.arg<std::string>(2)));
        });
    });
    golden::for_each(module, "_activity_kind", [](const golden::Case& c) {
        replay(c,
               [&] { return Json(activity_kind(c.arg<std::string>(0), c.arg<std::string>(1))); });
    });
    golden::for_each(module, "AgentActivity", [](const golden::Case& c) {
        replay(c, [&] {
            const AgentActivity activity{c.arg<std::string>(0), c.arg<std::string>(1),
                                         c.arg<std::string>(2), c.arg<std::string>(3),
                                         c.arg<std::string>(4)};
            return activity.to_json();
        });
    });
}

TEST_CASE("usage: merge_usage sequences", "[usage][golden]") {
    golden::for_each("meeting.agent.base", "merge_usage", [](const golden::Case& c) {
        replay(c, [&] {
            Json total = c.arg<Json>(0);
            for (const auto& usage : c.arg<Json>(1)) {
                merge_usage(total, usage);
            }
            return total;
        });
    });
}

TEST_CASE("guidance: guidance_prompt over the corrections states", "[guidance][golden]") {
    golden::for_each("meeting.corrections", "guidance_prompt", [](const golden::Case& c) {
        replay(c, [&] { return Json(guidance_prompt(c.arg<Json>(0))); });
    });
}
