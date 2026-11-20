// The pass budgets of sidecar.py (live passes 60 s wall and 45 s stall, polish 180 s, the
// consolidation 900 s and 300 s stall) and what happens when one runs out: the pass is
// canceled, its writes kept, and the core stays healthy for the next pass (the sidecar was not
// restarted). The stub sidecar's slow, slow_progress and silent modes become provider scripts.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <format>
#include <future>
#include <string>
#include <vector>

#include "meeting_agent/scheduler.h"
#include "native_support.h"

namespace {

using namespace fv::meeting_agent;
using namespace fv::meeting_agent::native_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;
using fv::meeting_state::OpResult;

struct Budget {
    std::string name;
    CheckpointPayload payload;
    int wall_s;
    int stall_s;
};

TEST_CASE("each pass kind runs on its own wall and stall budget", "[native]") {
    const auto budget = GENERATE(
        Budget{"cards", {.request_id = "cards"}, 60, 45},
        Budget{"notes", {.request_id = "notes", .is_notes = true}, 60, 45},
        Budget{"polish", {.request_id = "polish", .is_polish = true}, 180, 45},
        Budget{"consolidation", {.request_id = "final", .is_consolidation = true}, 900, 300});
    const auto kind = GENERATE(std::string("pi"), std::string("opencode"));
    CAPTURE(budget.name, kind);
    // A write the pass may make, so that it reaches the host.
    const Json op = budget.payload.is_polish  ? Json{{"op", "revise_segment_text"},
                                                     {"segment_id", "sg_1"},
                                                     {"text", "Budget."},
                                                     {"evidence", Json::array({"sg_1"})}}
                    : budget.payload.is_notes ? Json{{"op", "add_item"},
                                                     {"card", "live_notes"},
                                                     {"text", "Budget."},
                                                     {"evidence", Json::array({"sg_1"})}}
                                              : set_topic("Budget");
    const Json write = patch(Json::array({op}));
    fv::FakeClock clock;
    Harness harness({probe(), tools_turn({call("a", "patch_state", write)}), text_turn(),
                     tools_turn({call("b", "patch_state", write)}), silent(), text_turn()},
                    kind, &clock);
    std::chrono::seconds work{budget.wall_s - 1};
    harness.tools->on_apply([&](const Json& ops) {
        clock.advance(work);
        return std::vector<OpResult>{{.ok = true, .op = ops[0]}};
    });

    // A write that takes all but a second of the wall budget still finishes the pass. For the
    // consolidation that is far past any live pass's limit.
    const auto within = harness.core->checkpoint(budget.payload);
    INFO(within.error);
    CHECK(within.ok);

    // One that outlasts it ends the pass with its write kept.
    work = std::chrono::seconds(budget.wall_s + 1);
    const auto over = harness.core->checkpoint(budget.payload);
    CHECK_FALSE(over.ok);
    CHECK(over.error == std::format("Agent pass timed out after {}s", budget.wall_s));
    CHECK(over.op_results.size() == 1);
    CHECK(harness.core->is_healthy());

    // A silent provider is canceled at the stall budget, not before.
    auto stalled =
        std::async(std::launch::async, [&] { return harness.core->checkpoint(budget.payload); });
    CancelOnExit stopStalled{*harness.core};
    REQUIRE(await_requests(harness.provider, 5));
    clock.advance(std::chrono::seconds(budget.stall_s - 1));
    CHECK_FALSE(finishes(stalled, 200ms));
    clock.advance(2s);
    REQUIRE(finishes(stalled, 3000ms));
    const auto silent_result = stalled.get();
    CHECK_FALSE(silent_result.ok);
    CHECK(silent_result.error ==
          std::format("Agent pass stalled after {}s without agent progress", budget.stall_s));

    // Neither failure restarted anything: the same core runs the next pass.
    CHECK(harness.core->is_healthy());
    CHECK(harness.core->checkpoint({.request_id = "next"}).ok);
}

TEST_CASE("the built-in budgets are sidecar.py's", "[native]") {
    CHECK(FINAL_POLISH_TIMEOUT_S == 180.0);
    CHECK(CONSOLIDATION_TIMEOUT_CAP_S == 900.0);
}

TEST_CASE("a stream that keeps thinking outlives the stall budget", "[native]") {
    // slow_progress: the reply takes longer than the stall budget but never goes quiet for that
    // long, so it finishes inside the wall budget.
    auto slow = thinking_turn(8);
    slow.piece_delay = 150ms;
    SseProvider provider({slow});
    const auto endpoint = local_profile(provider);
    AgentLoop loop(direct_client(), endpoint, "local-model",
                   fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", fv::system_clock());
    const auto outcome =
        loop.run_pass("charter", "prompt", meeting_tools(), {}, {10, 0.6}, fv::CancelToken{});
    INFO(outcome.error);
    CHECK(outcome.ok);
}

TEST_CASE("progress keeps alive only the pass it belongs to", "[native]") {
    // Two passes at once, each with its own clock: one thinking steadily, one silent. The
    // neighbour's thinking does not hold the silent one open.
    auto slow = thinking_turn(8);
    slow.piece_delay = 150ms;
    SseProvider busy({slow});
    SseProvider quiet({silent()});
    const auto busyProfile = local_profile(busy);
    const auto quietProfile = local_profile(quiet);
    AgentLoop busyLoop(direct_client(), busyProfile, "local-model",
                       fv::llm::model_spec(busyProfile, "local-model"), {}, "dummy",
                       fv::system_clock());
    AgentLoop quietLoop(direct_client(), quietProfile, "local-model",
                        fv::llm::model_spec(quietProfile, "local-model"), {}, "dummy",
                        fv::system_clock());
    auto thinking = std::async(std::launch::async, [&] {
        return busyLoop.run_pass("charter", "prompt", meeting_tools(), {}, {10, 0.6}, {});
    });
    const auto stalled =
        quietLoop.run_pass("charter", "prompt", meeting_tools(), {}, {10, 0.6}, {});
    CHECK_FALSE(stalled.ok);
    CHECK_THAT(stalled.error, ContainsSubstring("stalled"));
    CHECK(thinking.get().ok);
}

TEST_CASE("a reply that arrives after the wall budget is not a success", "[native]") {
    // slow: the provider answers, but only after the host gave up; the late reply is dropped
    // and the next pass is unaffected.
    auto late = text_turn("Too late.");
    late.piece_delay = 400ms;
    SseProvider provider({late, text_turn()});
    const auto endpoint = local_profile(provider);
    AgentLoop loop(direct_client(), endpoint, "local-model",
                   fv::llm::model_spec(endpoint, "local-model"), {}, "dummy", fv::system_clock());
    const auto started = std::chrono::steady_clock::now();
    const auto outcome = loop.run_pass("charter", "prompt", meeting_tools(), {}, {0.5, {}}, {});
    CHECK(std::chrono::steady_clock::now() - started < 1500ms);
    CHECK_FALSE(outcome.ok);
    CHECK_FALSE(outcome.canceled);
    CHECK_THAT(outcome.error, ContainsSubstring("timed out"));
    CHECK(loop.run_pass("charter", "next", meeting_tools(), {}, {5, {}}, {}).ok);
}

TEST_CASE("a broken transfer fails that pass only", "[native]") {
    // sidecar.py restarted the sidecar after a transport error; with no process, the pass
    // fails (after the provider retries) and the core carries on.
    std::vector<SseReply> replies{probe()};
    for (int i = 0; i < 4; ++i) {
        auto dropped = text_turn();
        dropped.drop_after = 1;
        dropped.headers.emplace_back("Retry-After-Ms", "1");
        replies.push_back(dropped);
    }
    replies.push_back(text_turn());
    Harness harness(std::move(replies));
    const auto failed = harness.core->checkpoint({.request_id = "req-dead"});
    CHECK_FALSE(failed.ok);
    CHECK(failed.error == fv::llm::kStreamDroppedError);
    CHECK(harness.provider.requests().size() == 5);
    CHECK(harness.core->is_healthy());
    CHECK(harness.core->checkpoint({.request_id = "req-next"}).ok);
}

} // namespace
