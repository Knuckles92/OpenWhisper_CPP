// The AI cleanup page, its level, custom prompt and switch, Home's cleanup rows, the Learned
// rules gate and the rail, inside a real Settings window over fakes.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <string>
#include <vector>

#include <QAbstractButton>
#include <QComboBox>
#include <QEventLoop>
#include <QFocusEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMetaMethod>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>

#include "cleanup_support.h"
#include "settings/keys.h"
#include "text/cleanup_prompts.h"
#include "ui_personalize/cleanup_gates.h"
#include "ui_personalize/cleanup_profiles_panel.h"
#include "ui_settings/settings_window.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;
namespace levels = fv::text::cleanup_levels;

namespace {

constexpr const char* kEnabled = "transcript_cleanup_enabled";
constexpr const char* kLevel = "transcript_cleanup_level";
constexpr const char* kPrompt = "transcript_cleanup_prompt";
const std::string kCustom = "Rewrite it as a friendly Slack message.";

QString preset(std::string_view level) {
    return qs(fv::text::preset(level));
}

QString note(std::string_view level) {
    return qs(levels::note(level));
}

Json withModel(Json values) {
    values["transcript_cleanup_provider"] = "openai";
    values["transcript_cleanup_model"] = "gpt-test";
    return values;
}

bool stored(CleanupRig& rig, const char* key) {
    return rig.settings().snapshot()->contains(key);
}

void focusOut(QWidget* widget) {
    QFocusEvent event(QEvent::FocusOut);
    QApplication::sendEvent(widget, &event);
}

QStringList comboData(QComboBox* combo) {
    QStringList items;
    for (int i = 0; i < combo->count(); ++i) {
        items << combo->itemData(i).toString();
    }
    return items;
}

void chooseOnHome(QComboBox* combo, std::string_view level) {
    combo->setCurrentIndex(combo->findData(qs(level)));
    Q_EMIT combo->activated(combo->currentIndex());
}

QAbstractButton* checkOf(CleanupRig& rig) {
    auto* check = qobject_cast<QAbstractButton*>(rig.window().control("transcript_cleanup_check"));
    REQUIRE(check != nullptr);
    return check;
}

} // namespace

// --- the level bar ---

// from
// tests/test_personalize_s2_settings.py::test_the_bar_shows_the_saved_level_and_saves_only_clicks
TEST_CASE("test_the_bar_shows_the_saved_level_and_saves_only_clicks",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "high"}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    CHECK(rig.selected() == "high");
    CHECK(page.levelNote()->text() == note("high"));
    CHECK_FALSE(page.levelBar()->buttons().back()->isEnabled());
    CHECK(page.promptEdit()->toPlainText().isEmpty());
    CHECK(page.promptEdit()->placeholderText() == preset("high"));

    page.levelBar()->setCurrentIndex(static_cast<int>(levels::segment_index("light")));
    CHECK(rig.saved(kLevel) == "high");

    rig.answer = QMessageBox::Yes;
    rig.clickLevel("light");
    CHECK(rig.asked.isEmpty());
    CHECK(rig.saved(kLevel) == "light");
    CHECK(rig.selected() == "light");
    CHECK(page.levelNote()->text() == note("light"));
    CHECK(page.promptEdit()->placeholderText() == preset("light"));
    CHECK(rig.railValue(dest::cleanup) == QStringLiteral(u"Light · openrouter/free"));
}

// from tests/test_personalize_s2_settings.py::test_the_level_follows_the_switch
TEST_CASE("test_the_level_follows_the_switch", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, false}});
    fv::CleanupPage& page = rig.builtCleanupPage();
    CHECK_FALSE(page.levelTile()->isEnabled());
    CHECK(rig.selected() == "medium");

    page.cleanupCheck()->setChecked(true);

    CHECK(page.levelTile()->isEnabled());
    CHECK(rig.saved(kEnabled) == true);
    CHECK_FALSE(stored(rig, kLevel));
}

// from tests/test_personalize_s2_settings.py::test_a_saved_custom_prompt_shows_as_custom
TEST_CASE("test_a_saved_custom_prompt_shows_as_custom", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig(withModel({{kEnabled, true}, {kPrompt, "  " + kCustom + " "}}));
    fv::CleanupPage& page = rig.builtCleanupPage();

    CHECK(rig.selected() == levels::CUSTOM);
    CHECK(page.levelBar()->buttons().back()->isEnabled());
    CHECK(page.levelNote()->text() == note(levels::CUSTOM));
    CHECK(page.promptEdit()->toPlainText() == qs(kCustom));
    CHECK(page.promptResetButton()->isEnabled());
    CHECK(rig.railValue(dest::cleanup) == QStringLiteral(u"Custom · gpt-test"));
}

// from tests/test_personalize_s2_settings.py::test_a_saved_built_in_prompt_is_not_custom
TEST_CASE("test_a_saved_built_in_prompt_is_not_custom", "[test_personalize_s2_settings][widget]") {
    const std::string saved =
        GENERATE(fv::text::legacy_default_prompt(), fv::text::preset(std::string_view("high")),
                 std::string("   "));
    CleanupRig rig({{kEnabled, true}, {kPrompt, saved}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    CHECK(rig.selected() == "medium");
    CHECK(page.promptEdit()->toPlainText().isEmpty());
    CHECK_FALSE(page.promptResetButton()->isEnabled());
}

// from tests/test_personalize_s2_settings.py::test_declining_keeps_the_custom_prompt
TEST_CASE("test_declining_keeps_the_custom_prompt", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "light"}, {kPrompt, kCustom}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    rig.answer = QMessageBox::No;
    rig.clickLevel("high");

    CHECK(rig.asked.size() == 1);
    CHECK(rig.saved(kPrompt) == kCustom);
    CHECK(rig.saved(kLevel) == "light");
    CHECK(rig.selected() == levels::CUSTOM);
    CHECK(page.promptEdit()->toPlainText() == qs(kCustom));
}

// from tests/test_personalize_s2_settings.py::test_declining_in_a_real_dialog_still_shows_custom
TEST_CASE("test_declining_in_a_real_dialog_still_shows_custom",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kPrompt, kCustom}});
    rig.builtCleanupPage();
    // A real QMessageBox runs an event loop of its own before answering.
    rig.cleanup().question = [](QWidget*, const QString&, const QString&,
                                QMessageBox::StandardButtons, QMessageBox::StandardButton) {
        QEventLoop loop;
        QTimer::singleShot(30, &loop, &QEventLoop::quit);
        loop.exec();
        return QMessageBox::No;
    };
    rig.clickLevel("light");

    CHECK(rig.selected() == levels::CUSTOM);
}

// from
// tests/test_personalize_s2_settings.py::test_accepting_removes_the_custom_prompt_and_saves_the_level
TEST_CASE("test_accepting_removes_the_custom_prompt_and_saves_the_level",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "light"}, {kPrompt, kCustom}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    rig.answer = QMessageBox::Yes;
    rig.clickLevel("high");

    CHECK_FALSE(stored(rig, kPrompt));
    CHECK(rig.saved(kLevel) == "high");
    CHECK(rig.selected() == "high");
    CHECK_FALSE(page.levelBar()->buttons().back()->isEnabled());
    CHECK(page.promptEdit()->toPlainText().isEmpty());
    CHECK(page.promptEdit()->placeholderText() == preset("high"));
}

// from
// tests/test_personalize_s2_settings.py::test_clicking_custom_while_it_is_in_use_changes_nothing
TEST_CASE("test_clicking_custom_while_it_is_in_use_changes_nothing",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kPrompt, kCustom}});
    rig.builtCleanupPage();

    rig.answer = QMessageBox::Yes;
    rig.clickLevel(levels::CUSTOM);

    CHECK(rig.asked.isEmpty());
    CHECK(rig.saved(kPrompt) == kCustom);
    CHECK(rig.selected() == levels::CUSTOM);
}

// --- the custom prompt ---

// from tests/test_personalize_s2_settings.py::test_typing_a_prompt_makes_it_custom_on_focus_out
TEST_CASE("test_typing_a_prompt_makes_it_custom_on_focus_out",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    page.promptEdit()->setPlainText(qs(kCustom) + QStringLiteral("\n"));
    focusOut(page.promptEdit());

    CHECK(rig.saved(kPrompt) == kCustom);
    CHECK(rig.selected() == levels::CUSTOM);
    CHECK(rig.railValue(dest::cleanup) == QStringLiteral(u"Custom · openrouter/free"));
}

// from tests/test_personalize_s2_settings.py::test_empty_or_built_in_text_removes_the_prompt
TEST_CASE("test_empty_or_built_in_text_removes_the_prompt",
          "[test_personalize_s2_settings][widget]") {
    const std::string text =
        GENERATE(std::string(), std::string("  \n"), fv::text::preset(std::string_view("medium")),
                 fv::text::legacy_default_prompt());
    CleanupRig rig({{kEnabled, true}, {kPrompt, kCustom}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    page.promptEdit()->setPlainText(qs(text));
    focusOut(page.promptEdit());

    CHECK_FALSE(stored(rig, kPrompt));
    CHECK(page.promptEdit()->toPlainText().isEmpty());
    CHECK(rig.selected() == "medium");
}

// from tests/test_personalize_s2_settings.py::test_closing_never_saves_preset_text
TEST_CASE("test_closing_never_saves_preset_text", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}});
    rig.builtCleanupPage();

    rig.window().close();

    CHECK_FALSE(stored(rig, kPrompt));
}

// from tests/test_personalize_s2_settings.py::test_reset_asks_then_removes_the_key
TEST_CASE("test_reset_asks_then_removes_the_key", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "high"}, {kPrompt, kCustom}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    rig.answer = QMessageBox::No;
    page.promptResetButton()->click();
    CHECK(rig.saved(kPrompt) == kCustom);

    rig.answer = QMessageBox::Yes;
    page.promptResetButton()->click();

    REQUIRE_FALSE(rig.asked.isEmpty());
    CHECK(rig.asked.back().contains(QStringLiteral("High")));
    CHECK_FALSE(stored(rig, kPrompt));
    CHECK(rig.saved(kLevel) == "high");
    CHECK(page.promptEdit()->toPlainText().isEmpty());
    CHECK_FALSE(page.promptResetButton()->isEnabled());
    CHECK(rig.selected() == "high");
}

// from
// tests/test_personalize_s2_settings.py::test_reset_also_clears_an_old_default_saved_by_earlier_versions
TEST_CASE("test_reset_also_clears_an_old_default_saved_by_earlier_versions",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kPrompt, fv::text::legacy_default_prompt()}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    rig.answer = QMessageBox::Yes;
    page.resetCleanupPrompt();

    CHECK(rig.asked.isEmpty());
    CHECK_FALSE(stored(rig, kPrompt));
}

// from tests/test_personalize_s2_settings.py::test_the_editor_starts_from_the_levels_instructions
TEST_CASE("test_the_editor_starts_from_the_levels_instructions",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "light"}});
    fv::CleanupPage& page = rig.builtCleanupPage();
    rig.editorAnswer = preset("light") + QStringLiteral(" Use British spelling.");

    page.openCleanupPromptEditor();

    CHECK(rig.editorOpened == QStringList{preset("light")});
    CHECK(rig.saved(kPrompt) ==
          fv::text::preset(std::string_view("light")) + " Use British spelling.");
    CHECK(rig.selected() == levels::CUSTOM);
}

// --- rail, Home and the Learned rules gate ---

// from tests/test_personalize_s2_settings.py::test_rail_and_home_name_the_level
TEST_CASE("test_rail_and_home_name_the_level", "[test_personalize_s2_settings][widget]") {
    struct Case {
        Json values;
        const char16_t* rail;
        const char* level;
    };
    const Case example = GENERATE(
        Case{Json::object(), u"Off", ""},
        Case{withModel({{kEnabled, true}}), u"Medium · gpt-test", "Medium"},
        Case{withModel({{kEnabled, true}, {kLevel, "high"}}), u"High · gpt-test", "High"},
        Case{withModel({{kEnabled, true}, {kPrompt, kCustom}}), u"Custom · gpt-test", "Custom"},
        Case{Json{{kPrompt, kCustom}, {kLevel, "light"}}, u"Off", ""});
    CleanupRig rig(example.values);
    rig.window().selectDestination(qs(dest::overview));

    CHECK(rig.railValue(dest::cleanup) == QString::fromUtf16(example.rail));
    QComboBox* combo = rig.homeLevelCombo();
    const QString level = QString::fromUtf8(example.level);
    CHECK(combo->isEnabled() == !level.isEmpty());
    if (!level.isEmpty()) {
        CHECK(combo->currentText() == level);
    }
}

// from
// tests/test_personalize_s2_settings.py::test_the_learned_rules_gate_turns_cleanup_on_at_the_saved_level
TEST_CASE("test_the_learned_rules_gate_turns_cleanup_on_at_the_saved_level",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kLevel, "high"}});
    fv::RulesPage& page = rig.builtRulesPage();
    fv::Button* button = page.turnOnButton();

    CHECK_FALSE(page.gateTile()->isHidden());
    CHECK(button->text() == QStringLiteral("Turn on (High)"));
    CHECK_FALSE(page.section()->composerTile()->isEnabled());

    button->click();

    CHECK(rig.saved(kEnabled) == true);
    CHECK(rig.saved(kLevel) == "high");
    CHECK(checkOf(rig)->isChecked());
    CHECK(page.gateTile()->isHidden());
    CHECK(page.section()->composerTile()->isEnabled());
    CHECK(rig.railValue(dest::cleanup) == QStringLiteral(u"High · openrouter/free"));
}

// from
// tests/test_personalize_s2_settings.py::test_a_gate_on_another_page_turns_cleanup_on_without_building_it
TEST_CASE("test_a_gate_on_another_page_turns_cleanup_on_without_building_it",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kLevel, "light"}});
    fv::SettingsDialog& dialog = rig.window();
    // As Apps & styles would: a gate on a page of its own.
    fv::InfoTile* gate = rig.cleanup().cleanupGateTile(
        QStringLiteral("Styles only change text while AI cleanup runs."));
    dialog.page(dest::overview)->pageLayout()->insertWidget(0, gate);
    rig.cleanup().updateCleanupUi();
    fv::Button* button = rig.gates().turnOnButton(gate);
    REQUIRE(button != nullptr);
    CHECK_FALSE(gate->isHidden());
    CHECK(button->text() == QStringLiteral("Turn on (Light)"));

    button->click();

    CHECK(rig.saved(kEnabled) == true);
    CHECK(rig.saved(kLevel) == "light");
    CHECK(gate->isHidden());
    CHECK_FALSE(dialog.isBuilt(dest::cleanup));
}

// --- Home ---

// from tests/test_personalize_s2_settings.py::test_basic_shows_the_level_under_the_switch
TEST_CASE("test_basic_shows_the_level_under_the_switch", "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kLevel, "high"}});
    rig.window();
    QComboBox* combo = rig.homeLevelCombo();

    CHECK(comboData(combo) ==
          QStringList{QStringLiteral("light"), QStringLiteral("medium"), QStringLiteral("high")});
    CHECK(combo->currentData().toString() == QStringLiteral("high"));
    CHECK(combo->isEnabled());
    CHECK(rig.homeDetails().contains(qs(levels::short_note("high"))));
    CHECK_FALSE(rig.window().isBuilt(dest::cleanup));
}

// from tests/test_personalize_s2_settings.py::test_basic_level_is_disabled_while_cleanup_is_off
TEST_CASE("test_basic_level_is_disabled_while_cleanup_is_off",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig;
    rig.window();
    QComboBox* combo = rig.homeLevelCombo();

    CHECK_FALSE(combo->isEnabled());
    CHECK(combo->currentData().toString() == QStringLiteral("medium"));
}

// from
// tests/test_personalize_s2_settings.py::test_basic_saves_a_level_without_building_the_cleanup_page
TEST_CASE("test_basic_saves_a_level_without_building_the_cleanup_page",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}});
    rig.window();
    QComboBox* combo = rig.homeLevelCombo();

    rig.answer = QMessageBox::Yes;
    chooseOnHome(combo, "light");

    CHECK(rig.asked.isEmpty());
    CHECK(rig.saved(kLevel) == "light");
    CHECK(rig.homeDetails().contains(qs(levels::short_note("light"))));
    CHECK_FALSE(rig.window().isBuilt(dest::cleanup));
}

// from tests/test_personalize_s2_settings.py::test_basic_asks_before_replacing_a_custom_prompt
TEST_CASE("test_basic_asks_before_replacing_a_custom_prompt",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}, {kPrompt, kCustom}});
    rig.window();
    QComboBox* combo = rig.homeLevelCombo();
    CHECK(comboData(combo) == QStringList{QStringLiteral("light"), QStringLiteral("medium"),
                                          QStringLiteral("high"), QStringLiteral("custom")});
    CHECK(combo->currentData().toString() == QStringLiteral("custom"));

    rig.answer = QMessageBox::No;
    chooseOnHome(combo, "high");
    CHECK(rig.saved(kPrompt) == kCustom);
    CHECK(combo->currentData().toString() == QStringLiteral("custom"));

    rig.answer = QMessageBox::Yes;
    chooseOnHome(combo, "high");
    CHECK_FALSE(stored(rig, kPrompt));
    CHECK(rig.saved(kLevel) == "high");
    CHECK(comboData(combo) ==
          QStringList{QStringLiteral("light"), QStringLiteral("medium"), QStringLiteral("high")});
    CHECK(combo->currentData().toString() == QStringLiteral("high"));
}

// from tests/test_personalize_s2_settings.py::test_basic_follows_a_change_made_on_the_cleanup_page
TEST_CASE("test_basic_follows_a_change_made_on_the_cleanup_page",
          "[test_personalize_s2_settings][widget]") {
    CleanupRig rig({{kEnabled, true}});
    rig.window();
    QComboBox* combo = rig.homeLevelCombo();
    rig.builtCleanupPage();

    rig.clickLevel("high");

    CHECK(combo->currentData().toString() == QStringLiteral("high"));
}

// --- layout ---

// from tests/test_personalize_s2_settings.py::test_cleanup_tiles_fit_narrow_windows_at_large_fonts
TEST_CASE("test_cleanup_tiles_fit_narrow_windows_at_large_fonts",
          "[test_personalize_s2_settings][widget]") {
    // The classic window at its 940 px floor; Omarchy's 720 px variant is Linux's.
    AppTheme theme(QStringLiteral("dark"), 130);
    CleanupRig rig({{kPrompt, kCustom}});
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    dialog.fitToScreen();
    dialog.resize(940, 640);
    const auto checkTiles = [&](std::string_view key, const std::vector<std::string>& names) {
        dialog.selectDestination(qs(key));
        processEvents(8);
        QWidget* page = dialog.page(key);
        for (const std::string& name : names) {
            QWidget* tile = dialog.control(name);
            INFO(name);
            REQUIRE(tile != nullptr);
            CHECK(tile->isVisible());
            for (QAbstractButton* control : tile->findChildren<QAbstractButton*>()) {
                if (!control->isVisible()) {
                    continue;
                }
                INFO(control->text().toStdString());
                CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
                CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
                CHECK(control->width() >= control->minimumSizeHint().width());
            }
            for (QLabel* label : tile->findChildren<QLabel*>()) {
                if (label->isVisible() && label->wordWrap()) {
                    INFO(label->text().toStdString());
                    CHECK(label->height() >= label->heightForWidth(label->width()));
                }
            }
        }
    };
    checkTiles(dest::cleanup, {"cleanup_level_tile", "cleanup_prompt_tile"});
    checkTiles(dest::cleanup_rules, {"cleanup_rules_gate_tile"});
    dialog.close();
}

// --- the switch gates the other tiles ---

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_cleanup_toggle_gates_the_prompt_and_rule_tiles
TEST_CASE("test_cleanup_toggle_gates_the_prompt_and_rule_tiles",
          "[test_settings_general_layout][widget]") {
    CleanupRig rig;
    fv::CleanupPage& cleanup = rig.builtCleanupPage();
    fv::RulesPage& rules = rig.builtRulesPage();

    cleanup.cleanupCheck()->setChecked(true);
    CHECK(cleanup.levelTile()->isEnabled());
    CHECK(cleanup.promptTile()->isEnabled());
    CHECK(rules.section()->composerTile()->isEnabled());
    CHECK(rules.gateTile()->isHidden());

    cleanup.cleanupCheck()->setChecked(false);
    for (QWidget* tile :
         std::vector<QWidget*>{cleanup.levelTile(), cleanup.promptTile(),
                               rules.section()->composerTile(), rules.section()->libraryTile()}) {
        CHECK_FALSE(tile->isEnabled());
    }
    CHECK_FALSE(rules.section()->addButton()->isEnabled());
    // The cleanup model stays editable so it can be set up before turning cleanup on.
    CHECK(cleanup.modelSection()->isEnabled());
    CHECK(cleanup.modelSection()->picker()->isEnabled());
    CHECK_FALSE(rules.gateTile()->isHidden());
    CHECK(rules.gateTile()->isEnabled());
    CHECK(rules.turnOnButton()->isEnabled());
    CHECK(rules.turnOnButton()->text() == QStringLiteral("Turn on (Medium)"));
    rig.window().close();
}

// from
// tests/test_settings_general_layout.py::TestSettingsGeneralLayout::test_learned_rules_gate_button_turns_cleanup_on
TEST_CASE("test_learned_rules_gate_button_turns_cleanup_on",
          "[test_settings_general_layout][widget]") {
    CleanupRig rig;
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    processEvents();
    checkOf(rig)->setChecked(false);
    fv::RulesPage& rules = rig.rulesPage();

    rules.turnOnButton()->click();
    processEvents();

    CHECK(dialog.currentDestination() == qs(dest::cleanup_rules));
    CHECK(checkOf(rig)->isChecked());
    CHECK(rules.gateTile()->isHidden());
    CHECK(rules.section()->composerTile()->isEnabled());
    dialog.close();
}

// --- links and the rail ---

// from
// tests/test_settings_unified.py::TestPages::test_cleanup_profiles_model_link_opens_the_cleanup_model
TEST_CASE("test_cleanup_profiles_model_link_opens_the_cleanup_model",
          "[test_settings_unified][widget]") {
    CleanupRig rig;
    fv::CleanupProfilesPanel& panel = rig.profilesPanel();

    Q_EMIT panel.modelRequested();

    CHECK(rig.window().currentDestination() == qs(dest::cleanup));
}

// from
// tests/test_settings_unified.py::TestUiControllerDestinations::test_text_alias_scrolls_to_the_cleanup_model
TEST_CASE("test_text_alias_scrolls_to_the_cleanup_model", "[test_settings_unified][widget]") {
    CleanupRig rig;
    fv::SettingsWindow window(rig.context());
    int raised = 0;
    window.raiseDialog = [&raised](fv::SettingsDialog&) {
        ++raised;
    };

    window.open(QStringLiteral("text"));

    REQUIRE(window.dialog() != nullptr);
    CHECK(window.dialog()->currentDestination() == qs(dest::cleanup));
    CHECK(window.dialog()->isBuilt(dest::cleanup));
    CHECK(raised == 1);
    delete window.dialog();
}

// from
// tests/test_settings_unified.py::TestRailValues::test_cleanup_rail_combines_the_level_and_the_hosted_model
TEST_CASE("test_cleanup_rail_combines_the_level_and_the_hosted_model",
          "[test_settings_unified][widget]") {
    CleanupRig rig(withModel({{kEnabled, true}}));
    rig.window();
    CHECK(rig.railValue(dest::cleanup) == QStringLiteral(u"Medium · gpt-test"));

    checkOf(rig)->setChecked(false);

    CHECK(rig.railValue(dest::cleanup) == QStringLiteral("Off"));
}

// from
// tests/test_desktop_performance.py::test_initializing_another_settings_page_preserves_unfinished_prompt
TEST_CASE("test_initializing_another_settings_page_preserves_unfinished_prompt",
          "[test_desktop_performance][widget]") {
    CleanupRig rig({{kPrompt, "Saved prompt"}});
    fv::CleanupPage& page = rig.builtCleanupPage();

    page.promptEdit()->setPlainText(QStringLiteral("Unfinished prompt draft"));
    rig.window().ensurePage(dest::general);

    CHECK(page.promptEdit()->toPlainText() == QStringLiteral("Unfinished prompt draft"));
    CHECK(rig.saved(kPrompt) == "Saved prompt");
}

// from
// tests/test_personalize_foundation_ui_settings.py::test_new_pages_fit_narrow_windows_at_large_fonts
TEST_CASE("test_new_pages_fit_narrow_windows_at_large_fonts",
          "[test_personalize_foundation_ui_settings][widget]") {
    // The classic window at its 940 px floor; Omarchy's 560 px variant is Linux's.
    AppTheme theme(QStringLiteral("dark"), 130);
    CleanupRig rig;
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    dialog.resize(940, 600);
    for (const std::string_view key :
         {dest::overview, dest::cleanup, dest::cleanup_rules, dest::cleanup_profiles,
          dest::dictionary, dest::snippets, dest::styles, dest::commands}) {
        INFO(std::string(key));
        dialog.selectDestination(qs(key));
        processEvents(8);
        CHECK(dialog.width() == 940);
        QWidget* page = dialog.page(key);
        QList<QWidget*> controls;
        for (QAbstractButton* button : page->findChildren<QAbstractButton*>()) {
            controls << button;
        }
        for (QComboBox* combo : page->findChildren<QComboBox*>()) {
            controls << combo;
        }
        for (QLineEdit* edit : page->findChildren<QLineEdit*>()) {
            controls << edit;
        }
        for (QWidget* control : controls) {
            if (!control->isVisible()) {
                continue;
            }
            INFO(control->objectName().toStdString());
            CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
            CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
        }
        for (fv::TileBase* tile : page->findChildren<fv::TileBase*>()) {
            if (tile->isHidden()) {
                continue; // a tile shown only in some states
            }
            for (fv::WrappedLabel* label : {tile->titleLabel(), tile->descriptionLabel()}) {
                CHECK(label->height() >= label->heightForWidth(label->width()));
                CHECK(tile->rect().contains(label->mapTo(tile, label->rect().bottomRight())));
            }
        }
    }
    dialog.close();
}

// --- Apps & styles follows the cleanup switch ---

namespace {

bool stylesLocked(fv::StylesPage& page) {
    return !page.appStylesTile()->isEnabled();
}

void go(CleanupRig& rig, std::string_view key) {
    rig.window().selectDestination(qs(key));
    processEvents(3);
}

Json openRouter() {
    return Json{{"transcript_cleanup_provider", "openrouter"},
                {"transcript_cleanup_model", "openrouter/free"}};
}

} // namespace

// from
// tests/test_personalize_fix_f4_styles_gate.py::test_cleanup_turned_on_on_its_own_page_unlocks_styles
TEST_CASE("test_cleanup_turned_on_on_its_own_page_unlocks_styles",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    CleanupRig rig(openRouter());
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    CHECK(stylesLocked(page));

    go(rig, dest::cleanup);
    checkOf(rig)->setChecked(true);
    CHECK(rig.saved(kEnabled) == true);
    go(rig, dest::styles);

    CHECK_FALSE(stylesLocked(page));
    CHECK(page.appStylesTile()->isEnabled());
    CHECK(page.privacy()->text().contains(QStringLiteral("OpenRouter")));
    rig.window().hide();
}

// from
// tests/test_personalize_fix_f4_styles_gate.py::test_cleanup_turned_off_on_its_own_page_locks_styles
TEST_CASE("test_cleanup_turned_off_on_its_own_page_locks_styles",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    Json values = openRouter();
    values[kEnabled] = true;
    values[kLevel] = "medium";
    CleanupRig rig(values);
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    CHECK_FALSE(stylesLocked(page));

    go(rig, dest::cleanup);
    checkOf(rig)->setChecked(false);
    go(rig, dest::styles);

    CHECK(stylesLocked(page));
    CHECK(page.privacy()->text().startsWith(QStringLiteral("AI cleanup is off")));
    rig.window().hide();
}

// from tests/test_personalize_fix_f4_styles_gate.py::test_learned_rules_turn_on_unlocks_styles
TEST_CASE("test_learned_rules_turn_on_unlocks_styles",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    CleanupRig rig;
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    fv::RulesPage& rules = rig.rulesPage();
    processEvents(3);
    QTest::mouseClick(rules.turnOnButton(), Qt::LeftButton);
    go(rig, dest::styles);

    CHECK_FALSE(stylesLocked(page));
    rig.window().hide();
}

// from
// tests/test_personalize_fix_f4_styles_gate.py::test_provider_changed_while_away_shows_on_return
TEST_CASE("test_provider_changed_while_away_shows_on_return",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    Json values = openRouter();
    values[kEnabled] = true;
    values[kLevel] = "medium";
    CleanupRig rig(values);
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    CHECK(page.privacy()->text().contains(QStringLiteral("OpenRouter")));

    go(rig, dest::commands);
    rig.settings().update(
        Json{{"transcript_cleanup_provider", "ollama"}, {"transcript_cleanup_model", "llama3.2"}});
    go(rig, dest::styles);

    CHECK(page.privacy()->text().contains(QStringLiteral("on this computer")));
    rig.window().hide();
}

// --- Home's status line ---

// The line under Home's cleanup rows (settings_home.py _cleanup_status), with the key read off
// the GUI thread.
TEST_CASE("Home's cleanup status says where transcript text goes", "[widget]") {
    CleanupRig rig({{"transcript_cleanup_provider", "openrouter"},
                    {"transcript_cleanup_model", "openrouter/free"}});
    rig.window();
    fv::WrappedLabel* status =
        rig.home().findChild<fv::WrappedLabel*>(QStringLiteral("homeStatus"));
    REQUIRE(status != nullptr);
    const QString goes =
        QStringLiteral(u"OpenRouter · Transcript text goes to this provider when cleanup is on.");
    CHECK(pumpUntil([&] { return status->text() == goes + QStringLiteral(" API key needed."); }));

    rig.settings().update(
        Json{{"transcript_cleanup_provider", "ollama"}, {"transcript_cleanup_model", "llama3.2"}});
    rig.window().refresh();
    CHECK(status->text() == QStringLiteral(u"Ollama · Transcript text stays on this computer."));
}

// "<feature> connects every signal" (architecture 8.5) for the classes of the AI cleanup pages:
// only CleanupSettings declares signals, and they are the app's (the controller connects them,
// as OpenWhisper's UIController assigns on_cleanup_changed, on_cleanup_profiles_changed and
// on_profile_hotkey_capture); the window's own changes reach them.
TEST_CASE("the AI cleanup pages declare only CleanupSettings' signals, which follow the window",
          "[widget]") {
    const auto declared = [](const QMetaObject& meta) {
        int count = 0;
        for (int i = meta.methodOffset(); i < meta.methodCount(); ++i) {
            count += meta.method(i).methodType() == QMetaMethod::Signal ? 1 : 0;
        }
        return count;
    };
    for (const QMetaObject* meta :
         {&fv::CleanupPage::staticMetaObject, &fv::RulesPage::staticMetaObject,
          &fv::CleanupModelSection::staticMetaObject, &fv::CleanupRulesSection::staticMetaObject}) {
        INFO(meta->className());
        CHECK(declared(*meta) == 0);
    }
    CHECK(declared(fv::CleanupSettings::staticMetaObject) == 3);

    CleanupRig rig;
    int changed = 0;
    int profiles = 0;
    std::vector<bool> captures;
    QObject::connect(&rig.cleanup(), &fv::CleanupSettings::cleanupChanged,
                     [&changed] { ++changed; });
    QObject::connect(&rig.cleanup(), &fv::CleanupSettings::cleanupProfilesChanged,
                     [&profiles] { ++profiles; });
    QObject::connect(&rig.cleanup(), &fv::CleanupSettings::profileHotkeyCapture,
                     [&captures](bool capturing) { captures.push_back(capturing); });
    rig.window();

    checkOf(rig)->setChecked(true);
    rig.window().notifyChanged(QStringLiteral("cleanup_profiles"));
    rig.window().setHotkeyCaptureSuspended(true);

    CHECK(changed == 1);
    CHECK(profiles == 1);
    CHECK(captures == std::vector<bool>{true});
    CHECK(rig.changes.contains(QStringLiteral("cleanup")));
}

// The AI cleanup level chosen from outside a Settings window (guided setup) saves straight to
// the settings file.
TEST_CASE("chooseCleanupLevel without a Settings window saves to the settings file", "[widget]") {
    CleanupRig rig({{kEnabled, true}, {kPrompt, kCustom}});
    int changed = 0;
    QObject::connect(&rig.cleanup(), &fv::CleanupSettings::cleanupChanged,
                     [&changed] { ++changed; });

    rig.answer = QMessageBox::No;
    CHECK_FALSE(rig.cleanup().chooseCleanupLevel(QStringLiteral("high")));
    CHECK(rig.saved(kPrompt) == kCustom);
    rig.answer = QMessageBox::Yes;
    CHECK(rig.cleanup().chooseCleanupLevel(QStringLiteral("high")));

    CHECK(rig.saved(kLevel) == "high");
    CHECK_FALSE(stored(rig, kPrompt));
    CHECK(changed == 1);
    REQUIRE(rig.asked.size() == 2);
    CHECK(rig.asked.back() == QStringLiteral("Remove your custom prompt?: AI cleanup will go back "
                                             "to the High level's instructions. Your learned "
                                             "rules stay."));
}
