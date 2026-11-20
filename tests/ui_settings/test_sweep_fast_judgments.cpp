// Sweep S2 (open-items A31): the Fast judgments Test button reads the route's key as
// settings_fast_judgments.py's test_route does on every press: a missing key is reported without
// a "Testing…" line first, and a key put in the environment while Settings is open is tested.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "fast_judgments_support.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

TEST_CASE("a Test before the keys are read says nothing until the store has answered", "[widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    // Built, its keys not read yet.
    auto& page = scene.rig().page<fv::FastJudgmentsPage>(dest::meeting_fast);
    REQUIRE_FALSE(page.stateCurrent());
    page.routeTestButton()->click();
    CHECK(page.routeStatus()->text() != QStringLiteral("Testing OpenRouter…"));
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    CHECK(scene.verifiedNow().empty());
    CHECK(page.routeStatus()->text() ==
          QStringLiteral("No OpenRouter API key to test. Add one under API keys → OpenRouter."));
    CHECK(page.routeStatus()->property("tone").toString() == QStringLiteral("warning"));
    CHECK(page.routeTestButton()->isEnabled());
}

TEST_CASE("a key put in the environment while Settings is open is tested at once", "[widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}, {"typesafe_enabled", true}});
    scene.answer(true, "openrouter.ai accepted the key and typesafe/jev-1.13 answered in 0.15 s.");
    auto& page = scene.page();
    REQUIRE_FALSE(page.keyNotice()->isHidden());
    scene.rig().env().set(kOpenRouterEnv, kOpenRouterKey);
    page.routeTestButton()->click();
    CHECK(page.routeStatus()->text() == QStringLiteral("Testing OpenRouter…"));
    REQUIRE(pumpUntil([&page] { return !page.testing(); }));
    const std::vector<Verified> asked = scene.verifiedNow();
    REQUIRE(asked.size() == 1);
    CHECK(asked[0].route == "openrouter");
    CHECK(asked[0].key == kOpenRouterKey);
    CHECK(page.routeStatus()->text().contains(QStringLiteral("typesafe/jev-1.13 answered")));
    // The keys were read again, so the notice no longer says the key is missing.
    CHECK(pumpUntil([&page] { return page.stateCurrent() && page.keyNotice()->isHidden(); }));
}

TEST_CASE("a Test with no key anywhere still says so at once, without a request", "[widget]") {
    FastJudgmentsRig scene(Json{{"typesafe_provider", "openrouter"}});
    auto& page = scene.page();
    // Set but empty: not a key, as loading .env never overrides a variable that is set.
    scene.rig().env().set(kOpenRouterEnv, "");
    page.routeTestButton()->click();
    CHECK_FALSE(page.testing());
    CHECK(page.routeStatus()->text() ==
          QStringLiteral("No OpenRouter API key to test. Add one under API keys → OpenRouter."));
    fvtest::processEvents(3);
    CHECK(scene.verifiedNow().empty());
}
