// Settings > Home (settings_home.py, settings_dialog.py _refresh_home): the projections of the
// pages' own controls, the cards and their reflow, the where strip, the Storage card, the footer,
// the "New in FioriVoce" note and the HomeRows hook.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <QAbstractButton>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>

#include "appcore/mic_priority.h"
#include "appcore/whats_new.h"
#include "audio/devices.h"
#include "base/utf8.h"
#include "home_support.h"
#include "hotkeys/mouse_shortcuts.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/model_selection.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_settings/recording_page.h"
#include "ui_settings/whats_new_tile.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/nav_rail.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/speech_backend_picker.h"
#include "ui_widgets/wrapped_label.h"

using fvtest::HomeRig;
using fvtest::qs;
namespace dest = fv::destinations;
namespace keys = fv::keys;

namespace {

QStringList itemTexts(const QComboBox* combo) {
    QStringList texts;
    for (int index = 0; index < combo->count(); ++index) {
        texts.append(combo->itemText(index));
    }
    return texts;
}

QAbstractButton* switchOf(fv::HomePage& home, std::string_view key) {
    auto* control = qobject_cast<QAbstractButton*>(home.control(key));
    REQUIRE(control != nullptr);
    return control;
}

QComboBox* comboOf(fv::HomePage& home, std::string_view key) {
    auto* control = qobject_cast<QComboBox*>(home.control(key));
    REQUIRE(control != nullptr);
    return control;
}

QString token(const fv::Json& entry) {
    return qs(fv::appcore::mic::entry_token(
        {entry["name"].get<std::string>(), entry["hostapi"].get<std::string>()}));
}

// Builds Recording and waits for its device list, as the s7 tests' fixture does.
fv::RecordingPage& recordingWithDevices(HomeRig& rig) {
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.ensurePage(dest::recording);
    auto* page = dynamic_cast<fv::RecordingPage*>(dialog.pageContent(dest::recording));
    REQUIRE(page != nullptr);
    REQUIRE(fvtest::pumpUntil([page] { return page->microphones().devices().has_value(); }));
    fvtest::processEvents(2);
    return *page;
}

// The link of a card whose text starts with `text`.
QPushButton* link(QWidget* root, const QString& text) {
    for (QPushButton* button : root->findChildren<QPushButton*>(QStringLiteral("homeLink"))) {
        if (button->text().startsWith(text)) {
            return button;
        }
    }
    return nullptr;
}

// A page module's Home rows, as the fake page module of the foundation tests contributes them.
struct StandInRows {
    std::vector<std::string> calls;
    fv::SettingsSwitch* control = nullptr;
};

void contribute(HomeRig& rig, StandInRows& rows) {
    rig.homeRows().addRows(fv::HomeCardId::Personalize, [&rows](fv::HomePage& home) {
        rows.calls.push_back("home_rows");
        rows.control = new fv::SettingsSwitch;
        home.addRow(fv::HomeCardId::Personalize, QStringLiteral("Fake words"),
                    QStringLiteral("zebra quokka"), rows.control);
        home.addRefreshHook([&rows](const fv::Json&) { rows.calls.push_back("home_refresh"); });
    });
    rig.homeRows().addRows(fv::HomeCardId::Meetings, [](fv::HomePage& home) {
        home.addRow(fv::HomeCardId::Meetings, QStringLiteral("Voice & speakers"), QString(),
                    fv::neutralButton(new fv::Button(QStringLiteral("Change"))));
    });
}

} // namespace

// --- TestHome ---

// from
// tests/test_settings_unified.py::TestHome::test_home_opens_without_building_pages_or_asking_for_microphones
TEST_CASE("test_home_opens_without_building_pages_or_asking_for_microphones",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK(dialog.currentDestination() == qs(dest::overview));
    CHECK(dialog.stack()->currentWidget() == dialog.pageScroll(dest::overview));
    CHECK(rig.builtPages() == std::vector<std::string>{std::string(dest::overview)});
    QComboBox* combo = comboOf(rig.home(), keys::audio_input_priority.key());
    CHECK(itemTexts(combo) == QStringList{QStringLiteral("System default")});
    fvtest::processEvents(3);
    CHECK(rig.listings == 0);
}

// from tests/test_settings_unified.py::TestHome::test_basic_tab_keys_open_their_home_cards
TEST_CASE("test_basic_tab_keys_open_their_home_cards", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    fv::HomeCard* previous = nullptr;
    for (const auto& [key, card] :
         {std::pair{dest::basic_meetings, "meetings"}, std::pair{dest::basic_app, "app"},
          std::pair{dest::basic_dictation, "dictation"}}) {
        INFO(card);
        dialog.selectDestination(qs(dest::recording));
        dialog.selectDestination(qs(key));
        fvtest::processEvents();
        CHECK(dialog.currentDestination() == qs(dest::overview));
        CHECK(rig.home().card(card)->property("searchHit").toBool());
        if (previous != nullptr) {
            // Each reveal clears the one before it.
            CHECK_FALSE(previous->property("searchHit").toBool());
        }
        previous = rig.home().card(card);
    }
    dialog.close();
}

// from
// tests/test_settings_unified.py::TestHome::test_home_and_page_changes_share_the_same_store_and_callbacks
TEST_CASE("test_home_and_page_changes_share_the_same_store_and_callbacks",
          "[test_settings_unified][widget]") {
    HomeRig rig(fv::Json{{"streaming_enabled", false}, {"streaming_overlay_enabled", true}});
    fv::HomePage& home = rig.home();
    int told = 0;
    QObject::connect(&rig.signals(), &fv::SettingsPagesSignals::streamingSettingsChanged,
                     [&told] { ++told; });
    switchOf(home, keys::streaming_enabled.key())->click();
    CHECK(rig.saved(keys::streaming_enabled.key()) == fv::Json(true));
    CHECK_FALSE(rig.has(keys::streaming_overlay_enabled.key()));
    CHECK(told == 1);
    rig.dialog().selectDestination(qs(dest::recording));
    auto* check = qobject_cast<QAbstractButton*>(rig.dialog().control("streaming_enabled_check"));
    REQUIRE(check != nullptr);
    CHECK(check->isChecked());
    check->setChecked(false);
    rig.dialog().showHome();
    CHECK_FALSE(switchOf(home, keys::streaming_enabled.key())->isChecked());
}

// from tests/test_settings_unified.py::TestHome::test_microphone_reads_devices_once_its_menu_opens
TEST_CASE("test_microphone_reads_devices_once_its_menu_opens", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    std::vector<fv::Json> told;
    QObject::connect(&rig.signals(), &fv::SettingsPagesSignals::audioDeviceChanged,
                     [&told](const fv::Json& priority) { told.push_back(priority); });
    QComboBox* combo = comboOf(rig.home(), keys::audio_input_priority.key());
    CHECK_FALSE(dialog.isBuilt(dest::recording));
    rig.home().loadMicrophones();
    CHECK(dialog.isBuilt(dest::recording));
    auto* page = dynamic_cast<fv::RecordingPage*>(dialog.pageContent(dest::recording));
    REQUIRE(page != nullptr);
    REQUIRE(fvtest::pumpUntil([page] { return page->microphones().devices().has_value(); }));
    fvtest::processEvents(2);
    // The device list redrew the rail, and Home, the page shown, copied it.
    const QString snowball = token(fvtest::kSnowball);
    REQUIRE(combo->findData(snowball) >= 0);
    int legacyIndex = -1;
    for (const auto& device : fv::audio::list_input_devices(rig.backend())) {
        if (device.name == "Microphone (Blue Snowball )" && device.hostapi == fvtest::kMme) {
            legacyIndex = device.index;
        }
    }

    combo->setCurrentIndex(combo->findData(snowball));
    CHECK(rig.saved(keys::audio_input_priority.key()) == fv::Json::array({fvtest::kSnowball}));
    // The pre-ranking index follows along for a downgrade.
    CHECK(rig.saved(keys::audio_input_device.key()) == fv::Json(legacyIndex));
    REQUIRE(told.size() == 1);
    CHECK(told.front() == fv::Json::array({fvtest::kSnowball}));
    auto* source = qobject_cast<QComboBox*>(dialog.control("audio_device_combo"));
    REQUIRE(source != nullptr);
    CHECK(source->currentData().toString() == snowball);

    combo->setCurrentIndex(combo->findData(qs(fv::appcore::mic::kSystemDefault)));
    CHECK(rig.saved(keys::audio_input_priority.key()) == fv::Json::array());
    CHECK_FALSE(rig.has(keys::audio_input_device.key()));
}

// from
// tests/test_settings_unified.py::TestHome::test_voice_choice_uses_the_canonical_controller_label_without_model_setup
TEST_CASE("test_voice_choice_uses_the_canonical_controller_label_without_model_setup",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    QComboBox* combo = rig.home().voiceCombo();
    CHECK(combo->currentData().toString() == QStringLiteral("parakeet"));
    // models.on_backend_changed, as the test's MagicMock stands in for the controller's hook.
    QStringList told;
    rig.context().service<fv::HomeSources>().backendChanged = [&rig,
                                                               &told](const QString& displayName) {
        told.append(displayName);
        fv::save_model_selection(rig.settings(),
                                 displayName == fv::backendDisplayName("api") ? "api" : "parakeet");
    };
    combo->setCurrentIndex(combo->findData(QStringLiteral("api")));
    CHECK(rig.saved(keys::selected_model.key()) == fv::Json("api"));
    CHECK(told == QStringList{fv::backendDisplayName("api")});
    // The Voice model page stays unbuilt (VOICE_MODEL not in dialog._built_pages).
    CHECK_FALSE(dialog.isBuilt(dest::voice_model));
    CHECK(combo->currentData().toString() == QStringLiteral("api"));
}

// choose_backend once the Voice model page is built: the page's own engine combo takes the
// choice, so its handler runs and the page shows it.
TEST_CASE("a voice chosen on Home with the Voice model page built goes through the page",
          "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    QComboBox* combo = rig.home().voiceCombo();
    dialog.ensurePage(dest::voice_model);
    auto* voice = dynamic_cast<fv::VoiceModelPage*>(dialog.pageContent(dest::voice_model));
    REQUIRE(voice != nullptr);
    QStringList told;
    QObject::connect(voice, &fv::VoiceModelPage::backendChanged,
                     [&told](const QString& name) { told.append(name); });
    combo->setCurrentIndex(combo->findData(QStringLiteral("api")));
    CHECK(rig.saved(keys::selected_model.key()) == fv::Json("api"));
    CHECK(told == QStringList{fv::backendDisplayName("api")});
    CHECK(voice->engineCombo()->currentData().toString() == QStringLiteral("api"));
    CHECK(combo->currentData().toString() == QStringLiteral("api"));
}

// choose_backend without a hook saves the choice itself; a hook that fails is shown and Home
// goes back to the saved engine.
TEST_CASE("a voice chosen on Home without the page saves it, and a failed change is shown",
          "[widget]") {
    SECTION("no hook") {
        HomeRig rig;
        QComboBox* combo = rig.home().voiceCombo();
        combo->setCurrentIndex(combo->findData(QStringLiteral("api")));
        CHECK(rig.saved(keys::selected_model.key()) == fv::Json("api"));
        CHECK_FALSE(rig.dialog().isBuilt(dest::voice_model));
        CHECK(rig.dialog().rail()->value(qs(dest::voice_model)) ==
              fv::VoiceModelPage::railValue(*rig.settings().snapshot()));
    }
    SECTION("a hook that fails") {
        HomeRig rig;
        rig.context().service<fv::HomeSources>().backendChanged = [](const QString&) {
            throw std::runtime_error("the engine is busy");
        };
        QComboBox* combo = rig.home().voiceCombo();
        combo->setCurrentIndex(combo->findData(QStringLiteral("api")));
        CHECK_FALSE(rig.has(keys::selected_model.key()));
        CHECK(rig.dialog().messageText() ==
              QStringLiteral("Couldn't change voice model: the engine is busy"));
        CHECK(combo->currentData().toString() == QStringLiteral("parakeet"));
        CHECK_FALSE(rig.dialog().isBuilt(dest::voice_model));
    }
}

// from
// tests/test_settings_unified.py::TestHome::test_app_theme_and_meeting_report_edits_use_existing_handlers
TEST_CASE("test_app_theme_and_meeting_report_edits_use_existing_handlers",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    QStringList told;
    QObject::connect(&rig.signals(), &fv::SettingsPagesSignals::uiThemeChanged,
                     [&told](const QString& theme) { told.append(theme); });
    QComboBox* theme = comboOf(rig.home(), keys::ui_theme.key());
    theme->setCurrentIndex(theme->findData(QStringLiteral("light")));
    CHECK(rig.saved(keys::ui_theme.key()) == fv::Json("light"));
    CHECK(told == QStringList{QStringLiteral("light")});
    // The meeting report half needs the After the meeting page (M7), which contributes its row
    // through HomeRows.
}

// from tests/test_settings_unified.py::TestHome::test_failed_save_restores_home_and_allows_retry
TEST_CASE("test_failed_save_restores_home_and_allows_retry", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    QAbstractButton* control = switchOf(rig.home(), keys::auto_paste.key());
    const bool previous = control->isChecked();
    rig.settings().file().fail_next_replace("Disk full");
    control->click();
    CHECK(control->isChecked() == previous);
    auto* source = qobject_cast<QAbstractButton*>(dialog.control("auto_paste_check"));
    REQUIRE(source != nullptr);
    CHECK(source->isChecked() == previous);
    CHECK(dialog.messageText().contains(QStringLiteral("Disk full")));
    control->click();
    CHECK(rig.saved(keys::auto_paste.key()) == fv::Json(!previous));
    CHECK(dialog.messageText().isEmpty());
}

// from
// tests/test_settings_unified.py::TestHome::test_home_shortcut_capture_cancels_when_leaving_home
TEST_CASE("test_home_shortcut_capture_cancels_when_leaving_home",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    std::vector<bool> suspended;
    QObject::connect(&dialog, &fv::SettingsDialog::hotkeyCaptureSuspended,
                     [&suspended](bool value) { suspended.push_back(value); });
    fv::ProfileHotkeyInput* field = rig.home().shortcut();
    field->beginCapture();
    REQUIRE_FALSE(suspended.empty());
    CHECK(suspended.back());
    dialog.selectDestination(qs(dest::recording));
    CHECK_FALSE(field->isCapturing());
    CHECK_FALSE(suspended.back());
}

// from
// tests/test_settings_unified.py::TestHome::test_change_button_captures_and_saves_a_shortcut_inline
TEST_CASE("test_change_button_captures_and_saves_a_shortcut_inline",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.activateWindow();
    REQUIRE(QTest::qWaitForWindowActive(&dialog));
    fv::ProfileHotkeyInput* field = rig.home().shortcut();
    QPushButton* button = rig.home().shortcutChangeButton();
    CHECK(button->objectName() == QStringLiteral("settingsChangeShortcut"));
    QTest::mouseClick(button, Qt::LeftButton);
    CHECK(field->hasFocus());
    CHECK(field->isCapturing());
    QTest::keyClick(field, Qt::Key_J, Qt::ControlModifier | Qt::AltModifier);
    CHECK_FALSE(field->isCapturing());
    const fv::Json saved = fv::load_hotkey_settings(rig.settings());
    CHECK(qs(saved.value("record_toggle", std::string())) == field->hotkey());
    CHECK(field->hotkey() == QStringLiteral("ctrl+alt+j"));
    dialog.selectDestination(qs(dest::hotkeys));
    auto* hotkeys = dynamic_cast<fv::HotkeysPage*>(dialog.pageContent(dest::hotkeys));
    REQUIRE(hotkeys != nullptr);
    CHECK(hotkeys->hotkeyInput("record_toggle")->text() == field->text());
    dialog.close();
}

// from tests/test_settings_unified.py::TestHome::test_home_rows_fit_narrow_windows_and_large_fonts
TEST_CASE("test_home_rows_fit_narrow_windows_and_large_fonts", "[test_settings_unified][widget]") {
    // The Omarchy variant (460 px) is other-platform: no Omarchy desktop runs FioriVoce.
    const int width = GENERATE(940, 1315);
    INFO(width);
    fvtest::AppTheme theme(QStringLiteral("dark"), 130);
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.fitToScreen();
    dialog.resize(width, 600);
    fvtest::processEvents(8);
    fv::HomePage& page = rig.home();
    CHECK(dialog.width() == width);
    // Too narrow for the rail beside a page at 130 %: the header's menu stands in.
    const bool narrow = width < fv::ui::scaled(fv::SettingsDialog::kNarrowWidth);
    CHECK(dialog.railPane()->isHidden() == narrow);
    CHECK(dialog.pageMenuButton()->isHidden() == !narrow);
    QList<QWidget*> controls;
    for (QAbstractButton* button : page.findChildren<QAbstractButton*>()) {
        controls.append(button);
    }
    for (QComboBox* box : page.findChildren<QComboBox*>()) {
        controls.append(box);
    }
    for (QLineEdit* edit : page.findChildren<QLineEdit*>()) {
        controls.append(edit);
    }
    for (QWidget* control : controls) {
        if (!control->isVisible()) {
            continue;
        }
        INFO(control->objectName().toStdString() << " " << control->accessibleName().toStdString());
        CHECK(control->mapTo(&page, control->rect().topLeft()).x() >= 0);
        CHECK(control->mapTo(&page, control->rect().bottomRight()).x() < page.width());
    }
    for (QLabel* label : page.findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    QScrollArea* scroll = dialog.pageScroll(dest::overview);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    fvtest::processEvents();
    CHECK(page.mapTo(scroll->viewport(), page.rect().bottomRight()).y() <=
          scroll->viewport()->height());
    dialog.close();
}

// from
// tests/test_settings_unified.py::TestHome::test_cards_sit_in_two_columns_when_the_page_is_wide
TEST_CASE("test_cards_sit_in_two_columns_when_the_page_is_wide",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    StandInRows rows;
    contribute(rig, rows);
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    fv::HomePage& home = rig.home();
    dialog.resize(1500, 850);
    fvtest::processEvents(4);
    CHECK(home.isTwoColumns());
    CHECK(home.card("personalize")->x() > home.card("dictation")->x());
    CHECK(home.card("app")->y() > home.card("dictation")->y());
    // The where strip lies across the page.
    CHECK(home.localTitle()->parentWidget()->y() == home.cloudTitle()->parentWidget()->y());
    dialog.resize(fv::SettingsDialog::kMinimumSize);
    fvtest::processEvents(4);
    CHECK_FALSE(home.isTwoColumns());
    for (const char* name : {"personalize", "meetings", "app", "storage"}) {
        INFO(name);
        CHECK(home.card(name)->x() == home.card("dictation")->x());
    }
    CHECK(home.card("storage")->y() > home.card("app")->y());
    CHECK(home.card("app")->y() > home.card("meetings")->y());
    CHECK(home.card("meetings")->y() > home.card("personalize")->y());
    CHECK(home.card("personalize")->y() > home.card("dictation")->y());
    CHECK(home.cloudTitle()->parentWidget()->y() > home.localTitle()->parentWidget()->y());
    dialog.close();
}

// --- TestHomeSummary ---

// from
// tests/test_settings_unified.py::TestHomeSummary::test_rows_and_storage_repeat_what_each_destination_reports
TEST_CASE("test_rows_and_storage_repeat_what_each_destination_reports",
          "[test_settings_unified][widget]") {
    HomeRig rig;
    rig.storage = fv::models::StorageSummary{1, 2, 145'000'000, {{"Parakeet", 145'000'000}}};
    rig.dialog().selectDestination(qs(dest::overview));
    fv::HomePage& home = rig.home();
    CHECK(home.storageValue()->text() == QStringLiteral("145 MB of speech models"));
    CHECK(home.storageDetail()->text().startsWith(QStringLiteral("1 of ")));
    // The Meetings card's lines (meeting model, AI insights) are the meeting pages' (M7).
}

// from
// tests/test_settings_unified.py::TestHomeSummary::test_links_and_buttons_open_their_destination
TEST_CASE("test_links_and_buttons_open_their_destination", "[test_settings_unified][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    QPushButton* downloads = link(rig.home().card("storage"), QStringLiteral("Downloads"));
    REQUIRE(downloads != nullptr);
    downloads->click();
    CHECK(dialog.currentDestination() == qs(dest::downloads));
    dialog.showHome();
    QPushButton* all = link(rig.home().card("dictation"), QStringLiteral("All dictation settings"));
    REQUIRE(all != nullptr);
    QTest::mouseClick(all, Qt::LeftButton);
    CHECK(dialog.currentDestination() == qs(dest::recording));
    dialog.showHome();
    QPushButton* app = link(rig.home().card("app"), QStringLiteral("Overlay, hotkeys"));
    REQUIRE(app != nullptr);
    app->click();
    CHECK(dialog.currentDestination() == qs(dest::appearance));
    // "Set up AI insights" is the meeting pages' row (M7).
    dialog.close();
}

// from tests/test_settings_unified.py::TestHomeSummary::test_home_is_only_reread_while_it_shows
TEST_CASE("test_home_is_only_reread_while_it_shows", "[test_settings_unified][widget]") {
    HomeRig rig;
    int refreshes = 0;
    rig.homeRows().addRows(fv::HomeCardId::Personalize, [&refreshes](fv::HomePage& home) {
        home.addRefreshHook([&refreshes](const fv::Json&) { ++refreshes; });
    });
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.selectDestination(qs(dest::general));
    const int before = refreshes;
    auto* check = qobject_cast<QAbstractButton*>(dialog.control("auto_paste_check"));
    REQUIRE(check != nullptr);
    check->click();
    CHECK(refreshes == before);
    dialog.showHome();
    CHECK(refreshes > before);
    CHECK(switchOf(rig.home(), keys::auto_paste.key())->isChecked() ==
          rig.saved(keys::auto_paste.key()).get<bool>());
}

// --- The rest of Home's refresh (A11-29, A11-31, A11-90, A11-91) ---

TEST_CASE("the where strip lists where dictation runs and the recordings", "[widget]") {
    SECTION("Parakeet on this computer") {
        HomeRig rig;
        fv::HomePage& home = rig.home();
        // The saved cleanup provider is OpenRouter, so AI cleanup is listed on the cloud side.
        CHECK(home.localList()->text() ==
              QStringLiteral(
                  "Dictation voice · Text near the cursor (off) · Learned rules · Recordings"));
        CHECK(home.cloudList()->text() == QStringLiteral("AI cleanup (off)"));
        CHECK(home.voiceDetail()->text() ==
              QStringLiteral("Audio is transcribed on this computer."));
    }
    SECTION("the API engine on a hosted server") {
        HomeRig rig(fv::Json{{"selected_model", "api"}});
        fv::HomePage& home = rig.home();
        CHECK(home.localList()->text() ==
              QStringLiteral("Text near the cursor (off) · Learned rules · Recordings"));
        CHECK(home.cloudList()->text() == QStringLiteral("Dictation voice · AI cleanup (off)"));
        CHECK(home.voiceDetail()->text() ==
              qs(fv::voice_detail(fv::Json{{"selected_model", "api"}})));
    }
    SECTION("a paired computer") {
        HomeRig rig(fv::Json{{"selected_model", "remote"}});
        const fv::Json settings{{"selected_model", "remote"}};
        REQUIRE(fv::voice_is_remote(settings));
        REQUIRE_FALSE(fv::voice_is_cloud(settings));
        fv::HomePage& home = rig.home();
        CHECK(home.cloudList()->text() == QStringLiteral("Dictation voice (%1) · AI cleanup (off)")
                                              .arg(qs(fv::voice_detail(settings)).toLower()));
    }
    SECTION("pages add their items in OpenWhisper's places") {
        HomeRig rig;
        // Learned rules is Home's own item (always local, before the recordings).
        rig.homeRows().addWhereItems([](const fv::Json&) {
            return std::vector<fv::HomeWhereItem>{
                {fv::HomeRows::kWhereMeetingIntelligence,
                 QStringLiteral("Meeting intelligence (transcript text, when enabled)"), true},
                {fv::HomeRows::kWhereMeetingVoice, QStringLiteral("Meeting voice"), false},
            };
        });
        fv::HomePage& home = rig.home();
        CHECK(home.localList()->text() ==
              QStringLiteral("Dictation voice · Meeting voice · Text near the cursor (off) · "
                             "Learned rules · Recordings"));
        CHECK(home.cloudList()->text() ==
              QStringLiteral("AI cleanup (off) · Meeting intelligence (transcript text, when "
                             "enabled)"));
    }
}

TEST_CASE("the Storage card shows the inventory, its bar and the components", "[widget]") {
    fvtest::AppTheme theme;
    HomeRig rig;
    SECTION("while the inventory is read") {
        rig.storage.reset();
        fv::HomePage& home = rig.home();
        CHECK(home.storageValue()->text() == QStringLiteral("Checking…"));
        CHECK(home.storageDetail()->text() == QStringLiteral("Scanning downloaded models"));
        CHECK(home.storageBar()->segments().empty());
        CHECK(home.storageLegend()->text().isEmpty());
    }
    SECTION("nothing downloaded") {
        fv::HomePage& home = rig.home();
        CHECK(home.storageValue()->text() == QStringLiteral("No speech models downloaded yet"));
        CHECK(home.storageDetail()->text() == QStringLiteral("0 of 0 downloaded"));
        CHECK(home.storageLegend()->isHidden());
    }
    SECTION("backends by size, the legend's colours cycling") {
        rig.storage = fv::models::StorageSummary{7,
                                                 9,
                                                 1'400'000'000,
                                                 {{"Small", 100'000'000},
                                                  {"Big", 700'000'000},
                                                  {"B", 200'000'000},
                                                  {"C", 150'000'000},
                                                  {"D", 130'000'000},
                                                  {"E", 120'000'000}}};
        fv::HomePage& home = rig.home();
        CHECK(home.storageValue()->text() == QStringLiteral("1.40 GB of speech models"));
        CHECK(home.storageDetail()->text() == QStringLiteral("7 of 9 downloaded"));
        const auto& segments = home.storageBar()->segments();
        REQUIRE(segments.size() == 6);
        CHECK(segments[0].second == "accent");
        CHECK(segments[0].first == 0.5);
        CHECK(segments[4].second == "accent-cyan");
        CHECK(segments[5].second == "accent");
        const QString legend = home.storageLegend()->text();
        CHECK(legend.indexOf(QStringLiteral("Big 700 MB")) <
              legend.indexOf(QStringLiteral("Small")));
        CHECK(legend.startsWith(QStringLiteral("<span style=\"color:%1\">■</span>&nbsp;Big")
                                    .arg(fv::ui::tokenColor("accent").name())));
        CHECK(legend.contains(QStringLiteral("&nbsp;&nbsp;&nbsp;")));
    }
    SECTION("components, installed and not") {
        fv::HomePage& home = rig.home();
        const QString text = home.componentsLabel()->text();
        CHECK(text.startsWith(QStringLiteral("<b>Components · 1 of 2 installed</b><br>")));
        CHECK(text.contains(QStringLiteral("<span style=\"color:%1\">●</span>&nbsp;NVIDIA "
                                           "speech runtime (CPU)")
                                .arg(fv::ui::tokenColor("success").name())));
        CHECK(text.contains(QStringLiteral("<span style=\"color:%1\">●</span>&nbsp;NVIDIA "
                                           "speech runtime (CUDA)")
                                .arg(fv::ui::tokenColor("slate-text-4").name())));
    }
    SECTION("no components here") {
        rig.components.clear();
        CHECK(rig.home().componentsLabel()->text() ==
              QStringLiteral("<b>Components</b> · none available"));
    }
}

TEST_CASE("the footer names the API keys, the runtime and the download policy", "[widget]") {
    const auto [policy, label] =
        GENERATE(std::pair{"ask", "ask first"}, std::pair{"always", "always download"},
                 std::pair{"never", "offline"});
    HomeRig rig(fv::Json{{"hf_access_policy", policy}});
    const fv::Json settings{{"hf_access_policy", policy}};
    const fv::SettingsPageSpec* keysPage = rig.context().settingsPages().page(dest::api_keys);
    REQUIRE(keysPage != nullptr);
    CHECK(rig.home().footerLabel()->text() ==
          QStringLiteral("API keys: %1  ·  Runtime: %2  ·  Missing models: %3")
              .arg(keysPage->railValue(settings), qs(fv::runtime_summary(settings, false)),
                   QString::fromUtf8(label)));
}

TEST_CASE("the shortcut row says what the record shortcut does", "[widget]") {
    SECTION("toggle") {
        HomeRig rig;
        CHECK(rig.home().shortcut()->hotkey() ==
              qs(fv::default_hotkeys()["record_toggle"].get<std::string>()));
        CHECK(rig.home().shortcutDescription()->text() ==
              QStringLiteral("Press to start or stop dictation."));
    }
    SECTION("push and hold, with the hands-free latch") {
        HomeRig rig(fv::Json{{"recording_trigger_mode", "push_hold"}});
        CHECK(rig.home().shortcutDescription()->text() ==
              QStringLiteral("Hold to record, or double-tap to keep recording hands-free."));
    }
    SECTION("push and hold, without the latch") {
        HomeRig rig(fv::Json{{"recording_trigger_mode", "push_hold"},
                             {"recording_hands_free_latch", false}});
        CHECK(rig.home().shortcutDescription()->text() ==
              QStringLiteral("Hold to record; release to stop and transcribe."));
    }
    SECTION("a side button notes what it changes") {
        HomeRig rig(fv::Json{{"hotkeys", fv::Json{{"record_toggle", "mouse4"}}}});
        const std::string note = fv::hotkeys::mouse_shortcut_note("mouse4");
        CHECK(rig.home().shortcutDescription()->text() ==
              QStringLiteral("Press to start or stop dictation.") +
                  (note.empty() ? QString() : QStringLiteral(" ") + qs(note)));
    }
}

// from tests/test_personalize_s6_settings.py::test_basic_recording_shortcut_mentions_double_tap
TEST_CASE("test_basic_recording_shortcut_mentions_double_tap",
          "[test_personalize_s6_settings][widget]") {
    HomeRig rig(fv::Json{{"recording_trigger_mode", "push_hold"}});
    fv::HomePage& page = rig.home();
    page.refreshShortcut();
    CHECK(page.shortcutDescription()->text().contains(QStringLiteral("double-tap")));
}

// The Home half of TestRouting::test_refresh_rebuilds_home_once (test_shell.cpp has the shell's,
// over a stand-in): one refresh of the window, with pages that refresh themselves built, re-reads
// the real Home once.
TEST_CASE("one refresh of the window re-reads Home once", "[widget]") {
    HomeRig rig;
    int refreshes = 0;
    rig.homeRows().addRows(fv::HomeCardId::Personalize, [&refreshes](fv::HomePage& home) {
        home.addRefreshHook([&refreshes](const fv::Json&) { ++refreshes; });
    });
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.ensurePage(dest::general);
    dialog.ensurePage(dest::appearance);
    REQUIRE(dialog.currentDestination() == qs(dest::overview));
    refreshes = 0;
    dialog.refresh();
    CHECK(refreshes == 1);
}

// The Home half of TestRouting::test_show_home_returns_to_the_top_of_home: the real Home, taller
// than the smallest window, is scrolled back to its top.
TEST_CASE("show Home returns to the top of the real Home", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    dialog.resize(fv::SettingsDialog::kMinimumSize);
    fvtest::processEvents(4);
    QScrollArea* scroll = dialog.pageScroll(dest::overview);
    REQUIRE(scroll != nullptr);
    REQUIRE(scroll->verticalScrollBar()->maximum() > 0);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    dialog.selectDestination(qs(dest::general));
    dialog.showHome();
    CHECK(dialog.currentDestination() == qs(dest::overview));
    CHECK(scroll->verticalScrollBar()->value() == 0);
    dialog.close();
}

TEST_CASE("a Home row whose page cannot be built saves nothing and shows the saved value",
          "[widget]") {
    HomeRig rig(fv::Json{{"auto_paste", true}}, {dest::dictionary});
    const auto heading = std::ranges::find_if(fvtest::headings(), [](const fvtest::Heading& item) {
        return item.key == dest::dictionary;
    });
    REQUIRE(heading != fvtest::headings().end());
    fv::SettingsPageSpec broken = fvtest::standInSpec(*heading);
    broken.create = [](QWidget*) -> QWidget* {
        throw std::runtime_error("This page cannot be built.");
    };
    rig.context().settingsPages().addPage(std::move(broken));
    fv::SettingsSwitch* row = nullptr;
    rig.homeRows().addRows(fv::HomeCardId::Personalize, [&row](fv::HomePage& home) {
        row = home.toggle(
            fv::HomeCardId::Personalize, QStringLiteral("Broken row"), QString(),
            std::string(dest::dictionary), "broken_check",
            [](const fv::Json& settings) { return QVariant(keys::auto_paste.read(settings)); });
    });
    fv::HomePage& home = rig.home();
    REQUIRE(row != nullptr);
    CHECK(row->isChecked());
    row->click();
    // Nothing was saved and the switch shows the saved value again; a retry still reaches apply.
    CHECK(row->isChecked());
    CHECK(rig.saved(keys::auto_paste.key()) == fv::Json(true));
    CHECK_FALSE(rig.dialog().isBuilt(dest::dictionary));
    row->click();
    CHECK(row->isChecked());
    // The rest of Home still saves.
    switchOf(home, keys::auto_paste.key())->click();
    CHECK(rig.saved(keys::auto_paste.key()) == fv::Json(false));
}

TEST_CASE("the App card follows the update and tray rules", "[widget]") {
    SECTION("notifying needs automatic checks") {
        HomeRig rig(fv::Json{{"update_check_enabled", false}});
        fv::HomePage& home = rig.home();
        CHECK_FALSE(home.updateNotify()->isEnabled());
        switchOf(home, keys::update_check_enabled.key())->click();
        CHECK(rig.saved(keys::update_check_enabled.key()) == fv::Json(true));
        CHECK(home.updateNotify()->isEnabled());
    }
    SECTION("a system tray") {
        HomeRig rig;
        QAbstractButton* tray = switchOf(rig.home(), keys::minimize_tray.key());
        CHECK(tray->isEnabled());
        CHECK(tray->isChecked());
        CHECK(tray->toolTip().isEmpty());
    }
    SECTION("no system tray") {
        HomeRig rig;
        rig.trayAvailable = false;
        QAbstractButton* tray = switchOf(rig.home(), keys::minimize_tray.key());
        CHECK_FALSE(tray->isEnabled());
        CHECK_FALSE(tray->isChecked());
        CHECK(tray->toolTip() ==
              QStringLiteral("This desktop session does not provide a system tray."));
    }
    SECTION("the font size goes through Appearance") {
        HomeRig rig;
        QList<int> told;
        QObject::connect(&rig.signals(), &fv::SettingsPagesSignals::uiFontScaleChanged,
                         [&told](int percent) { told.append(percent); });
        QComboBox* size = comboOf(rig.home(), keys::ui_font_scale.key());
        CHECK(size->currentData().toInt() == 100);
        size->setCurrentIndex(size->findData(115));
        CHECK(rig.saved(keys::ui_font_scale.key()) == fv::Json(115));
        CHECK(told == QList<int>{115});
    }
}

TEST_CASE("changing Paste text automatically on Home saves through General and the rail follows",
          "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    CHECK(dialog.rail()->value(qs(dest::general)) == QStringLiteral("Auto-paste on"));
    switchOf(rig.home(), keys::auto_paste.key())->click();
    CHECK(rig.saved(keys::auto_paste.key()) == fv::Json(false));
    CHECK(dialog.isBuilt(dest::general));
    CHECK(dialog.rail()->value(qs(dest::general)) != QStringLiteral("Auto-paste on"));
}

TEST_CASE("a card with no rows whose link leads nowhere is hidden", "[widget]") {
    SECTION("every destination registered") {
        HomeRig rig;
        fv::HomePage& home = rig.home();
        CHECK_FALSE(home.card("personalize")->isHidden());
        CHECK_FALSE(home.card("meetings")->isHidden());
    }
    SECTION("this build's pages only: no Personalize or Meeting Mode page") {
        HomeRig rig(fv::Json::object(), {dest::dictionary, dest::meeting_voice, dest::downloads});
        fv::HomePage& home = rig.home();
        CHECK(home.card("personalize")->isHidden());
        CHECK(home.card("meetings")->isHidden());
        // The Storage card stays; its link to an unregistered Downloads does not.
        CHECK_FALSE(home.card("storage")->isHidden());
        QPushButton* downloads = link(home.card("storage"), QStringLiteral("Downloads"));
        REQUIRE(downloads != nullptr);
        CHECK(downloads->isHidden());
    }
    SECTION("a contributed row keeps the card, without the link") {
        HomeRig rig(fv::Json::object(), {dest::dictionary});
        rig.homeRows().addRows(fv::HomeCardId::Personalize, [](fv::HomePage& home) {
            home.addRow(fv::HomeCardId::Personalize, QStringLiteral("Dictionary"), QString(),
                        new fv::SettingsSwitch);
        });
        fv::HomePage& home = rig.home();
        CHECK_FALSE(home.card("personalize")->isHidden());
        QPushButton* personalize =
            link(home.card("personalize"), QStringLiteral("Dictionary, snippets"));
        REQUIRE(personalize != nullptr);
        CHECK(personalize->isHidden());
    }
}

// --- Page modules (test_personalize_foundation_ui_settings.py) ---

// from
// tests/test_personalize_foundation_ui_settings.py::TestHomePersonalize::test_home_has_a_personalize_card_fed_by_page_modules
TEST_CASE("test_home_has_a_personalize_card_fed_by_page_modules",
          "[test_personalize_foundation_ui_settings][widget]") {
    HomeRig rig;
    StandInRows rows;
    contribute(rig, rows);
    fv::SettingsDialog& dialog = rig.dialog();
    fv::HomePage& home = rig.home();
    QStringList titles;
    for (QLabel* label : home.findChildren<QLabel*>(QStringLiteral("homeCardTitle"))) {
        titles.append(label->text());
    }
    CHECK(titles == QStringList{QStringLiteral("Dictation"), QStringLiteral("Personalize"),
                                QStringLiteral("Meetings"), QStringLiteral("App"),
                                QStringLiteral("Storage")});
    REQUIRE_FALSE(rows.calls.empty());
    CHECK(rows.calls.front() == "home_rows");
    CHECK(home.card("personalize")->isAncestorOf(rows.control));
    CHECK(rig.builtPages() == std::vector<std::string>{std::string(dest::overview)});

    rows.calls.clear();
    dialog.refreshRailValues();
    CHECK(std::ranges::find(rows.calls, "home_refresh") != rows.calls.end());

    QPushButton* personalize =
        link(home.card("personalize"), QStringLiteral("Dictionary, snippets"));
    REQUIRE(personalize != nullptr);
    personalize->click();
    CHECK(dialog.currentDestination() == qs(dest::dictionary));
}

namespace {

std::vector<fv::WhatsNewTile*> introTiles(fv::SettingsDialog& dialog) {
    std::vector<fv::WhatsNewTile*> tiles;
    for (fv::WhatsNewTile* tile : dialog.page(dest::overview)->findChildren<fv::WhatsNewTile*>()) {
        tiles.push_back(tile);
    }
    return tiles;
}

} // namespace

// from
// tests/test_personalize_foundation_ui_settings.py::TestHomePersonalize::test_new_features_note_dismisses_and_stays_dismissed
TEST_CASE("test_new_features_note_dismisses_and_stays_dismissed",
          "[test_personalize_foundation_ui_settings][widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    const std::vector<fv::WhatsNewTile*> notes = introTiles(dialog);
    REQUIRE(notes.size() == 1);
    CHECK(notes[0]->descriptionLabel()->text() == qs(fv::appcore::whats_new().text));
    CHECK(notes[0]->titleLabel()->text() == QStringLiteral("New in FioriVoce"));
    CHECK(notes[0]->property("kind").toString() == QStringLiteral("intro"));
    CHECK(notes[0]->property("tileId").toString() == QStringLiteral("whatsNew"));
    CHECK(notes[0]->isVisible());
    QPushButton* dismiss = notes[0]->dismissButton();
    CHECK(dismiss->text() == QStringLiteral("Got it"));
    CHECK(dismiss->toolTip() == QStringLiteral("Hide this note"));
    dismiss->click();
    CHECK(rig.saved(keys::whats_new_seen.key()) == fv::Json(fv::appcore::whats_new().updated));
    CHECK(notes[0]->isHidden());
    dialog.close();

    fv::SettingsDialog& reopened = rig.reopen();
    CHECK(introTiles(reopened).empty());
}

// from
// tests/test_personalize_foundation_ui_settings.py::TestHomePersonalize::test_a_rewritten_note_shows_again_after_an_older_one_was_dismissed
TEST_CASE("test_a_rewritten_note_shows_again_after_an_older_one_was_dismissed",
          "[test_personalize_foundation_ui_settings][widget]") {
    HomeRig rig(fv::Json{{"whats_new_seen", "2000-01-01"}});
    CHECK(introTiles(rig.dialog()).size() == 1);
}

TEST_CASE("a note that cannot be saved as seen stays", "[widget]") {
    HomeRig rig;
    fv::SettingsDialog& dialog = rig.dialog();
    dialog.show();
    const std::vector<fv::WhatsNewTile*> notes = introTiles(dialog);
    REQUIRE(notes.size() == 1);
    rig.settings().file().fail_next_replace("Disk full");
    notes[0]->dismissButton()->click();
    CHECK_FALSE(notes[0]->isHidden());
    CHECK(dialog.messageText().contains(QStringLiteral("Disk full")));
    dialog.close();
}

// from tests/test_whats_new.py::test_note_fits_the_home_tile
TEST_CASE("test_note_fits_the_home_tile", "[test_whats_new][widget]") {
    const std::string& text = fv::appcore::whats_new().text;
    const QString note = qs(text);
    CHECK(note.trimmed() == note);
    const auto length = note.toUcs4().size();
    CHECK(length > 0);
    CHECK(static_cast<std::size_t>(length) <= fv::appcore::kWhatsNewMaxTextLength);
}

// --- Microphones (test_personalize_s7_settings.py) ---

// from
// tests/test_personalize_s7_settings.py::test_home_microphone_writes_the_first_entry_and_keeps_the_backups
TEST_CASE("test_home_microphone_writes_the_first_entry_and_keeps_the_backups",
          "[test_personalize_s7_settings][widget]") {
    HomeRig rig(
        fv::Json{{"audio_input_priority", fv::Json::array({fvtest::kUsb, fvtest::kSnowball})}});
    std::vector<fv::Json> told;
    QObject::connect(&rig.signals(), &fv::SettingsPagesSignals::audioDeviceChanged,
                     [&told](const fv::Json& priority) { told.push_back(priority); });
    recordingWithDevices(rig);
    rig.dialog().showHome();
    QComboBox* combo = comboOf(rig.home(), keys::audio_input_priority.key());
    CHECK(combo->currentData().toString() == token(fvtest::kUsb));

    combo->setCurrentIndex(combo->findData(token(fvtest::kWebcam)));
    const fv::Json expected = fv::Json::array({fvtest::kWebcam, fvtest::kUsb, fvtest::kSnowball});
    CHECK(rig.saved(keys::audio_input_priority.key()) == expected);
    REQUIRE(told.size() == 1);
    CHECK(told.front() == expected);
}

TEST_CASE("Home names the saved microphone before the device list is read", "[widget]") {
    HomeRig rig(fv::Json{{"audio_input_priority", fv::Json::array({fvtest::kSnowball})}});
    QComboBox* combo = comboOf(rig.home(), keys::audio_input_priority.key());
    CHECK(itemTexts(combo) ==
          QStringList{qs(fv::audio::display_name("Microphone (Blue Snowball )"))});
    CHECK(combo->currentData().toString() == token(fvtest::kSnowball));
    CHECK(rig.listings == 0);
}

// --- AI cleanup and the text read near the cursor in the where strip (A11-90) ---

namespace {

// The strip's entries on one side, as the user reads them.
QStringList whereItems(const fv::WrappedLabel* list) {
    return list->text().split(QStringLiteral(" · "));
}

} // namespace

// from
// tests/test_settings_unified.py::TestHomeSummary::test_where_strip_places_a_remote_cleanup_provider_in_the_cloud
TEST_CASE("test_where_strip_places_a_remote_cleanup_provider_in_the_cloud",
          "[test_settings_unified][widget]") {
    HomeRig rig(fv::Json{{"transcript_cleanup_provider", "openrouter"},
                         {"transcript_cleanup_model", "openrouter/free"}});
    fv::HomePage& home = rig.home();
    home.refreshHome();
    CHECK(home.cloudList()->text().contains(QStringLiteral("AI cleanup")));
    CHECK_FALSE(home.localList()->text().contains(QStringLiteral("AI cleanup")));
}

// from
// tests/test_settings_unified.py::TestHomeSummary::test_where_strip_keeps_a_local_cleanup_provider_on_this_computer
TEST_CASE("test_where_strip_keeps_a_local_cleanup_provider_on_this_computer",
          "[test_settings_unified][widget]") {
    HomeRig rig(fv::Json{{"transcript_cleanup_provider", "ollama"},
                         {"transcript_cleanup_model", "llama3.2"}});
    fv::HomePage& home = rig.home();
    home.refreshHome();
    CHECK(home.localList()->text().contains(QStringLiteral("AI cleanup")));
    CHECK_FALSE(home.cloudList()->text().contains(QStringLiteral("AI cleanup")));
}

// from tests/test_personalize_s1_settings_page.py::test_home_privacy_strip
TEST_CASE("test_home_privacy_strip", "[test_personalize_s1_settings_page][widget]") {
    struct Row {
        fv::Json values;
        bool local;
        const char* item;
    };
    const Row row =
        GENERATE(Row{fv::Json::object(), true, "Text near the cursor (off)"},
                 Row{fv::Json{{"app_context_read_text", true},
                              {"transcript_cleanup_provider", "openrouter"},
                              {"transcript_cleanup_model", "openrouter/free"}},
                     false, "Text near the cursor (with AI cleanup)"},
                 Row{fv::Json{{"app_context_read_text", true},
                              {"transcript_cleanup_provider", "ollama"},
                              {"transcript_cleanup_model", "llama3.2"}},
                     true, "Text near the cursor"},
                 Row{fv::Json{{"app_context_read_text", true}, {"app_context_enabled", false}},
                     true, "Text near the cursor (off)"});
    HomeRig rig(row.values);
    // A computer that can read text near the cursor (Windows); the Linux build cannot.
    rig.context().service<fv::HomeSources>().textReadingSupported = [] {
        return true;
    };
    fv::HomePage& home = rig.home();
    home.refreshHome();
    const QStringList local = whereItems(home.localList());
    const QStringList cloud = whereItems(home.cloudList());
    CHECK((row.local ? local : cloud).contains(QString::fromUtf8(row.item)));
    for (const QString& entry : row.local ? cloud : local) {
        CHECK_FALSE(entry.startsWith(QStringLiteral("Text near")));
    }
}

TEST_CASE("the cleanup item says whether cleanup is on, wherever it runs", "[widget]") {
    // The level decides: a saved level with the switch off is off, and Light is on.
    const auto [values, item] =
        GENERATE(std::pair{fv::Json{{"transcript_cleanup_provider", "ollama"},
                                    {"transcript_cleanup_model", "m"},
                                    {"transcript_cleanup_enabled", true}},
                           "AI cleanup (on)"},
                 std::pair{fv::Json{{"transcript_cleanup_provider", "ollama"},
                                    {"transcript_cleanup_model", "m"},
                                    {"transcript_cleanup_enabled", false},
                                    {"transcript_cleanup_level", "high"}},
                           "AI cleanup (off)"},
                 std::pair{fv::Json{{"transcript_cleanup_provider", "ollama"},
                                    {"transcript_cleanup_model", "m"},
                                    {"transcript_cleanup_enabled", true},
                                    {"transcript_cleanup_level", "light"}},
                           "AI cleanup (on)"});
    HomeRig rig(values);
    fv::HomePage& home = rig.home();
    home.refreshHome();
    CHECK(whereItems(home.localList()).contains(QString::fromUtf8(item)));
}

TEST_CASE("an installed agent or a deleted endpoint counts as a remote cleanup provider",
          "[widget]") {
    const fv::Json values = GENERATE(fv::Json{{"transcript_cleanup_provider", "claude_code"},
                                              {"transcript_cleanup_model", "sonnet"}},
                                     fv::Json{{"transcript_cleanup_provider", "custom_gone"},
                                              {"transcript_cleanup_model", "m"}});
    HomeRig rig(values);
    fv::HomePage& home = rig.home();
    home.refreshHome();
    CHECK(whereItems(home.cloudList()).contains(QStringLiteral("AI cleanup (off)")));
}

TEST_CASE("a local server the user moved to another computer counts as remote", "[widget]") {
    HomeRig rig(fv::Json{{"transcript_cleanup_provider", "ollama"},
                         {"transcript_cleanup_model", "m"},
                         {"ollama_base_url", "http://gpu-box:11434/v1"}});
    fv::HomePage& home = rig.home();
    home.refreshHome();
    CHECK(whereItems(home.cloudList()).contains(QStringLiteral("AI cleanup (off)")));
    CHECK_FALSE(whereItems(home.localList()).contains(QStringLiteral("AI cleanup (off)")));
}
