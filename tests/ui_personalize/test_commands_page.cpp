// Settings > Personalize > Commands: the Command Mode shortcut, its switches, the transforms
// library and the Home row (tests/test_personalize_s3_commands_page.py,
// test_personalize_fix_f4_command_field.py and test_personalize_fix3_h1_command_setup.py),
// inside a real Settings window over an in-memory settings file. provider_readiness is the rig's
// `ready`; QMessageBox.question is the panel's `question` seam.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include <QAbstractButton>
#include <QCoreApplication>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QTextEdit>

#include "pyport/number.h"
#include "settings/keys.h"
#include "styles_commands_support.h"
#include "text/personalize_copy.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

using fv::Json;
using namespace fvtest;

namespace {

const Json kProfile{{"id", "ticket"},
                    {"name", "Support ticket"},
                    {"instructions", "Format a ticket."},
                    {"hotkey", "ctrl+alt+t"}};

Json withProfiles(Json profiles) {
    return Json{{"transcript_cleanup_profiles", std::move(profiles)}};
}

QStringList listTexts(const QListWidget* list) {
    QStringList texts;
    for (int row = 0; row < list->count(); ++row) {
        texts << list->item(row)->text();
    }
    return texts;
}

QAbstractButton* buttonNamed(QWidget* parent, const QString& text) {
    for (auto* button : parent->findChildren<QAbstractButton*>()) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

std::string commandHotkey(PersonalizeRig& rig) {
    const Json hotkeys = rig.savedHotkeys();
    const auto found = hotkeys.find("command_mode");
    return found != hotkeys.end() && found->is_string() ? found->get<std::string>() : "";
}

} // namespace

// from
// tests/test_personalize_s3_commands_page.py::test_rail_value_counts_transforms_and_the_shortcut
TEST_CASE("test_rail_value_counts_transforms_and_the_shortcut",
          "[test_personalize_s3_commands_page][unit]") {
    using fv::text::commands_page::rail_value;
    CHECK(rail_value(Json::object()) == "No shortcut · 4 transforms");
    const Json settings{{"hotkeys", Json{{"command_mode", "ctrl+alt+k"}}},
                        {"text_transforms",
                         Json::array({Json{{"id", "a"}, {"name", "A"}, {"instruction", "Do."}}})}};
    CHECK(rail_value(settings) == "Shortcut set · 1 transform");
    CHECK(rail_value(Json{{"hotkeys", "broken"}}) == "No shortcut · 4 transforms");
}

// from
// tests/test_personalize_s3_commands_page.py::test_the_ai_cleanup_notice_shows_only_without_a_provider
TEST_CASE("test_the_ai_cleanup_notice_shows_only_without_a_provider",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    rig.ready = false;
    fv::SettingsDialog& dialog = rig.window();
    fv::CommandsPage& page = rig.commands();
    dialog.show();
    PersonalizeRig::answers();
    fv::SettingsPage* container = dialog.page(dest::commands);
    CHECK(page.gateTile()->isVisibleTo(container));
    QAbstractButton* link = buttonNamed(page.gateTile(), QStringLiteral("Open AI cleanup"));
    REQUIRE(link != nullptr);
    link->click();
    CHECK(dialog.currentDestination() == qs(dest::cleanup));

    // A provider set up on AI cleanup clears the notice on the way back.
    rig.ready = true;
    dialog.selectDestination(qs(dest::commands));
    PersonalizeRig::answers();
    CHECK_FALSE(page.gateTile()->isVisibleTo(container));
    dialog.hide();
}

// from tests/test_personalize_s3_commands_page.py::test_the_shortcut_saves_through_the_hotkey_path
TEST_CASE("test_the_shortcut_saves_through_the_hotkey_path",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::CommandShortcutField& field = *rig.commands().shortcut();

    Q_EMIT field.input()->captured(QStringLiteral("ctrl+alt+k"));

    CHECK(commandHotkey(rig) == "ctrl+alt+k");
    CHECK(field.input()->text() == display("ctrl+alt+k"));
    CHECK(field.message()->isHidden());
    CHECK(rig.railValue(dest::commands) == QStringLiteral("Shortcut set · 4 transforms"));

    field.clearButton()->click();
    CHECK(commandHotkey(rig).empty());
    CHECK_FALSE(field.clearButton()->isEnabled());
}

// from tests/test_personalize_s3_commands_page.py::test_a_taken_shortcut_is_refused_inline
TEST_CASE("test_a_taken_shortcut_is_refused_inline",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig(withProfiles(Json::array({kProfile})));
    fv::CommandShortcutField& field = *rig.commands().shortcut();

    field.input()->setHotkey(QStringLiteral("ctrl+alt+t"));
    Q_EMIT field.input()->captured(QStringLiteral("ctrl+alt+t"));

    CHECK(field.message()->text() ==
          QStringLiteral("That shortcut is already used by Support ticket."));
    CHECK_FALSE(field.message()->isHidden());
    CHECK(commandHotkey(rig).empty());
    CHECK(field.input()->hotkey().isEmpty());
}

// from
// tests/test_personalize_s3_commands_page.py::test_capturing_pauses_global_shortcuts_and_leaving_stops_it
TEST_CASE("test_capturing_pauses_global_shortcuts_and_leaving_stops_it",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::CommandShortcutField& field = *rig.commands().shortcut();
    field.beginCapture();
    CHECK(field.input()->isCapturing());

    rig.window().selectDestination(qs(dest::general));

    CHECK_FALSE(field.input()->isCapturing());
    CHECK(rig.captures == std::vector<bool>{true, false});
}

// from
// tests/test_personalize_s3_commands_page.py::test_a_shortcut_set_elsewhere_shows_when_the_page_returns
TEST_CASE("test_a_shortcut_set_elsewhere_shows_when_the_page_returns",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = rig.window();
    fv::CommandsPage& page = rig.commands();
    dialog.show();
    dialog.selectDestination(qs(dest::general));
    CHECK(dialog.setStandardHotkey(QStringLiteral("command_mode"), QStringLiteral("ctrl+alt+j"))
              .isEmpty());
    dialog.selectDestination(qs(dest::commands));
    QCoreApplication::processEvents();
    CHECK(page.shortcut()->input()->text() == display("ctrl+alt+j"));
    dialog.hide();
}

// from tests/test_personalize_s3_commands_page.py::test_the_hint_follows_the_recording_mode
TEST_CASE("test_the_hint_follows_the_recording_mode",
          "[test_personalize_s3_commands_page][widget]") {
    const std::vector<std::pair<const char*, const char*>> cases{
        {"push_hold", "Hold the shortcut while you speak, then let go."},
        {"toggle", "Press the shortcut, speak, then press it again."},
    };
    for (const auto& [mode, hint] : cases) {
        INFO(mode);
        PersonalizeRig rig({{"recording_trigger_mode", mode}});
        CHECK(rig.commands().modeHint()->text() == QString::fromUtf8(hint));
    }
}

// from
// tests/test_personalize_s3_commands_page.py::test_writing_without_a_selection_is_a_saved_switch
TEST_CASE("test_writing_without_a_selection_is_a_saved_switch",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig({{"command_mode_insert_without_selection", false}});
    fv::SettingTile* tile = rig.commands().insertTile();
    CHECK_FALSE(tile->checkbox()->isChecked());
    CHECK(tile->property("checked") == QVariant(false));

    tile->checkbox()->toggle();

    CHECK(rig.saved("command_mode_insert_without_selection") == true);
    CHECK(tile->property("checked") == QVariant(true));
}

// from tests/test_personalize_s3_commands_page.py::test_the_page_says_rewrites_are_kept_in_history
TEST_CASE("test_the_page_says_rewrites_are_kept_in_history",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    rig.commands();
    bool found = false;
    for (const auto* label : rig.window().page(dest::commands)->findChildren<QLabel*>()) {
        found = found || label->text().contains(QStringLiteral("saved in History"));
    }
    CHECK(found);
}

// from
// tests/test_personalize_s3_commands_page.py::test_profile_shortcuts_are_listed_and_link_to_profiles
TEST_CASE("test_profile_shortcuts_are_listed_and_link_to_profiles",
          "[test_personalize_s3_commands_page][widget]") {
    {
        PersonalizeRig rig(withProfiles(Json::array(
            {kProfile, Json{{"id", "mail"}, {"name", "Email"}, {"instructions", "Email."}}})));
        fv::CommandsPage& page = rig.commands();
        CHECK(page.profilesList()->text() ==
              QStringLiteral("Support ticket · ") + display("ctrl+alt+t"));
        QAbstractButton* link = buttonNamed(page.profilesTile(), QStringLiteral("Open Profiles"));
        REQUIRE(link != nullptr);
        link->click();
        CHECK(rig.window().currentDestination() == qs(dest::cleanup_profiles));
    }
    {
        PersonalizeRig rig;
        CHECK(rig.commands().profilesList()->text() ==
              QStringLiteral("No profile has a shortcut yet."));
    }
}

// --- transforms library ------------------------------------------------------------------------

// from tests/test_personalize_s3_commands_page.py::test_a_new_transform_is_saved_and_registered
TEST_CASE("test_a_new_transform_is_saved_and_registered",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::TransformsPanel& panel = rig.transforms();
    QStringList starters;
    for (const auto& transform : fv::text::starter_transforms()) {
        starters << qs(transform.name);
    }
    CHECK(listTexts(panel.transformList()) == starters);

    panel.newTransform();
    panel.nameEdit()->setText(QStringLiteral("Friendlier"));
    panel.instructionEdit()->setPlainText(QStringLiteral("Make it warmer."));
    Q_EMIT panel.hotkeyInput()->captured(QStringLiteral("ctrl+alt+f"));
    panel.hotkeyInput()->setHotkey(QStringLiteral("ctrl+alt+f"));
    CHECK(panel.saveTransform());

    const auto saved = rig.savedTransforms();
    REQUIRE_FALSE(saved.empty());
    CHECK(saved.back().name == "Friendlier");
    CHECK(saved.back().hotkey == "ctrl+alt+f");
    CHECK(panel.message()->text() ==
          QStringLiteral("Saved Friendlier. Select text anywhere and press %1.")
              .arg(display("ctrl+alt+f")));
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("transforms"));
    CHECK(rig.railValue(dest::commands) == QStringLiteral("No shortcut · 5 transforms"));
}

// from
// tests/test_personalize_s3_commands_page.py::test_a_taken_transform_shortcut_is_refused_when_captured
TEST_CASE("test_a_taken_transform_shortcut_is_refused_when_captured",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig(withProfiles(Json::array({kProfile})));
    fv::TransformsPanel& panel = rig.transforms();
    panel.hotkeyInput()->beginCapture();
    panel.hotkeyInput()->setHotkey(QStringLiteral("ctrl+alt+t"));

    Q_EMIT panel.hotkeyInput()->captured(QStringLiteral("ctrl+alt+t"));

    CHECK(panel.hotkeyInput()->hotkey().isEmpty());
    CHECK(panel.message()->text() ==
          QStringLiteral("That shortcut is already used by Support ticket. Choose another."));
    REQUIRE_FALSE(rig.captures.empty());
    CHECK(rig.captures.front() == true);
    panel.cancelCapture();
}

// from tests/test_personalize_s3_commands_page.py::test_invalid_transforms_explain_themselves
TEST_CASE("test_invalid_transforms_explain_themselves",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::TransformsPanel& panel = rig.transforms();
    panel.newTransform();
    panel.nameEdit()->setText(QStringLiteral("polish"));
    panel.instructionEdit()->setPlainText(QStringLiteral("Again."));

    CHECK_FALSE(panel.saveTransform());

    CHECK(panel.message()->text() == QStringLiteral("A transform with that name already exists."));
    CHECK(rig.saved("text_transforms").is_null());
    CHECK(rig.changes.isEmpty());
}

// from tests/test_personalize_s3_commands_page.py::test_starters_fill_the_instruction
TEST_CASE("test_starters_fill_the_instruction", "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::TransformsPanel& panel = rig.transforms();
    panel.newTransform();
    const auto& starters = fv::text::starter_transforms();
    const fv::text::Transform& starter = starters[1];

    panel.useStarter(starter);
    CHECK(panel.nameEdit()->text() == qs(starter.name));
    CHECK(panel.instructionEdit()->toPlainText() == qs(starter.instruction));

    rig.answer = QMessageBox::Cancel;
    panel.useStarter(starters[0]);
    CHECK(rig.asked ==
          QStringList{QStringLiteral("Use this starter?: Replace the current instruction?")});
    CHECK(panel.instructionEdit()->toPlainText() == qs(starter.instruction));
}

// from tests/test_personalize_s3_commands_page.py::
//     test_an_unsaved_draft_survives_a_refresh_and_asks_before_switching
TEST_CASE("test_an_unsaved_draft_survives_a_refresh_and_asks_before_switching",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::CommandsPage& page = rig.commands();
    fv::TransformsPanel& panel = *page.transformsPanel();
    panel.instructionEdit()->setPlainText(QStringLiteral("Polish, but gently."));

    page.refreshPage();
    CHECK(panel.instructionEdit()->toPlainText() == QStringLiteral("Polish, but gently."));

    rig.answer = QMessageBox::Cancel;
    panel.transformList()->setCurrentRow(2);
    CHECK(rig.asked ==
          QStringList{QStringLiteral("Unsaved transform: Save your changes to this transform?")});
    CHECK(panel.transformList()->currentRow() == 0);
    CHECK(panel.instructionEdit()->toPlainText() == QStringLiteral("Polish, but gently."));

    rig.answer = QMessageBox::Save;
    panel.transformList()->setCurrentRow(2);
    CHECK(panel.nameEdit()->text() == QStringLiteral("Fix grammar"));
    CHECK(rig.savedTransforms().front().instruction == "Polish, but gently.");
}

// from tests/test_personalize_s3_commands_page.py::test_duplicates_stay_drafts_until_saved
TEST_CASE("test_duplicates_stay_drafts_until_saved",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::TransformsPanel& panel = rig.transforms();

    panel.duplicateTransform();

    CHECK(panel.nameEdit()->text() == QStringLiteral("Polish copy"));
    CHECK(panel.hotkeyInput()->hotkey().isEmpty());
    CHECK(panel.hasUnsavedChanges());
    CHECK(rig.saved("text_transforms").is_null());
    CHECK(panel.saveTransform());
    CHECK(rig.savedTransforms().back().name == "Polish copy");
}

// from tests/test_personalize_s3_commands_page.py::test_delete_asks_first
TEST_CASE("test_delete_asks_first", "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::TransformsPanel& panel = rig.transforms();

    rig.answer = QMessageBox::Cancel;
    panel.deleteTransform();
    CHECK(rig.saved("text_transforms").is_null());

    rig.answer = QMessageBox::Yes;
    panel.deleteTransform();
    std::vector<std::string> ids;
    for (const auto& transform : rig.savedTransforms()) {
        ids.push_back(transform.id);
    }
    CHECK(ids == std::vector<std::string>{"make-concise", "fix-grammar", "prompt-engineer"});
    CHECK(panel.nameEdit()->text() == QStringLiteral("Make concise"));
    REQUIRE_FALSE(rig.changes.isEmpty());
    CHECK(rig.changes.back() == QStringLiteral("transforms"));
}

// from
// tests/test_personalize_s3_commands_page.py::test_cancel_capture_stops_every_capture_on_the_page
TEST_CASE("test_cancel_capture_stops_every_capture_on_the_page",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::CommandsPage& page = rig.commands();
    page.shortcut()->beginCapture();
    page.transformsPanel()->hotkeyInput()->beginCapture();

    page.cancelCapture();

    CHECK_FALSE(page.shortcut()->input()->isCapturing());
    CHECK_FALSE(page.transformsPanel()->hotkeyInput()->isCapturing());
}

// --- Home and layout -------------------------------------------------------------------------

// from tests/test_personalize_s3_commands_page.py::test_basic_has_a_command_mode_shortcut_row
TEST_CASE("test_basic_has_a_command_mode_shortcut_row",
          "[test_personalize_s3_commands_page][widget]") {
    PersonalizeRig rig;
    fv::SettingsDialog& dialog = rig.window();
    fv::HomePage& home = rig.home();
    auto* field = home.findChild<fv::CommandShortcutField*>();
    REQUIRE(field != nullptr);
    CHECK_FALSE(dialog.isBuilt(dest::commands));
    bool titled = false;
    for (const auto* label : home.findChildren<QLabel*>()) {
        titled = titled || label->text() == QStringLiteral("Command Mode shortcut");
    }
    CHECK(titled);

    Q_EMIT field->input()->captured(QStringLiteral("ctrl+alt+k"));
    CHECK(commandHotkey(rig) == "ctrl+alt+k");

    const Json defaults = fv::default_hotkeys();
    const QString record = qs(defaults.at("record_toggle").get<std::string>());
    Q_EMIT field->input()->captured(record);
    CHECK(dialog.messageText().contains(QStringLiteral("already used by Recording")));
    CHECK(field->message()->isHidden());
    CHECK(commandHotkey(rig) == "ctrl+alt+k");
}

// from
// tests/test_personalize_fix3_h1_command_setup.py::test_basic_says_command_mode_needs_an_ai_model
TEST_CASE("test_basic_says_command_mode_needs_an_ai_model",
          "[test_personalize_fix3_h1_command_setup][widget]") {
    PersonalizeRig rig;
    rig.ready = false;
    rig.window();
    fv::HomePage& home = rig.home();
    home.refresh();
    PersonalizeRig::answers();
    CHECK(rig.homeDetails().contains(
        QStringLiteral("Needs an AI cleanup model, even while cleanup is off.")));

    rig.ready = true;
    home.refresh();
    PersonalizeRig::answers();
    QString detail;
    for (const QString& text : rig.homeDetails()) {
        if (text.startsWith(QStringLiteral("Select text, then say how to change it."))) {
            detail = text;
        }
    }
    CHECK_FALSE(detail.isEmpty());
    CHECK_FALSE(detail.contains(QStringLiteral("AI cleanup")));
    CHECK_FALSE(rig.homeDetails().contains(
        QStringLiteral("Needs an AI cleanup model, even while cleanup is off.")));
}

// from
// tests/test_personalize_s3_commands_page.py::test_the_page_fits_a_narrow_window_at_large_fonts The
// classic variant; the omarchy one is Linux's Omarchy desktop look, not ported.
TEST_CASE("test_the_page_fits_a_narrow_window_at_large_fonts",
          "[test_personalize_s3_commands_page][widget]") {
    AppTheme theme(QStringLiteral("dark"), 130);
    PersonalizeRig rig(withProfiles(Json::array({kProfile})));
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    // Narrower than the classic window floor, as a tiled window can be.
    dialog.setMinimumSize(0, 0);
    dialog.resize(800, 600);
    fv::CommandsPage& page = rig.commands();
    for (int i = 0; i < 8; ++i) {
        QCoreApplication::processEvents();
    }
    fv::SettingsPage* container = dialog.page(dest::commands);
    QList<QWidget*> controls;
    for (auto* control : container->findChildren<QAbstractButton*>()) {
        controls << control;
    }
    for (auto* control : container->findChildren<QLineEdit*>()) {
        controls << control;
    }
    for (QWidget* control : controls) {
        if (!control->isVisible()) {
            continue;
        }
        INFO(control->objectName().toStdString() << " " << control->accessibleName().toStdString());
        CHECK(control->mapTo(container, control->rect().topLeft()).x() >= 0);
        CHECK(control->mapTo(container, control->rect().bottomRight()).x() < container->width());
        // Every button keeps at least its label's height.
        if (auto* button = qobject_cast<QAbstractButton*>(control);
            button != nullptr && !button->text().isEmpty()) {
            CHECK(button->height() >= button->fontMetrics().height());
        }
    }
    for (auto* label : container->findChildren<QLabel*>()) {
        if (label->isVisible() && label->wordWrap()) {
            INFO(label->text().toStdString());
            CHECK(label->height() >= label->heightForWidth(label->width()));
        }
    }
    fv::TransformsPanel& panel = *page.transformsPanel();
    CHECK(panel.duplicateButton()->geometry().right() < panel.deleteButton()->geometry().left());
    QScrollArea* scroll = dialog.pageScroll(dest::commands);
    REQUIRE(scroll != nullptr);
    scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
    QCoreApplication::processEvents();
    CHECK(container->mapTo(scroll->viewport(), container->rect().bottomRight()).y() <=
          scroll->viewport()->height());
    dialog.hide();
}

// --- the shortcut field in narrow windows -------------------------------------------------------

namespace {

void settle(fv::SettingsDialog& dialog, int width) {
    dialog.setMinimumSize(0, 0);
    dialog.resize(width, 700);
    for (int i = 0; i < 10; ++i) {
        QCoreApplication::processEvents();
    }
}

void assertApart(fv::CommandShortcutField& field) {
    std::vector<QWidget*> buttons;
    if (field.changeButton() != nullptr) {
        buttons.push_back(field.changeButton());
    }
    if (field.clearButton() != nullptr) {
        buttons.push_back(field.clearButton());
    }
    std::vector<QWidget*> widgets{field.input()};
    widgets.insert(widgets.end(), buttons.begin(), buttons.end());
    for (QWidget* widget : widgets) {
        INFO(widget->metaObject()->className());
        CHECK(widget->isVisible());
        CHECK(widget->geometry().left() >= 0);
        CHECK(widget->geometry().right() < field.width());
    }
    for (QWidget* button : buttons) {
        CHECK_FALSE(field.input()->geometry().intersects(button->geometry()));
    }
}

Json shortcutSet() {
    return Json{{"hotkeys", Json{{"command_mode", "ctrl+alt+shift+k"}}}};
}

} // namespace

// from
// tests/test_personalize_fix_f4_command_field.py::test_commands_page_field_never_overlaps_clear The
// classic variant (940 px) at 100 % and 130 %; the omarchy widths are Linux's Omarchy look.
TEST_CASE("test_commands_page_field_never_overlaps_clear",
          "[test_personalize_fix_f4_command_field][widget]") {
    for (const int scale : {100, 130}) {
        INFO(scale);
        AppTheme theme(QStringLiteral("dark"), scale);
        PersonalizeRig rig(shortcutSet());
        fv::SettingsDialog& dialog = rig.window();
        dialog.show();
        fv::CommandsPage& page = rig.commands();
        settle(dialog, 940);
        assertApart(*page.shortcut());
        dialog.hide();
    }
}

// from tests/test_personalize_fix_f4_command_field.py::test_home_row_field_never_overlaps_change
// At the test's widths and 130 %, in the one look OpenWhisper_CPP has.
TEST_CASE("test_home_row_field_never_overlaps_change",
          "[test_personalize_fix_f4_command_field][widget]") {
    for (const int width : {560, 460}) {
        INFO(width);
        AppTheme theme(QStringLiteral("dark"), 130);
        PersonalizeRig rig(shortcutSet());
        fv::SettingsDialog& dialog = rig.window();
        dialog.show();
        dialog.showHome();
        settle(dialog, width);
        auto* field = rig.home().findChild<fv::CommandShortcutField*>();
        REQUIRE(field != nullptr);
        assertApart(*field);
        dialog.hide();
    }
}

// from
// tests/test_personalize_fix_f4_command_field.py::test_a_wide_field_keeps_its_buttons_beside_the_box
TEST_CASE("test_a_wide_field_keeps_its_buttons_beside_the_box",
          "[test_personalize_fix_f4_command_field][widget]") {
    AppTheme theme(QStringLiteral("dark"), 100);
    PersonalizeRig rig(shortcutSet());
    fv::SettingsDialog& dialog = rig.window();
    dialog.show();
    fv::CommandsPage& page = rig.commands();
    settle(dialog, 1100);
    fv::CommandShortcutField& field = *page.shortcut();
    CHECK(field.clearButton()->geometry().top() < field.input()->geometry().bottom());
    CHECK(field.clearButton()->geometry().left() > field.input()->geometry().right());
    dialog.hide();
}

// from
// tests/test_personalize_fix_f4_command_field.py::test_stacks_when_a_button_floor_exceeds_its_hint
// Measured with the button's floor: below it, Qt would draw the box over Clear.
TEST_CASE("test_stacks_when_a_button_floor_exceeds_its_hint",
          "[test_personalize_fix_f4_command_field][widget]") {
    AppTheme theme(QStringLiteral("dark"), 100);
    PersonalizeRig rig(shortcutSet());
    fv::DialogPersonalizeHost host(rig.window());
    QWidget holder;
    auto* field = new fv::CommandShortcutField(host, false, true, true, &holder);
    fv::Button* clear = field->clearButton();
    clear->setMinimumWidth(clear->sizeHint().width() + 40);
    const int box = static_cast<int>(fv::py::round_half_even(
        fv::CommandShortcutField::kInputMinWidth * fv::ui::currentUiFontScale()));
    // Wide enough by the size hint, too narrow by the floor.
    const int width = box + field->row()->spacing() + clear->sizeHint().width() + 20;
    holder.resize(width, 200);
    field->resize(width, 200);
    holder.show();
    for (int i = 0; i < 5; ++i) {
        QCoreApplication::processEvents();
    }
    CHECK(field->row()->direction() == QBoxLayout::TopToBottom);
    assertApart(*field);
    holder.hide();
}
