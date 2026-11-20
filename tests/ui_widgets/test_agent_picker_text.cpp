// The agent picker's copy (ui_qt/widgets/agent_picker.py:178-249 and the purpose wording of
// AgentPicker) replayed against OpenWhisper's (tests/golden/ui_qt.widgets.agent_picker.json):
// the module's functions as they are, and what a picker for "AI insights" or "AI cleanup" shows.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "agents/installed_agents.h"
#include "base/json.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "ui_widgets/agent_picker_text.h"

namespace golden = fv::golden;
namespace text = fv::agent_picker;
using fv::Json;
using fv::agents::InstalledAgent;
using fv::agents::ScanResult;

namespace {

constexpr std::string_view kModule = "ui_qt.widgets.agent_picker";

std::optional<InstalledAgent> agentFrom(const Json& spec) {
    if (spec.is_null()) {
        return std::nullopt;
    }
    InstalledAgent agent;
    agent.id = spec.at("id").get<std::string>();
    agent.path = fv::path_from_utf8(spec.at("path").get<std::string>());
    agent.version = spec.at("version").get<std::string>();
    agent.account = spec.at("account").get<std::string>();
    if (!spec.at("signed_in").is_null()) {
        agent.signed_in = spec.at("signed_in").get<bool>();
    }
    agent.problem = spec.at("problem").get<std::string>();
    return agent;
}

ScanResult scanFrom(const Json& scan) {
    ScanResult result;
    for (const auto& [id, spec] : scan.items()) {
        result[id] = agentFrom(spec);
    }
    return result;
}

Json stateJson(const text::TileState& state) {
    return Json::array({state.tone, state.pill, state.detail, state.selectable, state.installed});
}

} // namespace

TEST_CASE("agent picker golden command_text", "[golden]") {
    golden::for_each(kModule, "command_text", [](const golden::Case& c) {
        CHECK(text::command_text(c.arg<std::string>(0)) == c.result<std::string>());
    });
}

TEST_CASE("agent picker golden describe_agent", "[golden]") {
    golden::for_each(kModule, "describe_agent", [](const golden::Case& c) {
        const auto state = text::describe_agent(c.arg<std::string>(0), agentFrom(c.args()[1]));
        CHECK(golden::equal(c.raw().at("result"), stateJson(state)));
    });
}

TEST_CASE("agent picker golden found_count_text", "[golden]") {
    golden::for_each(kModule, "found_count_text", [](const golden::Case& c) {
        CHECK(text::found_count_text(scanFrom(c.args()[0])) == c.result<std::string>());
    });
}

TEST_CASE("agent picker golden usage_caption", "[golden]") {
    golden::for_each(kModule, "usage_caption", [](const golden::Case& c) {
        const std::string actual = text::usage_caption(*agentFrom(c.args()[0]));
        INFO(actual);
        CHECK(golden::equal_rebranded(c.result<std::string>(), actual));
    });
}

TEST_CASE("agent picker golden blocked_notice", "[golden]") {
    golden::for_each(kModule, "blocked_notice", [](const golden::Case& c) {
        CHECK(text::blocked_notice(c.arg<std::string>(0), agentFrom(c.args()[1])) ==
              c.result<std::string>());
    });
}

TEST_CASE("agent picker golden picker_count_text", "[golden]") {
    golden::for_each(kModule, "picker_count_text", [](const golden::Case& c) {
        CHECK(text::purpose_count_text(scanFrom(c.args()[0]), c.arg<std::string>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("agent picker golden picker_usage_caption", "[golden]") {
    golden::for_each(kModule, "picker_usage_caption", [](const golden::Case& c) {
        const std::string actual =
            text::purpose_usage_caption(*agentFrom(c.args()[0]), c.arg<std::string>(1));
        INFO(actual);
        CHECK(golden::equal_rebranded(c.result<std::string>(), actual));
    });
}

TEST_CASE("agent picker golden picker_blocked_notice", "[golden]") {
    golden::for_each(kModule, "picker_blocked_notice", [](const golden::Case& c) {
        CHECK(text::purpose_blocked_notice(c.arg<std::string>(0), agentFrom(c.args()[1]),
                                           c.arg<std::string>(2)) == c.result<std::string>());
    });
}

TEST_CASE("agent picker states and timings are OpenWhisper's", "[unit]") {
    CHECK(text::idle_state() == text::TileState{"idle", "", "", false, false});
    CHECK(text::builtin_state() == text::TileState{"builtin", "Built in",
                                                   "Your API key · the insights model below", true,
                                                   true});
    CHECK(text::cleanup_builtin_state() ==
          text::TileState{"builtin", "Built in", "The cleanup model below", true, true});
    CHECK(text::kStaggerMs == 100);
    CHECK(text::kArrivalMs == 380);
    CHECK(text::kShimmerFadeMs == 160);
    CHECK(text::kHoverMs == 140);
    CHECK(text::kLiftMs == 220);
    CHECK(text::kShimmerPeriodMs == 1300.0);
    CHECK(text::kShimmerStart == 0.3);
    CHECK(text::kShimmerOffsetMs == 140.0);
    CHECK(text::kTickMs == 16);
    CHECK(text::kFilterThreshold == 20);
}
