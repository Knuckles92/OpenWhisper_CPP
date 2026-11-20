// services/thinking_levels.py: which thinking levels each runner and model offer, and what runs.

#include <catch2/catch_test_macros.hpp>

#include <map>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "llm/model_catalog.h"
#include "llm/profiles.h"
#include "llm/thinking_levels.h"

using namespace fv;
using namespace fv::llm;

namespace {

using Levels = std::vector<std::string>;

// Restores Codex's efforts when a test that installed its own ends.
struct CodexEffortsScope {
    CodexEffortsScope(std::map<std::string, std::vector<std::string>> efforts,
                      std::string default_model) {
        set_codex_efforts([efforts] { return efforts; },
                          [default_model](std::string_view) { return default_model; });
    }
    ~CodexEffortsScope() { set_codex_efforts({}, {}); }
    CodexEffortsScope(const CodexEffortsScope&) = delete;
    CodexEffortsScope& operator=(const CodexEffortsScope&) = delete;
};

// A custom endpoint whose saved metadata answers model_spec for one model, as the Python test
// replaces model_spec.
TextLLMProfile custom_with(const std::string& model, Json spec) {
    TextLLMProfile profile;
    profile.id = "custom_test";
    profile.name = "Test";
    profile.kind = "custom";
    profile.base_url = "https://gateway.test/v1";
    spec["model"] = model;
    profile.model_metadata = spec;
    return profile;
}

} // namespace

TEST_CASE("claude code offers its cli levels", "[test_thinking_levels]") {
    // from tests/test_thinking_levels.py::test_claude_code_offers_its_cli_levels
    CHECK(agent_levels("claude_code", "haiku") == Levels{"low", "medium", "high", "xhigh", "max"});
}

TEST_CASE("codex levels come from the model and unknown models get the shared ones",
          "[test_thinking_levels]") {
    // from tests/test_thinking_levels.py::
    //     test_codex_levels_come_from_the_model_and_unknown_models_get_the_shared_ones
    CodexEffortsScope scope({{"sol", {"low", "medium", "high", "xhigh", "max", "ultra"}},
                             {"luna", {"medium", "low", "high", "xhigh", "max"}}},
                            "");
    CHECK(agent_levels("codex", "sol").back() == "ultra");
    CHECK(agent_levels("codex", "luna") == Levels{"low", "medium", "high", "xhigh", "max"});
    CHECK(agent_levels("codex", "") == Levels{"low", "medium", "high", "xhigh", "max"});
}

TEST_CASE("codex without its effort cache falls back to low, medium, high",
          "[test_thinking_levels]") {
    set_codex_efforts({}, {});
    CHECK(agent_levels("codex", "sol") == Levels{"low", "medium", "high"});
    CHECK(agent_levels("opencode_cli", "any") == Levels{"low", "medium", "high"});
    CodexEffortsScope scope({{"sol", {"high"}}, {"luna", {"low"}}}, "sol");
    // The configured default model answers for an unnamed one; disjoint models share nothing.
    CHECK(agent_levels("codex", "") == Levels{"high"});
    CHECK(agent_levels("codex", "other") == Levels{"low", "medium", "high"});
}

TEST_CASE("endpoint catalog adds and removes levels", "[test_thinking_levels]") {
    // from tests/test_thinking_levels.py::test_endpoint_catalog_adds_and_removes_levels
    const auto plain =
        custom_with("plain", Json{{"reasoning", true}, {"reasoning_format", "openai"}});
    const auto deep = custom_with(
        "deep", Json{{"reasoning", true},
                     {"reasoning_format", "openai"},
                     {"thinking_levels", {{"off", nullptr}, {"xhigh", "xhigh"}, {"max", "max"}}}});
    const auto chat = custom_with("chat", Json::object());
    CHECK(endpoint_levels(&plain, "plain") == Levels{"off", "low", "medium", "high"});
    CHECK(endpoint_levels(&deep, "deep") == Levels{"low", "medium", "high", "xhigh", "max"});
    CHECK(endpoint_levels(&chat, "chat").empty());
    CHECK(endpoint_levels(nullptr, "chat").empty());
}

TEST_CASE("openai offers levels only for reasoning models", "[test_thinking_levels]") {
    // from tests/test_thinking_levels.py::test_openai_offers_levels_only_for_reasoning_models
    const auto openai = *builtin_profile("openai");
    CHECK(endpoint_levels(&openai, "gpt-5-mini") == Levels{"off", "low", "medium", "high"});
    CHECK(endpoint_levels(&openai, "gpt-4.1").empty());
}

TEST_CASE("saved level runs at the nearest offered one", "[test_thinking_levels]") {
    // from tests/test_thinking_levels.py::test_saved_level_runs_at_the_nearest_offered_one
    const Levels agent{"low", "medium", "high", "xhigh", "max"};
    CHECK(effective_level("off", agent) == "low");
    CHECK(effective_level("ultra", agent) == "max");
    CHECK(effective_level("high", agent) == "high");
    CHECK(effective_level("max", {"off", "low", "medium", "high"}) == "high");
    CHECK(effective_level("high", {}) == "off");
}

TEST_CASE("labels name every level in order", "[test_thinking_levels]") {
    REQUIRE(kThinkingLabels.size() == kThinkingOrder.size());
    for (std::size_t i = 0; i < kThinkingOrder.size(); ++i) {
        CHECK(kThinkingLabels[i].first == kThinkingOrder[i]);
    }
    CHECK(thinking_label("xhigh") == "Extra high");
    CHECK_FALSE(thinking_label("extreme"));
}

TEST_CASE("thinking levels replay services.thinking_levels", "[golden]") {
    golden::for_each("services.thinking_levels", "levels_for", [](const golden::Case& c) {
        const auto levels =
            levels_for(c.arg<std::string>(0), c.arg<std::string>(1), c.arg<Json>(2));
        CHECK(Json(levels) == c.result<Json>());
    });
    golden::for_each("services.thinking_levels", "nearest", [](const golden::Case& c) {
        CHECK(Json(nearest(c.arg<std::string>(0), c.arg<Levels>(1)).value_or("")) ==
              Json(c.result<std::optional<std::string>>().value_or("")));
    });
    golden::for_each("services.thinking_levels", "effective_level", [](const golden::Case& c) {
        CHECK(effective_level(c.arg<std::string>(0), c.arg<Levels>(1)) == c.result<std::string>());
    });
}
