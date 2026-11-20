#include "automation/action_runtime.h"
#include "support/action_support.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("approved action keeps the disclosed destination when settings change", "[automation]") {
    using namespace fv;
    using namespace fv::automation;
    using namespace fv::automation::test;
    Harness harness;
    auto approved = an_action("a1", "Copy", ActionKind::CLIPBOARD);
    harness.add(approved);
    auto snapshot = harness.settings.snapshot();
    harness.set("actions", Json::array({an_action("a1", "Send", ActionKind::SCRIPTS).to_dict()}));
    harness.rt->deliver_approved(action_job("a1"), approved, snapshot, "approved words", "", "",
                                 "");
    REQUIRE(harness.host.copied.size() == 1);
    CHECK(harness.host.copied.front().first == "approved words");
    CHECK_THROWS(
        harness.rt->deliver_approved(action_job("other"), approved, snapshot, "x", "", "", ""));
}
