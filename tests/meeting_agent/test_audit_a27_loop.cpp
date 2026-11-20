// Audit a27 (docs/port/audit/a27-meeting-sidecars-ts.json) and A26-31: what the built-in loop
// shows its model and what a pass returns where the audit found it differing from OpenWhisper's
// Pi sidecar. The tools are tools.ts's (A27-10 to A27-14), a turn cut off at the output limit
// settles as Pi's turnFailure counts it (A27-19), a provider failure reads as ARCHITECTURE.md's
// "{provider name} model turn failed (HTTP {status})" (A27-24), a canceled or refused pass
// returns sidecar.py's values and counts the calls it never ran as Pi's bridge did (A27-28), an
// empty model is refused as runner.ts refused it (A27-03).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "base/identity.h"
#include "meeting_agent/native_tools.h"
#include "meeting_agent/tool_specs.h"
#include "native_support.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using fv::Json;
using fv::meeting_state::OpResult;

Json load_native_fixture(const char* name) {
    const auto path =
        fv::testkit::source_root() / "tests" / "fixtures" / "meeting_agent" / "native" / name;
    std::ifstream file(path, std::ios::binary);
    REQUIRE(file);
    return fv::py::loads(
        std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
}

// The fixture's tools with the product's name where tools.ts named OpenWhisper's.
Json branded(Json tools, const std::string& placeholder) {
    for (auto& tool : tools) {
        auto& description = tool["function"]["description"];
        std::string text = description.get<std::string>();
        for (auto at = text.find(placeholder); at != std::string::npos;
             at = text.find(placeholder, at)) {
            text.replace(at, placeholder.size(), fv::identity().product);
        }
        description = text;
    }
    return tools;
}

fv::llm::TextLLMProfile loop_profile(const SseProvider& provider) {
    return local_profile(provider);
}

AgentLoop make_loop(const SseProvider& provider) {
    const auto endpoint = loop_profile(provider);
    return AgentLoop(direct_client(), endpoint, "local-model",
                     fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", fv::system_clock());
}

const Json kRevision = {{"op", "revise_segment_text"},
                        {"segment_id", "sg_1"},
                        {"text", "corrected"},
                        {"evidence", Json::array({"sg_1"})}};

} // namespace

TEST_CASE("the built-in loop offers tools.ts's five tools, the installed agents tool_specs.py's",
          "[audit][native]") {
    const Json fixture = load_native_fixture("tools.json");
    const auto placeholder = fixture.at("product_placeholder").get<std::string>();
    CHECK(native_tools() == branded(fixture.at("tools"), placeholder));
    CHECK(native_probe_tool() == fixture.at("probe_tool").at("tool"));
    // Two texts by design, not drift: Pi's sidecar registered tools.ts's, the installed agents'
    // MCP server and the retired direct agent tool_specs.py's.
    REQUIRE(native_tools().size() == meeting_tools().size());
    for (std::size_t i = 0; i < native_tools().size(); ++i) {
        CHECK(native_tools()[i]["function"]["name"] == meeting_tools()[i]["function"]["name"]);
        CHECK(native_tools()[i]["function"]["description"] !=
              meeting_tools()[i]["function"]["description"]);
    }
    const auto past = native_tools()[3]["function"]["description"].get<std::string>();
    CHECK(past.starts_with("Search earlier " + std::string(fv::identity().product) + " meetings"));
}

TEST_CASE("every built-in pass sends tools.ts's tools", "[audit][native]") {
    Harness harness({probe(), text_turn(), text_turn(), text_turn()});
    REQUIRE(harness.core->checkpoint({.request_id = "cards"}).ok);
    REQUIRE(harness.core->checkpoint({.request_id = "notes", .is_notes = true}).ok);
    REQUIRE(harness.core->consolidate({.request_id = "final", .is_consolidation = true}).ok);
    const auto bodies = harness.bodies();
    REQUIRE(bodies.size() == 4);
    CHECK(bodies[0]["tools"] == Json::array({native_probe_tool()}));
    for (std::size_t i = 1; i < bodies.size(); ++i) {
        CHECK(bodies[i]["tools"] == native_tools());
    }
}

TEST_CASE("a turn cut off at the output limit settles the pass with what it did",
          "[audit][native]") {
    // pi-adapter.ts turnFailure: a "length" stop is a settled turn, not a failure; its complete
    // tool calls run and the loop goes on.
    SseProvider provider(
        {turn({{"tool_calls",
                Json::array({call("w", "patch_state", patch(Json::array({kRevision})))})}},
              "length"),
         turn({{"content", "Partial answ"}}, "length")});
    auto loop = make_loop(provider);
    std::vector<std::string> ran;
    const auto outcome = loop.run_pass("charter", "prompt", native_tools(),
                                       [&](std::string_view name, const Json&) {
                                           ran.emplace_back(name);
                                           return std::string("1 applied, 0 rejected\n{}");
                                       },
                                       {10, {}}, {});
    INFO(outcome.error);
    CHECK(outcome.ok);
    CHECK(outcome.error.empty());
    CHECK(ran == std::vector<std::string>{"patch_state"});
    CHECK(provider.requests().size() == 2);
}

TEST_CASE("a content-filter stop still fails the pass", "[audit][native]") {
    SseProvider provider({turn({{"content", "Blocked"}}, "content_filter")});
    auto loop = make_loop(provider);
    const auto outcome = loop.run_pass("charter", "prompt", native_tools(), {}, {10, {}}, {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error == "Model output was incomplete (content_filter).");
}

TEST_CASE("a reply without an assistant message fails the pass", "[audit][native]") {
    // Pi: "model produced no assistant message".
    SseProvider provider({SseReply::json(200, {{"choices", Json::array()}})});
    auto loop = make_loop(provider);
    const auto outcome = loop.run_pass("charter", "prompt", native_tools(), {}, {10, {}}, {});
    CHECK_FALSE(outcome.ok);
    CHECK_FALSE(outcome.canceled);
    CHECK(outcome.error == "Model produced no assistant message.");
}

TEST_CASE("a failed model turn names the provider, the status and its cleaned message",
          "[audit][native]") {
    SseProvider provider({SseReply::json(
        400, {{"error",
               {{"message", "Invalid model id. Key sk-or-v1-0123456789abcdef was used.\nmore"},
                {"param", "the meeting transcript"}}}})});
    auto loop = make_loop(provider);
    const auto outcome = loop.run_pass("charter", "prompt", native_tools(), {}, {10, {}}, {});
    CHECK_FALSE(outcome.ok);
    CHECK(outcome.error ==
          "Local test model turn failed (HTTP 400): Invalid model id. Key was used.");
    CHECK(provider.requests().size() == 1);
}

TEST_CASE("a canceled or refused built-in pass returns sidecar.py's values, as installed agents do",
          "[audit][native]") {
    // Refused: shut down, or never initialized.
    Harness harness({probe(), text_turn()});
    harness.core->shutdown();
    CHECK(harness.core->checkpoint({.request_id = "after"}).error == "agent_unavailable");
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto fresh = create_agent_core("pi", deps);
    CHECK(fresh->checkpoint({.request_id = "never"}).error == "agent_unavailable");
    // InstalledAgentCore returns the same values (test_installed_core.cpp: "an unusable agent
    // says why at initialize" and its canceled pass).

    // Canceled during a write: the calls after it are refused and each op counts rejected.
    Harness canceled(
        {probe(),
         tools_turn({call("one", "patch_state", patch(Json::array({kRevision}))),
                     call("two", "patch_state", patch(Json::array({kRevision, set_topic("late")}))),
                     call("three", "search_past_meetings", {{"query", "x"}}),
                     call("four", "resolve_question", {{"question_id", "q_1"}})}),
         text_turn()});
    canceled.tools->on_apply([&](const Json& ops) {
        canceled.core->cancel();
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result =
        canceled.core->checkpoint({.request_id = "c",
                                   .new_segments = {{{"id", "sg_1"}, {"text", "draft"}}},
                                   .is_polish = true});
    CHECK_FALSE(result.ok);
    CHECK(result.error == "canceled");
    // One real write, two ops of the second call, none for the read, one for the question.
    REQUIRE(result.op_results.size() == 4);
    CHECK(result.op_results[0].ok);
    for (std::size_t i = 1; i < result.op_results.size(); ++i) {
        CHECK_FALSE(result.op_results[i].ok);
        CHECK(result.op_results[i].reason == "rejected");
    }
    CHECK(canceled.tools->batches().size() == 1);
    CHECK(canceled.tools->search_calls().empty());
}

TEST_CASE("a timed-out pass returns only its real results", "[audit][native]") {
    // sidecar.py's _checkpoint_failure kept the actual results of a timed-out pass and no
    // counts: the calls the timeout skipped are not tallied.
    fv::FakeClock clock;
    Harness harness({probe(),
                     tools_turn({call("one", "patch_state", patch(Json::array({kRevision}))),
                                 call("two", "patch_state", patch(Json::array({kRevision})))}),
                     text_turn()},
                    "pi", &clock);
    harness.tools->on_apply([&](const Json& ops) {
        clock.advance(181s);
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });
    const auto result =
        harness.core->checkpoint({.request_id = "slow",
                                  .new_segments = {{{"id", "sg_1"}, {"text", "draft"}}},
                                  .is_polish = true});
    CHECK(result.error == "Agent pass timed out after 180s");
    REQUIRE(result.op_results.size() == 1);
    CHECK(result.op_results[0].ok);
}

TEST_CASE("a meeting without a model never starts the built-in agent", "[audit][native]") {
    SseProvider provider({probe()});
    fv::llm::test::TestKeys keys;
    CoreDeps deps{direct_client(), keys.sources};
    auto core = create_agent_core("pi", deps);
    auto config = local_config(provider);
    config.model.clear();
    CHECK_THROWS_WITH(core->initialize(config, std::make_shared<testing::FakeToolHost>()),
                      "no model configured (initialize.model is empty)");
    CHECK(provider.requests().empty());
    CHECK_FALSE(core->is_healthy());
}
