// What the Settings pages tell the app (SettingsPagesSignals, the dialog's settingsChanged) and
// the tabs' Live preview switch, as the controller binds them. Ported from
// tests/test_live_preview_control.py, plus the receivers UIController._prepare_settings_dialog
// sets: the shortcuts saved by their receiver, the font scale and theme with their redraw, and
// the global shortcuts paused while a field captures one.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>

#include <QApplication>
#include <QCheckBox>

#include "controller_support.h"
#include "dictation/streaming_runtime.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "ui_controller/settings_bindings.h"
#include "ui_settings/app_pages.h"
#include "ui_shell/application.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

using fvtest::ControllerRig;
using fvtest::RigOptions;
using fvtest::waitUntil;

namespace {

fv::dictation::StreamingRuntime& streaming(ControllerRig& rig) {
    return rig.context().service<fv::dictation::StreamingRuntime>();
}

fv::SettingsPagesSignals& pageSignals(ControllerRig& rig) {
    return rig.context().service<fv::SettingsPagesSignals>();
}

} // namespace

// from tests/test_live_preview_control.py::test_tab_toggle_reconfigures_streaming
TEST_CASE("test_tab_toggle_reconfigures_streaming", "[test_live_preview_control][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    REQUIRE_FALSE(streaming(rig).configured());

    // The footer's switch saves the setting, then says so; the preview is set up at once.
    rig.settings().save_setting(fv::keys::streaming_enabled.key(), true);
    Q_EMIT tab->livePreviewChanged();

    CHECK(streaming(rig).configured());
    CHECK(streaming(rig).enabled());
}

// from
// tests/test_live_preview_control.py::test_settings_dialog_toggle_refreshes_tabs_then_reconfigures
TEST_CASE("test_settings_dialog_toggle_refreshes_tabs_then_reconfigures",
          "[test_live_preview_control][widget]") {
    ControllerRig rig;
    REQUIRE(rig.startEngine());
    fv::QuickRecordTab* tab = rig.quickRecord();
    REQUIRE(tab != nullptr);
    REQUIRE_FALSE(tab->livePreviewCheck()->isChecked());

    // Recording's Live preview switch saved the setting: the tab shows it, and the preview is
    // set up again.
    rig.settings().save_setting(fv::keys::streaming_enabled.key(), true);
    Q_EMIT pageSignals(rig).streamingSettingsChanged();

    CHECK(tab->livePreviewCheck()->isChecked());
    CHECK(streaming(rig).configured());
}

TEST_CASE("the controller saves the shortcuts the Hotkeys page sends it", "[widget]") {
    ControllerRig rig;
    fv::SettingsPagesSignals& signals = pageSignals(rig);
    REQUIRE(signals.hasHotkeysReceiver());

    Q_EMIT signals.hotkeysChanged(fv::Json{{"record_toggle", "ctrl+alt+r"}});

    const fv::Json saved = fv::load_hotkey_settings(rig.settings());
    CHECK(saved.value("record_toggle", "") == "ctrl+alt+r");
    CHECK(waitUntil([&rig] { return rig.hasStatus("Hotkeys updated"); }));
}

TEST_CASE("the trigger mode and capture suspension reach the hotkey runtime", "[widget]") {
    ControllerRig rig;
    fv::SettingsPagesSignals& signals = pageSignals(rig);

    Q_EMIT signals.recordingTriggerModeChanged(QStringLiteral("push_hold"));
    CHECK(waitUntil([&rig] { return rig.hasStatus("Push-and-hold recording enabled"); }));

    Q_EMIT signals.hotkeyCaptureSuspended(true);
    CHECK(rig.context().service<fv::hotkeys::HotkeyManager>().capture_suspended());
    Q_EMIT signals.hotkeyCaptureSuspended(false);
    CHECK_FALSE(rig.context().service<fv::hotkeys::HotkeyManager>().capture_suspended());
}

TEST_CASE("the font scale redraws the transcripts and a theme change applies the palette",
          "[widget]") {
    ControllerRig rig;
    fv::SettingsPagesSignals& signals = pageSignals(rig);

    Q_EMIT signals.uiFontScaleChanged(125);
    Q_EMIT signals.uiThemeChanged(QStringLiteral("light"));

    CHECK(rig.calls().has("apply_font_scale:125"));
    CHECK(rig.calls().has("apply_theme:light"));
}

// _apply_ui_theme in the app: the palette the Application keeps (and that follows the platform
// colour scheme) is the one switched, not a second manager's.
TEST_CASE("a Settings theme change goes through the application's own theme manager", "[widget]") {
    int argc = 1;
    char program[] = "openwhisper-cpp";
    char* argv[] = {program, nullptr};
    std::unique_ptr<fv::Application> application;
    RigOptions options;
    options.realTheme = true;
    options.beforeFeatures = [&](fv::AppContext& context) {
        application = std::make_unique<fv::Application>(context.settings(), argc, argv);
        context.provide(
            std::shared_ptr<fv::Application>(application.get(), [](fv::Application*) {}));
    };
    {
        ControllerRig rig(options);
        REQUIRE(application != nullptr);
        REQUIRE(application->themeManager().currentTheme() != QStringLiteral("light"));

        Q_EMIT pageSignals(rig).uiThemeChanged(QStringLiteral("light"));

        CHECK(application->themeManager().currentTheme() == QStringLiteral("light"));
        CHECK(fv::ui::currentUiThemePreference() == QStringLiteral("light"));
    }
    qApp->setStyleSheet(QString());
}

TEST_CASE("a saved microphone order reaches the recorder", "[widget]") {
    ControllerRig rig;

    Q_EMIT pageSignals(rig).audioDeviceChanged(fv::Json::array());

    CHECK(waitUntil([&rig] { return rig.hasStatus("Audio device changed"); }));
}
