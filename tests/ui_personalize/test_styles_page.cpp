// Settings > Personalize > Apps & styles: app awareness, text near the cursor, the apps never read
// from, tones and app moves, the Home row, keyboard focus in the app lists and Enter in the
// pickers (tests/test_personalize_s1_settings_page.py, test_personalize_fix2_g2_styles_focus.py,
// test_personalize_fix3_h1_basic_tone_copy.py, test_personalize_fix_f4_styles_gate.py and
// test_personalize_fix2_g2_enter_key.py), inside a real Settings window over an in-memory
// settings file. Text reading is pinned supported, as the Python tests pin it.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QAbstractButton>
#include <QAbstractItemView>
#include <QComboBox>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QTest>

#include "focus/snapshot.h"
#include "settings/keys.h"
#include "styles_commands_support.h"
#include "text/app_styles.h"
#include "text/personalize_copy.h"
#include "ui_hotkeys/hotkey_capture.h"
#include "ui_personalize/fit_row.h"
#include "ui_personalize/removable_rows.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;
namespace app_styles = fv::text::app_styles;

namespace {

const Json kCleanupOn{{"transcript_cleanup_enabled", true}};

Json with(Json values, const Json& more) {
    for (const auto& [key, value] : more.items()) {
        values[key] = value;
    }
    return values;
}

Json newInstallTones() {
    Json tones = Json::object();
    for (const auto& [category, tone] : app_styles::NEW_INSTALL_TONES) {
        tones[std::string(category)] = std::string(tone);
    }
    return tones;
}

std::vector<std::pair<std::string, std::string>> overridesOf(PersonalizeRig& rig) {
    return app_styles::resolve_overrides(*rig.settings().snapshot());
}

void click(QWidget* widget) {
    QTest::mouseClick(widget, Qt::LeftButton);
}

fv::Button* removeButton(fv::RemovableRows* rows, const QString& name) {
    for (fv::Button* button : rows->removeButtons()) {
        if (button->accessibleName() == QStringLiteral("Remove %1").arg(name)) {
            return button;
        }
    }
    return nullptr;
}

bool locked(fv::StylesPage& page) {
    bool all = true;
    bool any = false;
    for (const auto& card : page.cards()) {
        all = all && card.tile->isEnabled();
        any = any || card.tile->isEnabled();
    }
    const bool enabled = page.appStylesTile()->isEnabled();
    CHECK(all == enabled);
    CHECK(any == enabled);
    return !enabled;
}

QString focusedName() {
    QWidget* widget = QApplication::focusWidget();
    return widget != nullptr ? widget->accessibleName() : QString();
}

void focusOn(QWidget* widget) {
    widget->setFocus(Qt::TabFocusReason);
    flushDeletes();
}

void key(int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QTest::keyClick(QApplication::focusWidget(), static_cast<Qt::Key>(code), modifiers);
    flushDeletes();
}

} // namespace

// --- rail value ----------------------------------------------------------------------------

// from tests/test_personalize_s1_settings_page.py::test_rail_value
TEST_CASE("test_rail_value", "[test_personalize_s1_settings_page][unit]") {
    using fv::text::styles_page::rail_value;
    CHECK(rail_value(Json::object()) == "Needs AI cleanup");
    CHECK(rail_value(kCleanupOn) == "On · All formal");
    CHECK(rail_value(with(kCleanupOn, {{"app_style_tones", newInstallTones()}})) ==
          "On · 2 casual");
    CHECK(rail_value(with(kCleanupOn, {{"app_styles_enabled", false}})) == "Off");
    CHECK(rail_value(with(kCleanupOn, {{"app_context_enabled", false}})) == "Off");
}

// from tests/test_personalize_s1_settings_page.py::test_apps_summary_follows_the_users_choices
TEST_CASE("test_apps_summary_follows_the_users_choices",
          "[test_personalize_s1_settings_page][unit]") {
    using fv::text::styles_page::apps_summary;
    const std::vector<std::pair<std::string, std::string>> overrides{{"Notion", "work"},
                                                                     {"Gmail", "personal"}};
    CHECK(apps_summary("work", overrides) == "Notion, Slack, Teams, Google Chat and more.");
    CHECK(apps_summary("email", overrides) == "Outlook, Mail, Thunderbird and more.");
    CHECK(apps_summary("other", overrides) ==
          "Everything else, like Word, Google Docs and VS Code.");
    CHECK(apps_summary("other", {}) ==
          "Everything else, like Word, Notion, Google Docs and VS Code.");
}

// --- app awareness -------------------------------------------------------------------------

// from tests/test_personalize_s1_settings_page.py::test_defaults_show_awareness_on_and_reading_off
TEST_CASE("test_defaults_show_awareness_on_and_reading_off",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    fv::StylesPage& page = rig.styles();
    CHECK(rig.window().pageTitleLabel()->text() == QStringLiteral("Apps & styles"));
    CHECK(page.appContextCheck()->isChecked());
    CHECK(page.appContextTile()->property("checked") == QVariant(true));
    CHECK_FALSE(page.readTextCheck()->isChecked());
    CHECK(page.readTextTile()->isEnabled());
    // Exclusions also keep Command Mode out, so they stay editable.
    CHECK(page.excludedAppsTile()->isEnabled());
}

// from tests/test_personalize_s1_settings_page.py::test_exclusions_need_app_awareness
TEST_CASE("test_exclusions_need_app_awareness", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig({{"app_context_enabled", false}});
    CHECK_FALSE(rig.styles().excludedAppsTile()->isEnabled());
}

// from tests/test_personalize_s1_settings_page.py::test_switches_save_and_tell_the_app
TEST_CASE("test_switches_save_and_tell_the_app", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    fv::StylesPage& page = rig.styles();

    click(page.readTextTile());
    CHECK(rig.saved("app_context_read_text") == true);
    CHECK(page.excludedAppsTile()->isEnabled());
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("styles"));

    page.appContextCheck()->setChecked(false);
    CHECK(rig.saved("app_context_enabled") == false);
    CHECK_FALSE(page.readTextTile()->isEnabled());
    CHECK_FALSE(page.excludedAppsTile()->isEnabled());
    CHECK(page.privacy()->text().contains(QStringLiteral("first")));
}

// from tests/test_personalize_s1_settings_page.py::test_privacy_note_names_where_the_text_goes
TEST_CASE("test_privacy_note_names_where_the_text_goes",
          "[test_personalize_s1_settings_page][widget]") {
    {
        PersonalizeRig rig(with(kCleanupOn, {{"transcript_cleanup_provider", "openrouter"},
                                             {"transcript_cleanup_model", "openrouter/free"}}));
        const QString note = rig.styles().privacy()->text();
        CHECK(
            note.startsWith(QStringLiteral("Sent only to your AI cleanup provider (OpenRouter)")));
        CHECK(note.contains(QStringLiteral("Never saved")));
    }
    {
        PersonalizeRig rig(with(kCleanupOn, {{"transcript_cleanup_provider", "ollama"},
                                             {"transcript_cleanup_model", "llama3.2"}}));
        CHECK(rig.styles().privacy()->text().contains(QStringLiteral("on this computer")));
    }
    {
        PersonalizeRig rig;
        CHECK(rig.styles().privacy()->text().startsWith(QStringLiteral("AI cleanup is off")));
    }
}

// from
// tests/test_personalize_s1_settings_page.py::test_other_platforms_say_reading_text_is_windows_only
TEST_CASE("test_other_platforms_say_reading_text_is_windows_only",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    fv::StylesPage::setPlatform({{}, [] {
                                     return false;
                                 }});
    fv::StylesPage& page = rig.styles();
    CHECK_FALSE(page.readTextTile()->isEnabled());
    CHECK(page.privacy()->text().startsWith(QStringLiteral("Works on Windows for now")));
    CHECK(page.privacy()->text().contains(QStringLiteral("OpenWhisper_CPP only knows")));
}

// from tests/test_personalize_s1_settings_page.py::test_excluded_apps_add_and_remove
TEST_CASE("test_excluded_apps_add_and_remove", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig({{"app_context_read_text", true}});
    fv::StylesPage& page = rig.styles();
    CHECK(page.excludedRows()->rows().empty());

    for (const char* name : {"1Password", "  1password ", "Bitwarden"}) {
        page.excludedPicker()->setEditText(QString::fromUtf8(name));
        click(page.excludedAdd());
    }
    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"1Password", "Bitwarden"}));
    CHECK(page.excludedRows()->rows().size() == 2);
    CHECK(page.excludedPicker()->currentText().isEmpty());

    page.excludedPicker()->setEditText(QString());
    click(page.excludedAdd());
    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"1Password", "Bitwarden"}));

    auto* remove = page.excludedRows()->rows().front()->findChild<QAbstractButton*>();
    REQUIRE(remove != nullptr);
    CHECK(remove->accessibleName() == QStringLiteral("Remove 1Password"));
    click(remove);
    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"Bitwarden"}));
}

// from tests/test_personalize_s1_settings_page.py::test_pickers_offer_this_sessions_apps_first
TEST_CASE("test_pickers_offer_this_sessions_apps_first",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    fv::focus::AppIdentity chrome;
    chrome.app_id = "chrome.exe";
    chrome.name = "Chrome";
    chrome.title_hint = "Gmail";
    fv::focus::AppIdentity tool;
    tool.app_id = "foo_tool.exe";
    tool.name = "Foo Tool";
    fv::StylesPage::setPlatform(
        {[chrome, tool] { return std::vector<fv::focus::AppIdentity>{chrome, tool}; },
         [] {
             return true;
         }});
    fv::StylesPage& page = rig.styles();
    QStringList names;
    for (int index = 0; index < page.overridePicker()->count(); ++index) {
        names << page.overridePicker()->itemText(index);
    }
    REQUIRE(names.size() >= 2);
    CHECK(names.mid(0, 2) == QStringList{QStringLiteral("Gmail"), QStringLiteral("Foo Tool")});
    CHECK(names.contains(QStringLiteral("Slack")));
    QStringList folded;
    for (const QString& name : names) {
        if (!folded.contains(name.toCaseFolded())) {
            folded << name.toCaseFolded();
        }
    }
    CHECK(folded.size() == names.size());
}

// --- styles --------------------------------------------------------------------------------

// from tests/test_personalize_s1_settings_page.py::test_cleanup_off_shows_the_gate_and_locks_styles
TEST_CASE("test_cleanup_off_shows_the_gate_and_locks_styles",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    fv::StylesPage& page = rig.styles();

    CHECK_FALSE(page.stylesCleanupGateTile()->isHidden());
    CHECK(page.stylesCleanupGateTile()->titleLabel()->text() ==
          QStringLiteral("AI cleanup is off"));
    CHECK(page.stylesGateTile()->isHidden());
    CHECK_FALSE(page.appStylesTile()->isEnabled());
    for (const auto& card : page.cards()) {
        CHECK_FALSE(card.tile->isEnabled());
    }
    CHECK_FALSE(page.styleOverridesTile()->isEnabled());

    rig.gates().turnOnCleanup(&page);
    CHECK(rig.saved("transcript_cleanup_enabled") == true);
    CHECK(rig.window().currentDestination() == qs(dest::styles));
}

// from tests/test_personalize_s1_settings_page.py::test_awareness_off_gate_turns_it_back_on
TEST_CASE("test_awareness_off_gate_turns_it_back_on",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig(with(kCleanupOn, {{"app_context_enabled", false}}));
    fv::StylesPage& page = rig.styles();

    CHECK(page.stylesGateTile()->titleLabel()->text() ==
          QStringLiteral("Styles need app awareness"));
    CHECK(page.gateButton()->text() == QStringLiteral("Turn on"));
    click(page.gateButton());
    CHECK(rig.saved("app_context_enabled") == true);
    CHECK(page.stylesGateTile()->isHidden());
    CHECK(page.appStylesTile()->isEnabled());
}

// from tests/test_personalize_s1_settings_page.py::
//     test_picking_a_tone_saves_every_category_and_shows_an_example
TEST_CASE("test_picking_a_tone_saves_every_category_and_shows_an_example",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig(kCleanupOn);
    fv::StylesPage& page = rig.styles();
    const auto& work = page.card("work");
    CHECK(page.stylesGateTile()->isHidden());
    CHECK(work.tile->isEnabled());
    CHECK(work.example->text() == fv::StylesPage::example("work", "formal"));
    CHECK(work.tile->descriptionLabel()->text().startsWith(QStringLiteral("Slack, Teams")));

    click(work.bar->buttons()[2]);

    CHECK(rig.saved("app_style_tones") == Json{{"email", "formal"},
                                               {"work", "very_casual"},
                                               {"personal", "formal"},
                                               {"other", "formal"}});
    CHECK(work.example->text() == fv::StylesPage::example("work", "very_casual"));
    CHECK(rig.railValue(dest::styles) == QStringLiteral("On · 1 casual"));
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("styles"));
}

// from tests/test_personalize_s1_settings_page.py::test_loading_tones_never_saves
TEST_CASE("test_loading_tones_never_saves", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig(with(kCleanupOn, {{"app_style_tones", newInstallTones()}}));
    fv::StylesPage& page = rig.styles();
    CHECK(page.card("personal").bar->currentIndex() == 1);
    const auto writes = rig.settings().file().writes();
    rig.window().refresh();
    CHECK(rig.saved("app_style_tones") == newInstallTones());
    CHECK(rig.settings().file().writes() == writes);
    CHECK(rig.changes.isEmpty());
}

// from tests/test_personalize_s1_settings_page.py::test_styles_off_dims_the_cards
TEST_CASE("test_styles_off_dims_the_cards", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig(kCleanupOn);
    fv::StylesPage& page = rig.styles();

    click(page.appStylesTile());

    CHECK(rig.saved("app_styles_enabled") == false);
    for (const auto& card : page.cards()) {
        CHECK_FALSE(card.tile->isEnabled());
    }
    CHECK_FALSE(page.styleOverridesTile()->isEnabled());
    CHECK(rig.railValue(dest::styles) == QStringLiteral("Off"));
}

// from tests/test_personalize_s1_settings_page.py::test_overrides_add_replace_and_remove
TEST_CASE("test_overrides_add_replace_and_remove", "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig(kCleanupOn);
    fv::StylesPage& page = rig.styles();

    page.overridePicker()->setEditText(QStringLiteral("Notion"));
    page.overrideCategory()->setCurrentIndex(
        page.overrideCategory()->findData(QStringLiteral("work")));
    click(page.overrideAdd());

    CHECK(rig.saved("app_style_overrides") ==
          Json::array({Json{{"match", "Notion"}, {"category", "work"}}}));
    QStringList labels;
    for (QWidget* row : page.overrideRows()->rows()) {
        labels << row->findChild<QLabel*>(QStringLiteral("appStylesRowLabel"))->text();
    }
    CHECK(labels == QStringList{QStringLiteral("Notion  →  Work messages")});
    CHECK(page.card("work").tile->descriptionLabel()->text().startsWith(
        QStringLiteral("Notion, Slack")));
    CHECK_FALSE(
        page.card("other").tile->descriptionLabel()->text().contains(QStringLiteral("Notion")));

    page.overridePicker()->setEditText(QStringLiteral("notion"));
    page.overrideCategory()->setCurrentIndex(
        page.overrideCategory()->findData(QStringLiteral("personal")));
    click(page.overrideAdd());
    CHECK(rig.saved("app_style_overrides") ==
          Json::array({Json{{"match", "notion"}, {"category", "personal"}}}));

    click(page.overrideRows()->rows().front()->findChild<QAbstractButton*>());
    CHECK(rig.saved("app_style_overrides") == Json::array());
    CHECK(page.overrideRows()->rows().empty());
}

// from
// tests/test_personalize_s1_settings_page.py::test_search_finds_the_page_before_and_after_it_is_built
// The window's search index is its registered search fields: the same before the page is built
// and after, and each names a control the built page exposes with that title.
TEST_CASE("test_search_finds_the_page_before_and_after_it_is_built",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::styles);
    REQUIRE(spec != nullptr);
    QStringList titles;
    for (const auto& field : spec->searchFields) {
        titles << field.label;
    }
    for (const char* title : {"Read text near the cursor", "Never read from", "Work messages"}) {
        CHECK(titles.contains(QString::fromUtf8(title)));
    }
    rig.styles();
    for (const auto& field : spec->searchFields) {
        INFO(field.target);
        auto* tile = qobject_cast<fv::TileBase*>(rig.window().control(field.target));
        REQUIRE(tile != nullptr);
        CHECK(tile->titleLabel()->text() == field.label);
    }
}

// --- Home ----------------------------------------------------------------------------------

// from
// tests/test_personalize_s1_settings_page.py::test_home_row_matches_tone_and_explains_when_it_cannot
TEST_CASE("test_home_row_matches_tone_and_explains_when_it_cannot",
          "[test_personalize_s1_settings_page][widget]") {
    PersonalizeRig rig;
    rig.window();
    fv::HomePage& home = rig.home();
    auto* toggle = qobject_cast<fv::SettingsSwitch*>(home.control("app_styles_enabled"));
    REQUIRE(toggle != nullptr);
    CHECK(toggle->isChecked());
    CHECK_FALSE(toggle->isEnabled());
    CHECK(toggle->toolTip().contains(QStringLiteral("AI cleanup")));
    CHECK_FALSE(rig.window().isBuilt(dest::styles));

    rig.settings().save_setting("transcript_cleanup_enabled", true);
    rig.window().refreshRailValues();
    CHECK(toggle->isEnabled());
    CHECK(toggle->toolTip().isEmpty());

    toggle->click();
    CHECK(rig.saved("app_styles_enabled") == false);
    CHECK_FALSE(rig.styles().appStylesCheck()->isChecked());
}

// from tests/test_personalize_fix3_h1_basic_tone_copy.py::
//     test_an_upgraded_install_is_told_every_app_is_formal
TEST_CASE("test_an_upgraded_install_is_told_every_app_is_formal",
          "[test_personalize_fix3_h1_basic_tone_copy][widget]") {
    PersonalizeRig rig(kCleanupOn);
    rig.window();
    fv::HomePage& home = rig.home();
    auto* toggle = qobject_cast<fv::SettingsSwitch*>(home.control("app_styles_enabled"));
    REQUIRE(toggle != nullptr);
    CHECK(toggle->isChecked());
    QString text;
    for (const QString& detail : rig.homeDetails()) {
        if (detail.contains(QStringLiteral("Formal in every app"))) {
            text = detail;
        }
    }
    REQUIRE_FALSE(text.isEmpty());
    CHECK_FALSE(text.contains(QStringLiteral("relaxed")));
    CHECK_FALSE(text.toCaseFolded().contains(QStringLiteral("casual")));

    rig.settings().save_setting("app_style_tones", newInstallTones());
    home.refresh();
    CHECK(rig.homeDetails().contains(
        QStringLiteral("Formal for email and other apps, casual for chat.")));
}

// from tests/test_personalize_fix3_h1_basic_tone_copy.py::test_a_blocker_still_replaces_the_tones
TEST_CASE("test_a_blocker_still_replaces_the_tones",
          "[test_personalize_fix3_h1_basic_tone_copy][widget]") {
    PersonalizeRig rig;
    rig.window();
    rig.home();
    CHECK(rig.homeDetails().contains(QStringLiteral("Styles need AI cleanup, which is off.")));
}

// from tests/test_personalize_fix3_h1_basic_tone_copy.py::
//     test_the_summary_names_each_tone_where_it_applies
TEST_CASE("test_the_summary_names_each_tone_where_it_applies",
          "[test_personalize_fix3_h1_basic_tone_copy][unit]") {
    const auto copy = [](Json tones) {
        return fv::text::styles_page::home_copy(Json{{"app_style_tones", std::move(tones)}});
    };
    CHECK(copy(newInstallTones()) == "Formal for email and other apps, casual for chat.");
    CHECK(copy({{"email", "casual"},
                {"work", "casual"},
                {"personal", "casual"},
                {"other", "casual"}}) == "Casual in every app.");
    CHECK(copy(with(newInstallTones(), {{"personal", "very_casual"}})) ==
          "Formal for email and other apps, casual for work messages, very casual for personal "
          "messages.");
    CHECK(copy(with(newInstallTones(), {{"work", "formal"}})) ==
          "Formal for email, work messages and other apps, casual for personal messages.");
}

// --- layout ----------------------------------------------------------------------------------

// from tests/test_personalize_s1_settings_page.py::test_page_fits_narrow_windows_at_large_fonts
// The classic variant (940 px); the omarchy ones are Linux's Omarchy desktop look, not ported.
TEST_CASE("test_page_fits_narrow_windows_at_large_fonts",
          "[test_personalize_s1_settings_page][widget]") {
    AppTheme theme(QStringLiteral("dark"), 130);
    PersonalizeRig rig(
        with(kCleanupOn,
             {{"app_context_read_text", true},
              {"app_context_excluded_apps", Json::array({"A password manager with a long name"})},
              {"app_style_overrides",
               Json::array({Json{{"match", "Notion"}, {"category", "personal"}}})}}));
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    dialog.resize(940, 600);
    dialog.selectDestination(qs(dest::styles));
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
    CHECK(dialog.width() == 940);
    fv::SettingsPage* page = dialog.page(dest::styles);
    QList<QWidget*> controls;
    for (auto* control : page->findChildren<QAbstractButton*>()) {
        controls << control;
    }
    for (auto* control : page->findChildren<QComboBox*>()) {
        controls << control;
    }
    for (auto* control : page->findChildren<QLineEdit*>()) {
        controls << control;
    }
    for (QWidget* control : controls) {
        if (control->isVisible()) {
            INFO(control->objectName().toStdString()
                 << " " << control->accessibleName().toStdString());
            CHECK(control->mapTo(page, control->rect().topLeft()).x() >= 0);
            CHECK(control->mapTo(page, control->rect().bottomRight()).x() < page->width());
        }
    }
    for (auto* label : page->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    for (auto* tile : page->findChildren<fv::TileBase*>()) {
        if (tile->isVisible()) {
            INFO(tile->titleLabel()->text().toStdString());
            CHECK(page->rect().contains(tile->mapTo(page, QPoint(0, 0))));
            CHECK(tile->mapTo(page, QPoint(tile->width() - 1, 0)).x() < page->width());
        }
    }
    QScrollArea* scroll = dialog.pageScroll(dest::styles);
    REQUIRE(scroll != nullptr);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    CHECK(page->mapTo(scroll->viewport(), page->rect().bottomRight()).y() <=
          scroll->viewport()->height());
    dialog.hide();
}

// --- the shared cleanup gate ----------------------------------------------------------------

// from tests/test_personalize_fix_f4_styles_gate.py::
//     test_styles_shows_the_shared_cleanup_gate_and_turns_cleanup_on_in_place
TEST_CASE("test_styles_shows_the_shared_cleanup_gate_and_turns_cleanup_on_in_place",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    PersonalizeRig rig;
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    processEvents(3);
    fv::InfoTile* gate = page.stylesCleanupGateTile();
    fv::Button* button = rig.gates().turnOnButton(gate);
    REQUIRE(button != nullptr);

    CHECK_FALSE(gate->isHidden());
    CHECK(page.stylesGateTile()->isHidden());
    CHECK(gate->titleLabel()->text() == QStringLiteral("AI cleanup is off"));
    CHECK(button->text() == QStringLiteral("Turn on (Medium)"));

    click(button);

    CHECK(rig.saved("transcript_cleanup_enabled") == true);
    CHECK(rig.window().currentDestination() == qs(dest::styles));
    CHECK(gate->isHidden());
    CHECK_FALSE(locked(page));
    rig.window().hide();
}

// from
// tests/test_personalize_fix_f4_styles_gate.py::test_awareness_gate_shows_only_once_cleanup_is_on
TEST_CASE("test_awareness_gate_shows_only_once_cleanup_is_on",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    PersonalizeRig rig({{"app_context_enabled", false}});
    rig.window().show();
    fv::StylesPage& page = rig.styles();
    processEvents(3);
    CHECK_FALSE(page.stylesCleanupGateTile()->isHidden());
    CHECK(page.stylesGateTile()->isHidden());

    rig.gates().turnOnCleanup(&page);

    CHECK(page.stylesCleanupGateTile()->isHidden());
    CHECK_FALSE(page.stylesGateTile()->isHidden());
    CHECK(page.stylesGateTile()->titleLabel()->text() ==
          QStringLiteral("Styles need app awareness"));
    CHECK(locked(page));
    click(page.gateButton());
    CHECK(rig.saved("app_context_enabled") == true);
    CHECK(page.stylesGateTile()->isHidden());
    CHECK_FALSE(locked(page));
    rig.window().hide();
}

// from tests/test_personalize_fix_f4_styles_gate.py::test_the_style_switch_has_one_name
TEST_CASE("test_the_style_switch_has_one_name", "[test_personalize_fix_f4_styles_gate][widget]") {
    PersonalizeRig rig;
    rig.window();
    auto* toggle = rig.home().control("app_styles_enabled");
    REQUIRE(toggle != nullptr);
    const QString homeTitle = toggle->accessibleName();
    const fv::SettingsPageSpec* spec = rig.context().settingsPages().page(dest::styles);
    REQUIRE(spec != nullptr);
    QStringList titles;
    for (const auto& field : spec->searchFields) {
        if (field.target == "app_styles_tile") {
            titles << field.label;
        }
    }
    CHECK(titles == QStringList{homeTitle});
    CHECK(rig.styles().appStylesTile()->titleLabel()->text() == homeTitle);
}

// from tests/test_personalize_fix_f4_styles_gate.py::
//     test_command_mode_field_reads_like_every_other_shortcut_field
TEST_CASE("test_command_mode_field_reads_like_every_other_shortcut_field",
          "[test_personalize_fix_f4_styles_gate][widget]") {
    PersonalizeRig rig;
    fv::DialogPersonalizeHost host(rig.window());
    fv::CommandShortcutField field(host);
    fv::HotkeyCaptureInput plain;
    CHECK(field.input()->placeholderText() == plain.placeholderText());
}

// --- keyboard focus in the app lists --------------------------------------------------------

namespace {

const Json kBothLists{
    {"transcript_cleanup_enabled", true},
    {"app_context_excluded_apps", Json::array({"Signal", "1Password"})},
    {"app_style_overrides", Json::array({Json{{"match", "Signal"}, {"category", "work"}},
                                         Json{{"match", "Foo Tool"}, {"category", "email"}}})}};

// test_personalize_fix2_g2_styles_focus.py's _page: the shown, active window on Apps & styles.
fv::StylesPage& focusPage(PersonalizeRig& rig) {
    rig.shownWindow();
    fv::StylesPage& page = rig.styles();
    flushDeletes();
    return page;
}

} // namespace

// from tests/test_personalize_fix2_g2_styles_focus.py::
//     test_removing_an_excluded_app_focuses_the_next_then_the_picker
TEST_CASE("test_removing_an_excluded_app_focuses_the_next_then_the_picker",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig({{"app_context_excluded_apps", Json::array({"Signal", "1Password"})}});
    fv::StylesPage& page = focusPage(rig);
    focusOn(removeButton(page.excludedRows(), QStringLiteral("Signal")));

    key(Qt::Key_Space);

    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"1Password"}));
    CHECK(focusedName() == QStringLiteral("Remove 1Password"));

    key(Qt::Key_Space);

    CHECK(rig.saved("app_context_excluded_apps") == Json::array());
    CHECK(QApplication::focusWidget() == page.excludedPicker());
    CHECK(rig.window().currentDestination() == qs(dest::styles));
    rig.window().hide();
}

// from tests/test_personalize_fix2_g2_styles_focus.py::
//     test_removing_the_last_excluded_app_focuses_the_one_above
TEST_CASE("test_removing_the_last_excluded_app_focuses_the_one_above",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig(
        {{"app_context_excluded_apps", Json::array({"Signal", "1Password", "Zoom"})}});
    fv::StylesPage& page = focusPage(rig);
    focusOn(removeButton(page.excludedRows(), QStringLiteral("Zoom")));

    key(Qt::Key_Space);

    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"Signal", "1Password"}));
    CHECK(focusedName() == QStringLiteral("Remove 1Password"));
    rig.window().hide();
}

// from tests/test_personalize_fix2_g2_styles_focus.py::
//     test_removing_a_style_override_focuses_the_next_then_the_picker
TEST_CASE("test_removing_a_style_override_focuses_the_next_then_the_picker",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig(kBothLists);
    fv::StylesPage& page = focusPage(rig);
    focusOn(removeButton(page.overrideRows(), QStringLiteral("Signal")));

    key(Qt::Key_Space);

    CHECK(overridesOf(rig) ==
          std::vector<std::pair<std::string, std::string>>{{"Foo Tool", "email"}});
    CHECK(focusedName() == QStringLiteral("Remove Foo Tool"));

    key(Qt::Key_Space);

    CHECK(overridesOf(rig).empty());
    CHECK(QApplication::focusWidget() == page.overridePicker());
    rig.window().hide();
}

// from tests/test_personalize_fix2_g2_styles_focus.py::test_a_rebuild_keeps_focus_on_the_same_app
TEST_CASE("test_a_rebuild_keeps_focus_on_the_same_app",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig({{"app_context_excluded_apps", Json::array({"Signal", "1Password"})}});
    fv::StylesPage& page = focusPage(rig);
    focusOn(removeButton(page.excludedRows(), QStringLiteral("1Password")));

    page.excludedRows()->setRows({{QStringLiteral("Zoom"), QStringLiteral("Zoom")},
                                  {QStringLiteral("Signal"), QStringLiteral("Signal")},
                                  {QStringLiteral("1Password"), QStringLiteral("1Password")}});
    flushDeletes();

    CHECK(focusedName() == QStringLiteral("Remove 1Password"));
    rig.window().hide();
}

// from tests/test_personalize_fix2_g2_styles_focus.py::test_adding_an_app_leaves_focus_on_add
TEST_CASE("test_adding_an_app_leaves_focus_on_add",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig({{"app_context_excluded_apps", Json::array({"Signal"})}});
    fv::StylesPage& page = focusPage(rig);
    focusOn(page.excludedAdd());
    page.excludedPicker()->setEditText(QStringLiteral("Zoom"));

    key(Qt::Key_Space);

    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"Signal", "Zoom"}));
    CHECK(QApplication::focusWidget() == page.excludedAdd());
    rig.window().hide();
}

// from tests/test_personalize_fix2_g2_styles_focus.py::
//     test_tab_walks_each_list_in_order_before_its_picker (both lists)
TEST_CASE("test_tab_walks_each_list_in_order_before_its_picker",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    for (const bool excludedList : {true, false}) {
        INFO((excludedList ? "excluded_rows" : "override_rows"));
        PersonalizeRig rig(kBothLists);
        fv::StylesPage& page = focusPage(rig);
        fv::RemovableRows* rows = excludedList ? page.excludedRows() : page.overrideRows();
        QWidget* picker = excludedList ? page.excludedPicker() : page.overridePicker();
        QWidget* beside = excludedList ? static_cast<QWidget*>(page.excludedAdd())
                                       : static_cast<QWidget*>(page.overrideCategory());
        const std::vector<fv::Button*> removes = rows->removeButtons();
        REQUIRE(removes.size() == 2);
        focusOn(removes[0]);

        key(Qt::Key_Tab);
        CHECK(QApplication::focusWidget() == removes[1]);
        key(Qt::Key_Tab);
        CHECK(QApplication::focusWidget() == picker);
        key(Qt::Key_Tab);
        CHECK(QApplication::focusWidget() == beside);

        focusOn(removes[0]);
        key(Qt::Key_Backtab, Qt::ShiftModifier);
        QWidget* before = QApplication::focusWidget();
        CHECK(before != rig.window().pageMenuButton());
        CHECK_FALSE(rows->isAncestorOf(before));
        key(Qt::Key_Tab);
        CHECK(QApplication::focusWidget() == removes[0]);
        rig.window().hide();
    }
}

// from tests/test_personalize_fix2_g2_styles_focus.py::test_tab_order_holds_after_adding_an_app
TEST_CASE("test_tab_order_holds_after_adding_an_app",
          "[test_personalize_fix2_g2_styles_focus][widget]") {
    PersonalizeRig rig({{"app_context_excluded_apps", Json::array({"Signal"})}});
    fv::StylesPage& page = focusPage(rig);
    page.excludedPicker()->setEditText(QStringLiteral("Zoom"));
    page.excludedAdd()->click();
    flushDeletes();

    focusOn(removeButton(page.excludedRows(), QStringLiteral("Signal")));
    key(Qt::Key_Tab);
    CHECK(focusedName() == QStringLiteral("Remove Zoom"));
    key(Qt::Key_Tab);
    CHECK(QApplication::focusWidget() == page.excludedPicker());
    rig.window().hide();
}

// --- Enter in the pickers -------------------------------------------------------------------

namespace {

void press(QWidget* widget, int code = Qt::Key_Return,
           Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    widget->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    QTest::keyClick(widget, static_cast<Qt::Key>(code), modifiers);
    QApplication::processEvents();
}

void typeAndPress(QWidget* widget, const QString& text) {
    widget->setFocus(Qt::TabFocusReason);
    QApplication::processEvents();
    QTest::keyClicks(widget, text);
    press(widget);
}

// _pick_and_add: typing opens the picker's filtered list; the first Enter takes the match from
// it and the second is the field's own.
void pickAndAdd(fv::SearchableComboBox* picker, const QString& text) {
    typeAndPress(picker, text);
    CHECK_FALSE(picker->view()->isVisible());
    press(picker);
}

// _stayed: still on the page, and Enter never reached a header button (no page menu opened).
void stayed(fv::SettingsDialog& dialog, const QString& title) {
    CHECK(dialog.pageTitleLabel()->text() == title);
    CHECK(dialog.pageMenu() == nullptr);
}

const Json kEnterCleanupOn{{"selected_model", "parakeet"},
                           {"transcript_cleanup_enabled", true},
                           {"transcript_cleanup_provider", "openrouter"},
                           {"transcript_cleanup_model", "openrouter/free"}};

} // namespace

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_adds_an_app_to_never_read_from
// The classic variant; the omarchy one is Linux's Omarchy desktop look.
TEST_CASE("test_enter_adds_an_app_to_never_read_from",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    PersonalizeRig rig({{"selected_model", "parakeet"}});
    fv::SettingsDialog& dialog = rig.shownWindow();
    fv::StylesPage& page = rig.styles();
    processEvents(3);

    pickAndAdd(page.excludedPicker(), QStringLiteral("Signal"));

    CHECK(rig.saved("app_context_excluded_apps") == Json::array({"Signal"}));
    stayed(dialog, QStringLiteral("Apps & styles"));
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_moves_an_app_to_a_style
// The classic variant; the omarchy one is Linux's Omarchy desktop look.
TEST_CASE("test_enter_moves_an_app_to_a_style", "[test_personalize_fix2_g2_enter_key][widget]") {
    PersonalizeRig rig(kEnterCleanupOn);
    fv::SettingsDialog& dialog = rig.shownWindow();
    fv::StylesPage& page = rig.styles();
    processEvents(3);

    pickAndAdd(page.overridePicker(), QStringLiteral("Notion"));

    CHECK(overridesOf(rig) ==
          std::vector<std::pair<std::string, std::string>>{{"Notion", "email"}});
    stayed(dialog, QStringLiteral("Apps & styles"));
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_enter_key.py::
//     test_enter_in_a_field_without_an_action_changes_nothing
// The Dictionary search, the snippet trigger and the transform name; the Recording case is the
// Recording page's (a stand-in here). The classic variant; the omarchy one is Linux's.
TEST_CASE("test_enter_in_a_field_without_an_action_changes_nothing",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    const Json word{{"id", "a"},        {"term", "Ksenia"},
                    {"starred", false}, {"heard", Json::array()},
                    {"learned", false}, {"new", false}};
    const Json values{{"selected_model", "parakeet"},
                      {"dictation_dictionary", Json::array({word})}};
    for (const std::string_view destination : {dest::dictionary, dest::snippets, dest::commands}) {
        INFO(std::string(destination));
        PersonalizeRig rig(values);
        fv::SettingsDialog& dialog = rig.shownWindow();
        dialog.selectDestination(qs(destination));
        processEvents(3);
        QWidget* widget = nullptr;
        QString title;
        if (destination == dest::dictionary) {
            title = QStringLiteral("Dictionary");
            widget = dialog.findChild<QLineEdit*>(QStringLiteral("dictionarySearchInput"));

        } else if (destination == dest::snippets) {
            title = QStringLiteral("Snippets");
            fv::SnippetsPanel& panel = rig.snippetEditor();
            panel.newButton()->click();
            widget = panel.triggerEdit();
        } else {
            title = QStringLiteral("Commands");
            fv::TransformsPanel& panel = rig.transforms();
            panel.newButton()->click();
            widget = panel.nameEdit();
        }
        REQUIRE(widget != nullptr);
        widget->show();

        typeAndPress(widget, QStringLiteral("4"));

        stayed(dialog, title);
        CHECK(QApplication::focusWidget() == widget);
        dialog.hide();
    }
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_enter_on_a_focused_button_still_presses_it
// The classic variant; the omarchy one is Linux's Omarchy desktop look.
TEST_CASE("test_enter_on_a_focused_button_still_presses_it",
          "[test_personalize_fix2_g2_enter_key][widget]") {
    PersonalizeRig rig({{"selected_model", "parakeet"}});
    fv::SettingsDialog& dialog = rig.shownWindow();
    fv::DictionaryPage& page = rig.dictionary();
    processEvents(3);
    page.termEdit()->setText(QStringLiteral("Kubernetes"));

    press(page.addButton());

    CHECK(rig.terms() == std::vector<std::string>{"Kubernetes"});
    stayed(dialog, QStringLiteral("Dictionary"));
    dialog.hide();
}

// from tests/test_personalize_fix2_g2_enter_key.py::test_escape_still_closes_settings
// The classic variant; the omarchy one is Linux's Omarchy desktop look.
TEST_CASE("test_escape_still_closes_settings", "[test_personalize_fix2_g2_enter_key][widget]") {
    PersonalizeRig rig({{"selected_model", "parakeet"}});
    fv::SettingsDialog& dialog = rig.shownWindow();
    fv::DictionaryPage& page = rig.dictionary();
    processEvents(3);

    press(page.termEdit(), Qt::Key_Escape);

    CHECK_FALSE(dialog.isVisible());
}
