// What an agent changed over MCP reaches the desktop through the controller's graph
// (ui_controller.py _apply_agent_changes; test_personalize_s2_mcp.py): the MCP listener's
// desktop-change handler, built over the context the app composes, refreshes the AI cleanup
// controls and the Settings window.
#include <catch2/catch_test_macros.hpp>

#include "automation/control.h"
#include "controller_support.h"
#include "personalize_support.h"
#include "settings/keys.h"
#include "ui_automation/session_mcp.h"
#include "ui_record/quick_record_tab.h"
#include "ui_settings/settings_window.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;
using Json = fv::Json;

TEST_CASE("test_a_level_change_from_an_agent_refreshes_the_cleanup_controls",
          "[widget][personalize]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    // The Settings window exists, as when the user has opened it; it is refreshed in place.
    (void)fvtest::settingsDialog(rig);
    // The agent's mutation was committed by the listener: the store holds it, nothing on the
    // desktop has heard of it yet.
    rig.settings().save_setting(fv::keys::transcript_cleanup_enabled.key(), Json(true));
    rig.settings().save_setting(fv::keys::transcript_cleanup_level.key(), Json("light"));
    CHECK_FALSE(tab->cleanupCheck()->isChecked());

    const auto notify = fv::sessionMcpDesktopChanges(rig.context());
    notify("settings", Json{{fv::keys::transcript_cleanup_level.key(), "light"}});
    // refresh_cleanup_controls: Quick Record reloads the saved cleanup.
    CHECK(waitUntil([&] { return tab->cleanupCheck()->isChecked(); }));
}

// Difference a19-live-api-not-served: the composed app registers no AppControl for the live API,
// so nothing outside the app can start a recording or run an action through it.
TEST_CASE("the running app registers no live API control", "[widget]") {
    fvtest::MemoryKeys keys;
    ControllerRig rig(fvtest::personalizeOptions(keys, nullptr));
    CHECK(fv::automation::app_control() == nullptr);
}
