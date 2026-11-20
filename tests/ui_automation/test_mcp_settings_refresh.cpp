// An MCP settings change refreshes the open Settings window from the saved values, besides the
// desktop hooks test_session_mcp.cpp checks (tests/test_mcp_desktop_changes.py).
#include <thread>

#include <QApplication>
#include <QCheckBox>

#include "actions_page_support.h"
#include "ui_automation/session_mcp.h"
#include "ui_settings/settings_binder.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include <catch2/catch_test_macros.hpp>

TEST_CASE("MCP settings changes refresh the open Settings window", "[widget]") {
    // from
    // tests/test_mcp_desktop_changes.py::test_desktop_hooks_refresh_preview_cleanup_trigger_and_open_settings
    // (its _settings_dialog.refresh half)
    fvtest::ActionsRig rig(fv::Json{{"transcript_cleanup_enabled", true}});
    fv::SettingsDialog& dialog = rig.context().service<fv::SettingsWindow>().ensureDialog();
    QCheckBox check;
    dialog.bindings().checkbox(&check, "transcript_cleanup_enabled", [](const fv::Json& saved) {
        return saved.value("transcript_cleanup_enabled", false);
    });
    dialog.refresh();
    REQUIRE(check.isChecked());

    const auto notify = fv::sessionMcpDesktopChanges(rig.context());
    const fv::Json changes{{"transcript_cleanup_enabled", false}};
    rig.settings().update(changes);
    std::thread worker([&] { notify("settings", changes); });
    worker.join();
    CHECK(check.isChecked());
    QApplication::processEvents();
    CHECK_FALSE(check.isChecked());
}
