// Settings' App pages and Recording as registered pages: General's tiles and update gating, the
// card reflow and tile fit (tests/test_settings_general_layout.py), Appearance's look, layout and
// label switches (tests/test_recording_look.py), the theme and font size combos
// (tests/test_ui_theme.py, tests/test_ui_font_scale.py), Developer options, the Hotkeys page's
// host and the window's hotkey routes (tests/test_hotkey_settings_page.py).
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QTest>

#include "app_pages_support.h"
#include "hotkeys/testing/fake_capture.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_overlay/recording_look_preview.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_settings/appearance_page.h"
#include "ui_settings/developer_page.h"
#include "ui_settings/general_page.h"
#include "ui_settings/recording_page.h"
#include "ui_settings/settings_window.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

// _dialog / make_dialog: the window shown at the size the Python test gives it.
void showAt(fv::SettingsDialog& dialog, int width, int height) {
    dialog.resize(width, height);
    dialog.show();
    QApplication::processEvents();
}

// _open_appearance.
fv::AppearancePage& openAppearance(AppPagesRig& rig) {
    auto& page = rig.page<fv::AppearancePage>(dest::appearance);
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
    }
    return page;
}

int lookIndex(const char* look) {
    int index = 0;
    for (const std::string_view value : fv::keys::choices::recording_overlay_look) {
        if (value == look) {
            return index;
        }
        ++index;
    }
    return -1;
}

} // namespace

TEST_CASE("the App pages and Recording register with their rail values", "[widget]") {
    AppPagesRig rig({{"auto_paste", true},
                     {"recording_overlay_look", "bars"},
                     {"developer_mode", true},
                     {"audio_input_priority",
                      Json::array({entry("Microphone (Desk)"), entry("Microphone (Laptop)"),
                                   entry("Microphone (Webcam)")})}});
    fv::SettingsDialog& dialog = rig.dialog();
    for (const std::string_view key : {dest::recording, dest::general, dest::appearance,
                                       dest::hotkeys, dest::api_keys, dest::advanced}) {
        INFO(key);
        CHECK(dialog.isRegistered(key));
    }
    CHECK(dialog.rail()->value(qs(dest::general)) == QStringLiteral("Auto-paste on"));
    CHECK(dialog.rail()->value(qs(dest::appearance)) == QStringLiteral(u"Dark · Bars"));
    CHECK(dialog.rail()->value(qs(dest::recording)) == QStringLiteral(u"Desk → Laptop +1"));
    CHECK(dialog.rail()->value(qs(dest::advanced)) == QStringLiteral("Developer mode on"));
    CHECK(dialog.rail()->value(qs(dest::hotkeys)) ==
          fv::HotkeysPage::hotkeysRailValue(Json::object()));
    REQUIRE(pumpUntil(
        [&] { return dialog.rail()->value(qs(dest::api_keys)) == QStringLiteral("0 of 6 set"); }));
    // Search and deep links reach the controls before their pages are built.
    CHECK(rig.context().settingsPages().controlDestination("max_recordings_spinbox") ==
          dest::recording);
    CHECK(rig.context().settingsPages().controlDestination("developer_mode_check") ==
          dest::advanced);
    CHECK(dialog.control("update_check_check") != nullptr);
    CHECK(dialog.isBuilt(dest::general));
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_general_tiles_toggle_and_gate_update_notify
TEST_CASE("test_general_tiles_toggle_and_gate_update_notify",
          "[test_settings_general_layout][widget]") {
    AppPagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.resize(fv::SettingsDialog::kDefaultSize);
    dialog.ensureAllPages();
    dialog.show();
    QApplication::processEvents();
    dialog.rail()->select(qs(dest::general));
    QApplication::processEvents();
    auto& page = *dynamic_cast<fv::GeneralPage*>(dialog.pageContent(dest::general));
    fv::SettingTile* tile = page.copyClipboardTile();
    const bool before = tile->checkbox()->isChecked();
    QTest::mouseClick(tile, Qt::LeftButton, {}, QPoint(8, 8));
    CHECK(tile->checkbox()->isChecked() == !before);
    CHECK(tile->property("checked").toBool() == !before);

    page.updateCheckTile()->checkbox()->setChecked(true);
    CHECK(page.updateNotifyTile()->checkbox()->isEnabled());
    page.updateCheckTile()->checkbox()->setChecked(false);
    CHECK_FALSE(page.updateNotifyTile()->isEnabled());
    CHECK_FALSE(page.updateNotifyTile()->checkbox()->isEnabled());
    dialog.close();
}

TEST_CASE("the update switches save and tell the app", "[widget]") {
    AppPagesRig rig;
    QStringList kinds;
    int updates = 0;
    QObject::connect(&rig.dialog(), &fv::SettingsDialog::settingsChanged,
                     [&kinds](const QString& kind) { kinds.append(kind); });
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::updatesChanged,
                     [&updates] { ++updates; });
    auto& page = rig.page<fv::GeneralPage>(dest::general);
    CHECK(kinds.isEmpty());

    page.updateCheckTile()->checkbox()->setChecked(false);
    CHECK(rig.saved("update_check_enabled") == false);
    page.updateBetaTile()->checkbox()->setChecked(true);
    CHECK(rig.saved("update_beta_track_enabled") == true);
    page.updateNotifyTile()->checkbox()->setChecked(false);
    CHECK(rig.saved("update_notify_enabled") == false);
    page.updateRestartPromptTile()->checkbox()->setChecked(false);
    CHECK(rig.saved("update_restart_prompt_enabled") == false);
    CHECK(kinds == QStringList{QStringLiteral("updates"), QStringLiteral("updates"),
                               QStringLiteral("updates")});
    CHECK(updates == 3);
}

TEST_CASE("without a system tray the tray tile is off and says why", "[widget]") {
    AppPagesRig rig({{"minimize_tray", true}});
    rig.trayAvailable = false;
    auto& page = rig.page<fv::GeneralPage>(dest::general);
    CHECK_FALSE(page.minimizeTrayTile()->isEnabled());
    CHECK_FALSE(page.minimizeTrayTile()->checkbox()->isChecked());
    CHECK(page.minimizeTrayTile()->descriptionLabel()->text() ==
          QStringLiteral("This desktop session does not provide a system tray."));
    CHECK(rig.saved("minimize_tray") == true);
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_cards_reflow_when_the_window_narrows
TEST_CASE("test_cards_reflow_when_the_window_narrows", "[test_settings_general_layout][widget]") {
    AppPagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.resize(fv::SettingsDialog::kDefaultSize);
    dialog.ensureAllPages();
    dialog.show();
    // The window floor keeps two columns; the reflow itself is what this test pins down, so let
    // the body get narrow enough for one.
    dialog.setMinimumSize(0, 0);
    dialog.selectDestination(qs(dest::general));
    auto& page = *dynamic_cast<fv::GeneralPage*>(dialog.pageContent(dest::general));
    for (const int width : {1200, 800, 1200}) {
        dialog.resize(width, 600);
        for (int i = 0; i < 8; ++i) {
            QApplication::processEvents();
        }
        QWidget* left = page.autoPasteTile();
        QWidget* right = page.restoreClipboardTile();
        INFO(width);
        if (width == 800) {
            CHECK(left->x() == right->x());
            CHECK(right->y() > left->geometry().bottom());
        } else {
            CHECK(left->y() == right->y());
            CHECK(right->x() > left->geometry().right());
        }
    }
    dialog.close();
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_tile_titles_and_descriptions_fit_after_font_and_width_changes
TEST_CASE("test_tile_titles_and_descriptions_fit_after_font_and_width_changes",
          "[test_settings_general_layout][widget]") {
    AppTheme theme;
    fv::ui::ThemeManager manager(QStringLiteral("dark"));
    AppPagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.resize(fv::SettingsDialog::kDefaultSize);
    dialog.ensureAllPages();
    dialog.show();
    for (const int scale : {100, 115, 130, 100}) {
        fv::ui::applyUiFontScale(scale, *qApp, manager);
        // 800 is clamped to the 940 px minimum: the narrowest layout.
        for (const int width : {800, 1100}) {
            dialog.resize(width, 560);
            for (const QString& key : dialog.rail()->keys()) {
                dialog.rail()->select(key);
                for (int i = 0; i < 8; ++i) {
                    QApplication::processEvents();
                }
                QWidget* page = dialog.page(fv::qt::toUtf8(key));
                QList<QWidget*> controls;
                for (QAbstractButton* control : page->findChildren<QAbstractButton*>()) {
                    controls.append(control);
                }
                for (QComboBox* control : page->findChildren<QComboBox*>()) {
                    controls.append(control);
                }
                for (QLineEdit* control : page->findChildren<QLineEdit*>()) {
                    controls.append(control);
                }
                for (QWidget* control : controls) {
                    if (!control->isVisible()) {
                        continue;
                    }
                    INFO(scale << " " << width << " " << key.toStdString() << " "
                               << control->objectName().toStdString());
                    CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
                    CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
                }
                for (fv::TileBase* tile : page->findChildren<fv::TileBase*>()) {
                    for (fv::WrappedLabel* label : {tile->titleLabel(), tile->descriptionLabel()}) {
                        if (!label->isVisible()) {
                            continue;
                        }
                        INFO(scale << " " << width << " " << key.toStdString() << " "
                                   << tile->titleLabel()->text().toStdString());
                        CHECK(label->wordWrap());
                        CHECK(label->QLabel::text() == label->text());
                        CHECK(label->height() >= label->heightForWidth(label->width()));
                        CHECK(
                            tile->rect().contains(label->mapTo(tile, label->rect().bottomRight())));
                        CHECK(page->rect().contains(tile->geometry().translated(
                            tile->parentWidget()->mapTo(page, QPoint(0, 0)))));
                    }
                }
            }
        }
    }
    fv::ui::applyUiFontScale(100, *qApp, manager);
    dialog.close();
}

// from tests/test_recording_look.py::test_settings_loads_saves_and_pushes_the_look
TEST_CASE("test_settings_loads_saves_and_pushes_the_look", "[test_recording_look][widget]") {
    AppPagesRig rig({{"recording_overlay_look", "legacy"}});
    QStringList looks;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::recordingLookChanged,
                     [&looks](const QString& look) { looks.append(look); });
    showAt(rig.dialog(), 1100, 820);
    auto& page = openAppearance(rig);
    CHECK(page.lookBar()->currentIndex() == lookIndex("legacy"));
    CHECK(page.lookPreview()->overlay().recordingLook() == QStringLiteral("legacy"));
    CHECK(looks.isEmpty());
    // Classic has no live label.
    CHECK_FALSE(page.parts()->isEnabled());

    QTest::mouseClick(page.lookBar()->buttons()[static_cast<std::size_t>(lookIndex("glow"))],
                      Qt::LeftButton);
    CHECK(rig.saved("recording_overlay_look") == "glow");
    CHECK(page.lookPreview()->overlay().recordingLook() == QStringLiteral("glow"));
    CHECK(page.parts()->isEnabled());
    CHECK(looks == QStringList{QStringLiteral("glow")});

    page.dotSwitch()->click();
    CHECK(rig.saved("recording_overlay_dot") == false);
    const auto parts = page.lookPreview()->overlay().labelParts();
    CHECK(parts == fv::WaveformOverlay::LabelParts{false, true, true});
    CHECK(looks.size() == 2);
    rig.dialog().hide();
}

// from tests/test_recording_look.py::test_settings_loads_saves_and_pushes_the_preview_layout
TEST_CASE("test_settings_loads_saves_and_pushes_the_preview_layout",
          "[test_recording_look][widget]") {
    AppPagesRig rig({{"streaming_overlay_layout", "footer"}});
    QStringList layouts;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::streamingLayoutChanged,
                     [&layouts](const QString& layout) { layouts.append(layout); });
    showAt(rig.dialog(), 1100, 820);
    auto& page = openAppearance(rig);
    CHECK(page.layoutBar()->currentIndex() == 1);
    CHECK(page.layoutTile()->isVisible());
    CHECK(layouts.isEmpty());

    QTest::mouseClick(page.layoutBar()->buttons()[0], Qt::LeftButton);
    CHECK(rig.saved("streaming_overlay_layout") == "header");
    CHECK(layouts == QStringList{QStringLiteral("header")});
    rig.dialog().hide();
}

// from tests/test_recording_look.py::test_settings_loads_the_label_switches
TEST_CASE("test_settings_loads_the_label_switches", "[test_recording_look][widget]") {
    AppPagesRig rig({{"recording_overlay_text", false}, {"recording_overlay_clock", false}});
    int changes = 0;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::recordingLookChanged,
                     [&changes] { ++changes; });
    showAt(rig.dialog(), 1100, 820);
    auto& page = openAppearance(rig);
    CHECK(page.dotSwitch()->isChecked());
    CHECK_FALSE(page.textSwitch()->isChecked());
    CHECK_FALSE(page.clockSwitch()->isChecked());
    CHECK(page.lookPreview()->overlay().labelParts() ==
          fv::WaveformOverlay::LabelParts{true, false, false});
    CHECK(changes == 0);
    rig.dialog().hide();
}

// from
// tests/test_ui_theme.py::TestSettingsThemeControl::test_combo_lists_every_theme_and_persists_the_choice
TEST_CASE("test_combo_lists_every_theme_and_persists_the_choice", "[test_ui_theme][widget]") {
    AppPagesRig rig({{"ui_theme", "dark"}});
    auto& page = rig.page<fv::AppearancePage>(dest::appearance);
    QComboBox* combo = page.themeCombo();
    QStringList data;
    QStringList texts;
    for (int i = 0; i < combo->count(); ++i) {
        data.append(combo->itemData(i).toString());
        texts.append(combo->itemText(i));
    }
    CHECK(data == QStringList{QStringLiteral("dark"), QStringLiteral("light"),
                              QStringLiteral("system"), QStringLiteral("omarchy")});
    CHECK(texts == QStringList{QStringLiteral("Dark"), QStringLiteral("Light"),
                               QStringLiteral("Match system"), QStringLiteral("Omarchy desktop")});
    QStringList applied;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::uiThemeChanged,
                     [&applied](const QString& theme) { applied.append(theme); });
    combo->setCurrentIndex(combo->findData(QStringLiteral("light")));
    CHECK(rig.saved("ui_theme") == "light");
    CHECK(applied == QStringList{QStringLiteral("light")});
}

// from
// tests/test_ui_font_scale.py::TestSettingsFontScaleControl::test_combo_persists_the_chosen_percent
TEST_CASE("test_combo_persists_the_chosen_percent", "[test_ui_font_scale][widget]") {
    AppPagesRig rig({{"ui_font_scale", 100}});
    auto& page = rig.page<fv::AppearancePage>(dest::appearance);
    std::vector<int> applied;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::uiFontScaleChanged,
                     [&applied](int percent) { applied.push_back(percent); });
    page.fontScaleCombo()->setCurrentIndex(page.fontScaleCombo()->findData(115));
    CHECK(rig.saved("ui_font_scale") == 115);
    CHECK(applied == std::vector<int>{115});
}

TEST_CASE("loading the Appearance page saves nothing and tells the app nothing", "[widget]") {
    AppPagesRig rig({{"ui_theme", "light"},
                     {"ui_font_scale", 130},
                     {"streaming_overlay_font_size", 22},
                     {"recording_overlay_look", "orb"}});
    int told = 0;
    for (auto signal : {&fv::SettingsPagesSignals::uiThemeChanged,
                        &fv::SettingsPagesSignals::recordingLookChanged,
                        &fv::SettingsPagesSignals::streamingLayoutChanged}) {
        QObject::connect(&rig.pages().signals(), signal, [&told] { ++told; });
    }
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::uiFontScaleChanged,
                     [&told] { ++told; });
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::streamingFontChanged,
                     [&told] { ++told; });
    const Json before = *rig.settings().snapshot();
    auto& page = rig.page<fv::AppearancePage>(dest::appearance);
    CHECK(page.themeCombo()->currentData().toString() == QStringLiteral("light"));
    CHECK(page.fontScaleCombo()->currentData().toInt() == 130);
    CHECK(page.fontSizeSpin()->value() == 22);
    // An earlier build's name reads as its successor.
    CHECK(page.lookBar()->currentIndex() == lookIndex("pulse"));
    rig.dialog().refresh();
    CHECK(*rig.settings().snapshot() == before);
    CHECK(told == 0);

    page.fontSizeSpin()->setValue(30);
    CHECK(rig.saved("streaming_overlay_font_size") == 30);
    CHECK(told == 1);
}

TEST_CASE("the live preview switch drops the legacy streaming keys", "[widget]") {
    AppPagesRig rig({{"streaming_overlay_enabled", true}, {"live_typing_enabled", true}});
    int told = 0;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::streamingSettingsChanged,
                     [&told] { ++told; });
    auto& page = rig.page<fv::RecordingPage>(dest::recording);
    CHECK_FALSE(page.streamingCheck()->isChecked());
    page.streamingCheck()->setChecked(true);
    CHECK(rig.saved("streaming_enabled") == true);
    CHECK_FALSE(rig.has("streaming_overlay_enabled"));
    CHECK_FALSE(rig.has("live_typing_enabled"));
    CHECK(told == 1);
}

TEST_CASE("Developer mode saves and tells the app", "[widget]") {
    AppPagesRig rig;
    std::vector<bool> told;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::developerModeChanged,
                     [&told](bool on) { told.push_back(on); });
    auto& page = rig.page<fv::DeveloperPage>(dest::advanced);
    CHECK_FALSE(page.developerModeTile()->checkbox()->isChecked());
    CHECK(page.developerModeTile()->checkbox()->objectName() ==
          QStringLiteral("developerModeCheck"));
    page.developerModeTile()->checkbox()->setChecked(true);
    CHECK(rig.saved("developer_mode") == true);
    CHECK(told == std::vector<bool>{true});
    CHECK(rig.dialog().rail()->value(qs(dest::advanced)) == QStringLiteral("Developer mode on"));
}

TEST_CASE("the Hotkeys page saves standard shortcuts through the window", "[widget]") {
    AppPagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK_FALSE(dialog.isBuilt(dest::hotkeys));
    const Json defaults = fv::default_hotkeys();
    const std::string record = defaults["record_toggle"].get<std::string>();

    // A shortcut another action holds is refused, with the shell's reason, and nothing saved.
    const QString refused =
        dialog.setStandardHotkey(QStringLiteral("command_mode"), QString::fromStdString(record));
    CHECK(dialog.isBuilt(dest::hotkeys));
    CHECK(refused.startsWith(QStringLiteral("That shortcut is already used by ")));
    CHECK(dialog.messageText() == refused);
    CHECK_FALSE(rig.has("hotkeys"));

    // Without a receiver the host saves the table itself; the rail follows the page.
    CHECK(dialog.setStandardHotkey(QStringLiteral("record_toggle"), QStringLiteral("ctrl+alt+f9"))
              .isEmpty());
    CHECK(rig.saved("hotkeys")["record_toggle"] == "ctrl+alt+f9");
    CHECK(dialog.messageText().endsWith(QStringLiteral(" hotkey updated.")));
    CHECK(dialog.rail()->value(qs(dest::hotkeys)) ==
          fv::HotkeysPage::hotkeysRailValue({{"hotkeys", {{"record_toggle", "ctrl+alt+f9"}}}}));

    // With a receiver, the receiver saves (the controller registers the shortcuts too).
    std::vector<Json> received;
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::hotkeysChanged,
                     [&received](const Json& hotkeys) { received.push_back(hotkeys); });
    CHECK(dialog.setStandardHotkey(QStringLiteral("record_toggle"), QStringLiteral("ctrl+alt+f10"))
              .isEmpty());
    REQUIRE(received.size() == 1);
    CHECK(received[0]["record_toggle"] == "ctrl+alt+f10");
    CHECK(rig.saved("hotkeys")["record_toggle"] == "ctrl+alt+f9");
}

TEST_CASE("the Hotkeys page's capture suspension and trigger mode reach the app", "[widget]") {
    AppPagesRig rig;
    std::vector<bool> suspended;
    QStringList modes;
    QObject::connect(&rig.dialog(), &fv::SettingsDialog::hotkeyCaptureSuspended,
                     [&suspended](bool on) { suspended.push_back(on); });
    QObject::connect(&rig.pages().signals(), &fv::SettingsPagesSignals::recordingTriggerModeChanged,
                     [&modes](const QString& mode) { modes.append(mode); });
    auto& page = rig.page<fv::HotkeysPage>(dest::hotkeys);
    page.setHotkeyCaptureSuspended(true);
    page.setHotkeyCaptureSuspended(false);
    CHECK(suspended == std::vector<bool>{true, false});
    QComboBox* mode = page.recordModeCombo();
    mode->setCurrentIndex(mode->findData(QStringLiteral("push_hold")));
    CHECK(rig.saved("recording_trigger_mode") == "push_hold");
    CHECK(modes == QStringList{QStringLiteral("push_hold")});
}

TEST_CASE("Esc stops a shortcut capture and keeps Settings open", "[widget]") {
    // settings_dialog.py keyPressEvent: Esc while capturing cancels the capture only.
    AppPagesRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    auto& page = rig.page<fv::HotkeysPage>(dest::hotkeys);
    fv::hotkeys::FakeCaptureFactory captures;
    page.setCaptureFactory(captures.factory());
    dialog.show();
    QApplication::processEvents();

    QTest::mouseClick(page.hotkeyInput("record_toggle"), Qt::LeftButton);
    REQUIRE(page.capturingKey() == "record_toggle");
    CHECK(dialog.hasEscapeOverride());
    QTest::keyClick(&dialog, Qt::Key_Escape);
    CHECK_FALSE(page.capturingKey().has_value());
    REQUIRE(captures.last() != nullptr);
    CHECK(captures.last()->stopped());
    CHECK(dialog.isVisible());
    CHECK_FALSE(dialog.hasEscapeOverride());

    QTest::keyClick(&dialog, Qt::Key_Escape);
    CHECK_FALSE(dialog.isVisible());
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsNavigation::test_file_hotkeys_deep_links_to_settings_destination
TEST_CASE("test_file_hotkeys_deep_links_to_settings_destination",
          "[test_hotkey_settings_page][widget]") {
    AppPagesRig rig;
    fv::SettingsWindow window(rig.context());
    fv::SettingsDialog* raised = nullptr;
    window.raiseDialog = [&raised](fv::SettingsDialog& dialog) {
        raised = &dialog;
    };
    window.openHotkeys();
    REQUIRE(window.dialog() != nullptr);
    CHECK(window.dialog()->currentDestination() == qs(dest::hotkeys));
    CHECK(window.dialog()->isBuilt(dest::hotkeys));
    CHECK(raised == window.dialog());
}

// from
// tests/test_hotkey_settings_page.py::TestHotkeySettingsNavigation::test_regular_settings_opens_the_remembered_view
TEST_CASE("test_regular_settings_opens_the_remembered_view",
          "[test_hotkey_settings_page][widget]") {
    AppPagesRig rig;
    fv::SettingsWindow window(rig.context());
    fv::SettingsDialog* raised = nullptr;
    window.raiseDialog = [&raised](fv::SettingsDialog& dialog) {
        raised = &dialog;
    };
    window.openHotkeys();
    window.open();
    CHECK(window.dialog()->currentDestination() == qs(dest::overview));
    CHECK(raised == window.dialog());
}

// A11-99: _recording_look_recorder. The Appearance preview's throwaway recorder listens on the
// saved microphone order and writes to the preview's scratch file, never to a real recording.
TEST_CASE("the look preview's recorder uses the saved microphones and a scratch file", "[widget]") {
    fv::testkit::TempDataRoot root;
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const Json priority = Json::array({Json{{"name", "USB Mic"}, {"hostapi", "Windows WASAPI"}},
                                       Json{{"name", "Headset"}, {"hostapi", "Windows WASAPI"}}});
    fv::MemorySettingsStore settings(Json{{"audio_input_priority", priority}});
    fv::audio::testing::FakeBackend backend;
    fv::qt::QtUiExecutor ui(qApp);
    fv::AppServices services;
    services.identity = &fv::identity();
    services.paths = &paths;
    services.settings = &settings;
    services.audio = &backend;
    services.ui = &ui;
    fv::AppContext context(services);
    fv::AppPages pages(context);
    const fv::AppPagesHooks hooks = pages.resolvedHooks();
    REQUIRE(hooks.previewRecorder);
    const std::unique_ptr<fv::Recorder> recorder = hooks.previewRecorder(*settings.snapshot());
    REQUIRE(recorder != nullptr);
    CHECK(recorder->output_file() == fv::RecordingLookPreview::previewPath());
    CHECK(recorder->device_priority() ==
          std::vector<fv::audio::DeviceKey>{{"USB Mic", "Windows WASAPI"},
                                            {"Headset", "Windows WASAPI"}});
    // equivalent: Windows' temporary folder ends in a separator and may be spelled short.
    CHECK(std::filesystem::equivalent(fv::RecordingLookPreview::previewPath().parent_path(),
                                      std::filesystem::temp_directory_path()));
    CHECK(fv::RecordingLookPreview::previewPath().filename() ==
          "openwhisper_cpp_overlay_preview.wav");
}

// A11-49: the live preview text size as _build_appearance_page makes it: 10 to 48 pt, 16 by
// default, applied once typing is finished.
TEST_CASE("the live preview text size spans 10 to 48 pt from 16", "[widget]") {
    AppPagesRig rig;
    auto& page = rig.page<fv::AppearancePage>(dest::appearance);
    QSpinBox* spin = page.fontSizeSpin();
    CHECK(spin->minimum() == 10);
    CHECK(spin->maximum() == 48);
    CHECK(spin->suffix() == QStringLiteral(" pt"));
    CHECK(spin->value() == 16);
    CHECK_FALSE(spin->keyboardTracking());
    CHECK(spin->accessibleName() == QStringLiteral("Live preview text size"));
}
