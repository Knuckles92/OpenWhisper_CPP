// test_automation_fix2_cancel.py::test_closing_settings_cancels_its_try_it_runs_quietly: the
// Settings window's close cancels its Try it runs (ActionsPage::cancelTryRuns, through the
// context's ActionTester, the runtime's cancel_tests) without a status or a notice: the page that
// asked is gone.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <vector>

#include "automation/action_runtime.h"
#include "support/action_support.h"
#include "support/hook_support.h"

using namespace fv;
using namespace fv::automation;
using namespace fv::automation::test;
using namespace std::chrono_literals;

TEST_CASE("test_closing_settings_cancels_its_try_it_runs_quietly", "[unit]") {
    Harness h;
    BlockedAsk ask;
    h.ask.set([&ask](const std::string&, const std::string& text, const std::string&,
                     const CancelToken& cancel) { return ask(text, cancel); });
    const Action quick =
        an_action("q1", "Quick answer", ActionKind::AGENT,
                  Json{{"agent", "claude_code"}, {"mode", "ask"}, {"output", "notice"}});
    h.add(quick);
    std::vector<ActionResult> results;
    h.rt->test(quick, "sample", [&](ActionResult r) { results.push_back(std::move(r)); });
    REQUIRE(wait_until([&] { return ask.started(1); }, 5s));

    CHECK(h.rt->cancel_tests() == 1);

    REQUIRE(h.pump([&] { return !results.empty(); }));
    CHECK(results ==
          std::vector<ActionResult>{result(false, std::string(runtime::CANCELED_MESSAGE))});
    CHECK(h.host.statuses.empty());
    CHECK(h.host.notices.empty());
    ask.release();
}
