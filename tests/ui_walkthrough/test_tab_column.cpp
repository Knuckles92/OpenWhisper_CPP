// The Walkthrough tab's control column (ui_qt/widgets/walkthrough_tab.py): Start and Cancel, the
// shortcut rows, the badge, Auto Mode, the write-up rows and their menus, the copy choice, the
// prompt in its popover, the sizes. Ported from tests/test_walkthrough_buttons.py (the tab's 31
// tests) and the agent-label half of tests/test_walkthrough_write_up_notice.py.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <algorithm>
#include <cmath>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <QApplication>
#include <QComboBox>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalSpy>
#include <QTest>
#include <QTimer>

#include "settings/keys.h"
#include "tab_support.h"
#include "ui_snap/capture_delay.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/feature_badge.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/studio.h"
#include "walkthrough/styles.h"
#include "walkthrough/write_up_names.h"

namespace fvtest {
namespace {

using fv::Json;
namespace tab = fv::walkthrough_tab;

QString qs(std::string_view text) {
    return QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size()));
}

// The styles a new install picks from: the default, then the starters.
std::vector<fv::walkthrough::WriteUpStyle> styles() {
    return fv::walkthrough::load_styles(Json::object());
}

void shown(TabRig& rig, int width = 1100, int height = 760) {
    rig->resize(width, height);
    rig->show();
    QApplication::processEvents();
}

// The application's type size for one test case, put back to 100 % after it.
class ScopedFontScale {
public:
    explicit ScopedFontScale(int percent) { fv::ui::applyUiFontScale(percent, *qApp); }
    ~ScopedFontScale() { fv::ui::applyUiFontScale(100, *qApp); }
    ScopedFontScale(const ScopedFontScale&) = delete;
    ScopedFontScale& operator=(const ScopedFontScale&) = delete;
};

// The status after the tab in each agent's menu item.
QStringList statuses(TabRig& rig) {
    QStringList out;
    for (const QString& text : menuTexts(rig->agentField())) {
        out << text.section(QLatin1Char('\t'), 1);
    }
    return out;
}

std::string savedPrompt(const TabRig& rig) {
    return rig.saved().value("walkthrough_write_up_instructions", std::string("<unset>"));
}

} // namespace

// --- the Walkthrough tab -------------------------------------------------

// from tests/test_walkthrough_buttons.py::test_the_button_asks_for_a_walkthrough
TEST_CASE("the button asks for a walkthrough", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    QSignalSpy spy(rig.tab.get(), &fv::WalkthroughTab::walkthroughRequested);

    rig->walkthroughButton()->click();

    CHECK(spy.count() == 1);
    CHECK(rig->walkthroughButton()->text() == QStringLiteral("Start Walkthrough"));
    CHECK_FALSE(rig->cancelButton()->isVisibleTo(rig.tab.get()));
}

// from tests/test_walkthrough_buttons.py::test_auto_mode_is_opt_in_and_set_before_recording
TEST_CASE("auto mode is opt in and set before recording", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    CHECK_FALSE(rig->autoSwitch()->isChecked());
    rig->autoSwitch()->click();
    CHECK(rig.saved().at("walkthrough_auto_mode") == Json(true));

    rig->setRecordingState(true, true);
    CHECK_FALSE(rig->autoSwitch()->isEnabled());
    CHECK_FALSE(rig->captureDelay()->isEnabled());
    rig->setRecordingState(false, false);
    CHECK(rig->autoSwitch()->isEnabled());
    CHECK(rig->captureDelay()->isEnabled());
}

// from tests/test_walkthrough_buttons.py::test_auto_mode_is_offered_on_supported_desktops
// (Python's sys.platform is WalkthroughTabOptions::platform; the desktop parameter,
// OPENWHISPER_UI, changes nothing the tab reads)
TEST_CASE("auto mode is offered on supported desktops", "[test_walkthrough_buttons][widget]") {
    const auto [platform, visible] = GENERATE(table<std::string, bool>({
        {"darwin", true},
        {"win32", true},
        {"linux", false},
    }));
    CAPTURE(platform);
    TabRig::Options options;
    options.platform = platform;
    TabRig rig(options);
    CHECK(rig->autoRow()->isVisibleTo(rig.tab.get()) == visible);
    CHECK(rig->delayRow()->isVisibleTo(rig.tab.get()) == visible);
    CHECK(tab::offersAutoMode(platform) == visible);
}

// from tests/test_walkthrough_buttons.py::test_during_a_walkthrough_the_tab_finishes_or_cancels_it
TEST_CASE("during a walkthrough the tab finishes or cancels it",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    QSignalSpy cancel(rig.tab.get(), &fv::WalkthroughTab::cancelRequested);
    rig->setRecordingState(true, true);

    CHECK(rig->walkthroughButton()->text() == QStringLiteral("Finish Walkthrough"));
    CHECK(rig->walkthroughButton()->isActive());
    // Cancel takes the shortcuts' place under Finish; the stage shows them now.
    CHECK(rig->cancelButton()->isVisibleTo(rig.tab.get()));
    CHECK_FALSE(rig->keysBox()->isVisibleTo(rig.tab.get()));
    rig->cancelButton()->click();
    CHECK(cancel.count() == 1);

    rig->setRecordingState(false, false);
    CHECK(rig->walkthroughButton()->text() == QStringLiteral("Start Walkthrough"));
    CHECK(rig->walkthroughButton()->isActive());
    CHECK_FALSE(rig->cancelButton()->isVisibleTo(rig.tab.get()));
    CHECK(rig->keysBox()->isVisibleTo(rig.tab.get()));
}

// from tests/test_walkthrough_buttons.py::test_a_dictation_in_progress_holds_the_button
TEST_CASE("a dictation in progress holds the button", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    QSignalSpy spy(rig.tab.get(), &fv::WalkthroughTab::walkthroughRequested);
    rig->setRecordingState(true, false);

    QTest::mouseClick(rig->walkthroughButton(), Qt::LeftButton);

    CHECK_FALSE(rig->walkthroughButton()->isActive());
    CHECK(spy.count() == 0);
}

// from tests/test_walkthrough_buttons.py::test_the_tab_shows_both_shortcuts
// (the button's set_hotkey is read back from the hover pill it shows)
TEST_CASE("the tab shows both shortcuts", "[test_walkthrough_buttons][widget]") {
    TabRig rig;

    rig->setHotkeys(QStringLiteral("Ctrl+Alt+W"), QString());

    fv::HotkeyHintFilter* hint = rig->walkthroughButton()->hotkeyHintFilter();
    hint->showHint();
    REQUIRE(hint->hint() != nullptr);
    auto* pill = hint->hint()->findChild<QLabel*>();
    REQUIRE(pill != nullptr);
    CHECK(pill->text() == QStringLiteral("Ctrl+Alt+W"));
    hint->hideHint();
    CHECK(rig->startKey()->text() == QStringLiteral("Ctrl+Alt+W"));
    CHECK(rig->snapKey()->text() == QStringLiteral("Not set"));
    CHECK(rig->snapKey()->property("unset") == QVariant(true));
    // Recording, the stage says them too.
    CHECK(rig->recordingKeys()->text().contains(QStringLiteral("Ctrl+Alt+W")));
    CHECK(rig->recordingKeys()->text().contains(QStringLiteral("Not set")));
}

// from tests/test_walkthrough_buttons.py::test_the_two_shortcuts_line_up_as_a_column
TEST_CASE("the two shortcuts line up as a column", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig->setHotkeys(QStringLiteral("Ctrl+Alt+Shift+W"), QStringLiteral("F9"));
    shown(rig);

    QLabel* start = rig->startKey();
    QLabel* snap = rig->snapKey();
    CHECK(start->width() == snap->width());
    CHECK(start->mapTo(rig.tab.get(), start->rect().topRight()).x() ==
          snap->mapTo(rig.tab.get(), snap->rect().topRight()).x());
}

// from tests/test_walkthrough_buttons.py::test_the_button_leaves_its_explanation_to_the_badge
TEST_CASE("the button leaves its explanation to the badge", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    // Its own hover shows the shortcut pill, as the Quick Record buttons do.
    CHECK(rig->walkthroughButton()->toolTip().isEmpty());
    CHECK(rig->walkthroughBadge()->toolTip().contains(QStringLiteral("screenshots")));
}

// from tests/test_walkthrough_buttons.py::test_the_badge_leads_the_shortcut_row_not_the_button
TEST_CASE("the badge leads the shortcut row not the button", "[test_walkthrough_buttons][widget]") {
    // Off the button, so Start Walkthrough matches the other tabs' Start buttons.
    TabRig rig;
    shown(rig);

    fv::FeatureBadge* badge = rig->walkthroughBadge();
    CHECK_FALSE(rig->walkthroughButton()->isAncestorOf(badge));
    CHECK(badge->geometry().right() < rig->startKey()->geometry().left());
    CHECK(std::abs(badge->geometry().center().y() - rig->startKey()->geometry().center().y()) <= 1);
    CHECK_FALSE(badge->isCompact());
}

// from tests/test_walkthrough_buttons.py::test_start_walkthrough_spans_the_control_column
TEST_CASE("start walkthrough spans the control column", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    shown(rig);

    fv::SuccessButton* button = rig->walkthroughButton();
    QFrame* column = rig->column();
    const QMargins margins = column->layout()->contentsMargins();
    // Within the button's own stylesheet margin.
    CHECK(std::abs(button->width() - (column->width() - margins.left() - margins.right())) <= 2);
    // Every value in the form ends at the same right edge.
    std::set<int> edges;
    for (QWidget* field : std::vector<QWidget*>{rig->agentField(), rig->styleField(),
                                                rig->copyField(), rig->captureDelay()}) {
        if (field->isVisibleTo(rig.tab.get())) {
            edges.insert(field->mapTo(column, field->rect().topRight()).x());
        }
    }
    CHECK(edges.size() == 1);
}

namespace {

// _button_with_badge: a plain button with a badge on it, shown at `width` x 48.
struct ButtonWithBadge {
    QPushButton button{QStringLiteral("Start Walkthrough")};
    fv::FeatureBadge* badge;

    explicit ButtonWithBadge(int width)
        : badge(new fv::FeatureBadge(&button, QStringLiteral("What it does."))) {
        button.show();
        button.resize(width, 48);
        QApplication::processEvents();
    }
    ~ButtonWithBadge() { button.close(); }
};

} // namespace

// from tests/test_walkthrough_buttons.py::
//     test_on_a_button_the_badge_sits_on_its_right_side_clear_of_its_label
TEST_CASE("on a button the badge sits on its right side clear of its label",
          "[test_walkthrough_buttons][widget]") {
    ButtonWithBadge made(360);
    const QRect box = made.badge->geometry();
    CHECK(360 - 20 < box.right());
    CHECK(box.right() < 360);
    CHECK(std::abs(box.center().y() - 24) <= 1);
    const int label =
        made.button.fontMetrics().horizontalAdvance(QStringLiteral("Start Walkthrough"));
    CHECK(box.left() > (360 + label) / 2.0);
    CHECK_FALSE(made.badge->isCompact());
}

// from tests/test_walkthrough_buttons.py::test_in_a_narrow_button_new_folds_to_its_sparkle
TEST_CASE("in a narrow button new folds to its sparkle", "[test_walkthrough_buttons][widget]") {
    ButtonWithBadge made(220);
    CHECK(made.badge->isCompact());
    CHECK(made.badge->width() == made.badge->height());
    const int label =
        made.button.fontMetrics().horizontalAdvance(QStringLiteral("Start Walkthrough"));
    CHECK(made.badge->geometry().left() > (220 + label) / 2.0);
}

// from tests/test_walkthrough_buttons.py::test_clicking_the_badge_never_starts_a_walkthrough
TEST_CASE("clicking the badge never starts a walkthrough", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig->show();
    QSignalSpy spy(rig.tab.get(), &fv::WalkthroughTab::walkthroughRequested);

    QTest::mouseClick(rig->walkthroughBadge(), Qt::LeftButton);

    CHECK(spy.count() == 0);
}

// from tests/test_walkthrough_buttons.py::test_the_badge_says_new_until_the_first_walkthrough
TEST_CASE("the badge says new until the first walkthrough", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    CHECK(rig->walkthroughBadge()->text() == QStringLiteral("New"));

    rig->setRecordingState(true, true);

    CHECK(rig->walkthroughBadge()->text() == QStringLiteral("?"));
    CHECK(rig.saved().at("walkthrough_intro_seen") == Json(true));
    fv::WalkthroughTabOptions options;
    options.capturesRoot = rig.scratch.path() / "captures";
    options.io = &rig.io;
    fv::WalkthroughTab again(*rig.settings, {}, options);
    CHECK(again.walkthroughBadge()->text() == QStringLiteral("?"));
    again.close();
}

// from tests/test_walkthrough_buttons.py::test_picking_an_agent_turns_the_write_up_on_with_it
TEST_CASE("picking an agent turns the write up on with it", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    CHECK_FALSE(rig->writeUpSwitch()->isChecked());
    CHECK(std::ranges::none_of(rig->writeUpRows(), [](QWidget* row) { return row->isEnabled(); }));

    pick(rig->agentField(), QStringLiteral("Codex"));

    CHECK(rig.saved().at("walkthrough_write_up") == Json(true));
    CHECK(rig.saved().at("walkthrough_write_up_agent") == Json("codex"));
    CHECK(rig->agentField()->value() == QStringLiteral("Codex"));
    CHECK(std::ranges::all_of(rig->writeUpRows(), [](QWidget* row) { return row->isEnabled(); }));
    CHECK(rig->writeUpRowName()->toolTip().contains(QStringLiteral("Codex")));
}

// from tests/test_walkthrough_buttons.py::test_the_agent_menu_marks_the_chosen_agent
TEST_CASE("the agent menu marks the chosen agent", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    pick(rig->agentField(), QStringLiteral("Codex"));

    QStringList checked;
    for (const QString& text : menuTexts(rig->agentField(), true)) {
        checked << text.section(QLatin1Char('\t'), 0, 0);
    }

    CHECK(checked == QStringList{QStringLiteral("Codex")});
}

// from
// tests/test_walkthrough_buttons.py::test_switching_the_write_up_off_keeps_the_agent_for_next_time
TEST_CASE("switching the write up off keeps the agent for next time",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    pick(rig->agentField(), QStringLiteral("Codex"));

    rig->writeUpSwitch()->click();

    CHECK(rig.saved().at("walkthrough_write_up") == Json(false));
    CHECK(rig.saved().at("walkthrough_write_up_agent") == Json("codex"));
    // Dimmed, not hidden: the column keeps its shape.
    CHECK(std::ranges::none_of(rig->writeUpRows(), [](QWidget* row) { return row->isEnabled(); }));
    CHECK(std::ranges::all_of(rig->writeUpRows(),
                              [&rig](QWidget* row) { return row->isVisibleTo(rig.tab.get()); }));
    CHECK(rig->agentField()->value() == QStringLiteral("Codex"));
}

// from tests/test_walkthrough_buttons.py::test_the_tab_opens_on_the_saved_choices
TEST_CASE("the tab opens on the saved choices", "[test_walkthrough_buttons][widget]") {
    const auto starters = styles();
    TabRig::Options options;
    options.settings = Json{
        {"walkthrough_write_up", true},
        {"walkthrough_write_up_agent", "opencode_cli"},
        {"walkthrough_write_up_notes", true},
        {"walkthrough_copy_format", "both"},
        {"walkthrough_write_up_instructions", starters[2].instructions},
        {"walkthrough_minimize_to_tray", true},
    };
    TabRig rig(options);

    CHECK(rig->minimizeSwitch()->isChecked());
    CHECK(rig->writeUpSwitch()->isChecked());
    CHECK(rig->agentField()->value() == QStringLiteral("OpenCode"));
    CHECK(rig->notesSwitch()->isChecked());
    CHECK(rig->copyField()->value() == QStringLiteral("PDF + Markdown"));
    CHECK(rig->promptEdit()->toPlainText() == qs(starters[2].instructions));
    CHECK(rig->presetCombo()->currentText() == qs(starters[2].name));
    CHECK(rig->styleField()->value() == qs(starters[2].name));
}

// from tests/test_walkthrough_buttons.py::test_the_copy_choice_is_saved
TEST_CASE("the copy choice is saved", "[test_walkthrough_buttons][widget]") {
    const auto choice = GENERATE(from_range(tab::COPY_CHOICES));
    const QString label = qs(choice.text);
    CAPTURE(choice.value);
    TabRig rig;

    pick(rig->copyField(), label);

    CHECK(rig.saved().at("walkthrough_copy_format") == Json(std::string(choice.value)));
    CHECK(rig->copyField()->value() == label);
    rig->loadSettings();
    CHECK(rig->copyField()->value() == label);
    CHECK(menuTexts(rig->copyField(), true) == QStringList{label});
}

// from tests/test_walkthrough_buttons.py::
//     test_minimizing_to_tray_on_start_is_off_until_switched_on_and_then_saved
TEST_CASE("minimizing to tray on start is off until switched on and then saved",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    CHECK_FALSE(rig->minimizeSwitch()->isChecked());

    rig->minimizeSwitch()->click();

    CHECK(rig.saved().at("walkthrough_minimize_to_tray") == Json(true));
}

// from tests/test_walkthrough_buttons.py::test_a_style_picked_from_the_column_fills_the_prompt
TEST_CASE("a style picked from the column fills the prompt", "[test_walkthrough_buttons][widget]") {
    const auto starters = styles();
    TabRig rig;
    pick(rig->styleField(), qs(starters[2].name));

    CHECK(rig->promptEdit()->toPlainText() == qs(starters[2].instructions));
    CHECK(savedPrompt(rig) == starters[2].instructions);
    CHECK(rig->styleField()->value() == qs(starters[2].name));

    rig->promptEdit()->setPlainText(QStringLiteral("Make it a checklist."));
    CHECK(rig->styleField()->value() == qs(tab::OWN_PROMPT));
    const QStringList names = menuTexts(rig->styleField());
    CHECK(names.contains(qs(tab::OWN_PROMPT)));
    CHECK(names.contains(qs(tab::NEW_STYLE)));
}

// from tests/test_walkthrough_buttons.py::test_a_missing_agent_shows_on_its_field
TEST_CASE("a missing agent shows on its field", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    ScopedScanSeam restore;
    fv::agent_picker::scanSeam().cached_agents = [] {
        return std::optional<fv::agents::ScanResult>(fv::agents::ScanResult{});
    };

    pick(rig->agentField(), QStringLiteral("Claude Code"));

    CHECK(rig->agentField()->property("tone") == QVariant(QStringLiteral("warning")));
    CHECK(rig->agentField()->toolTip().contains(QStringLiteral("Not installed")));
    const QStringList all = statuses(rig);
    CHECK(all.size() == 3);
    CHECK(all == QStringList(all.size(), QStringLiteral("Not installed")));
}

// from tests/test_walkthrough_buttons.py::
//     test_editing_the_prompt_opens_it_beside_the_column_and_turns_the_write_up_on
TEST_CASE("editing the prompt opens it beside the column and turns the write up on",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    shown(rig);
    CHECK_FALSE(rig->writeUpSwitch()->isChecked());

    rig->editPrompt();
    QApplication::processEvents();

    CHECK(rig->writeUpSwitch()->isChecked());
    CHECK(rig->promptPopover()->isVisible());
    CHECK(rig->promptArea()->isVisible());
    // To the column's right, unless the screen's edge holds it back.
    fv::PromptPopover* popover = rig->promptPopover();
    const int beside = rig->column()->mapToGlobal(QPoint(rig->column()->width() + 8, 0)).x();
    const QRect screen = rig->screen()->availableGeometry();
    CHECK(popover->x() ==
          std::max(screen.left() + 8, std::min(beside, screen.right() - popover->width() - 8)));
    CHECK(screen.contains(popover->geometry()));
    popover->hide();
}

// from tests/test_walkthrough_buttons.py::test_closing_the_prompt_saves_what_was_typed
TEST_CASE("closing the prompt saves what was typed", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig->editPrompt();
    rig->promptEdit()->setPlainText(QStringLiteral("Typed, then closed."));
    CHECK(rig->promptTimer()->isActive());

    rig->promptPopover()->hide();

    CHECK_FALSE(rig->promptTimer()->isActive());
    CHECK(savedPrompt(rig) == "Typed, then closed.");
}

// from tests/test_walkthrough_buttons.py::test_too_narrow_for_both_the_column_goes_above_the_stage
TEST_CASE("too narrow for both the column goes above the stage",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig->show();
    rig->resize(1100, 760);
    QApplication::processEvents();
    fv::StudioSplit* split = rig->split();
    CHECK_FALSE(split->isStacked());
    CHECK(rig->stage()->geometry().left() > rig->column()->geometry().right());

    rig->resize(640, 760);
    QApplication::processEvents();
    CHECK(split->isStacked());
    CHECK(rig->stage()->geometry().top() > rig->column()->geometry().bottom());
    // Stacked, the column takes the page's width, and nothing runs past it.
    CHECK(rig->column()->width() == split->width());
    CHECK(rig->scrollArea()->horizontalScrollBar()->maximum() == 0);

    rig->resize(1100, 760);
    QApplication::processEvents();
    CHECK_FALSE(split->isStacked());
}

// from tests/test_walkthrough_buttons.py::test_a_larger_type_size_widens_the_column
TEST_CASE("a larger type size widens the column", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    const int narrow = rig->preferredWidth();
    ScopedFontScale larger(130);

    rig->applySizes();

    CHECK(rig->split()->columnWidth() == static_cast<int>(std::nearbyint(320 * 1.3)));
    CHECK(rig->agentField()->width() == static_cast<int>(std::nearbyint(tab::FIELD_WIDTH * 1.3)));
    CHECK(rig->preferredWidth() > narrow);
}

// from tests/test_walkthrough_buttons.py::
//     test_the_prompt_shows_the_default_words_and_saves_them_as_default
TEST_CASE("the prompt shows the default words and saves them as default",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    CHECK(rig->promptEdit()->toPlainText() == qs(fv::walkthrough::write_up::DEFAULT_INSTRUCTIONS));
    CHECK(rig->presetCombo()->currentText() == QStringLiteral("Walkthrough"));

    rig->savePrompt();

    CHECK(savedPrompt(rig).empty());
}

// from tests/test_walkthrough_buttons.py::test_a_style_fills_the_prompt_and_is_saved_at_once
TEST_CASE("a style fills the prompt and is saved at once", "[test_walkthrough_buttons][widget]") {
    const auto starters = styles();
    TabRig rig;

    rig->presetCombo()->setCurrentIndex(3);
    Q_EMIT rig->presetCombo()->activated(3);

    CHECK(rig->promptEdit()->toPlainText() == qs(starters[3].instructions));
    CHECK(savedPrompt(rig) == starters[3].instructions);
    CHECK_FALSE(rig->promptTimer()->isActive());
    CHECK(rig->presetCombo()->currentText() == qs(starters[3].name));
}

// from tests/test_walkthrough_buttons.py::
//     test_typing_your_own_prompt_saves_it_and_a_style_can_be_tried_and_undone
// (the pause is waited out: QTimer::timeout cannot be emitted from outside)
TEST_CASE("typing your own prompt saves it and a style can be tried and undone",
          "[test_walkthrough_buttons][widget]") {
    const auto starters = styles();
    TabRig rig;
    rig->promptEdit()->setPlainText(QStringLiteral("Make it a checklist."));

    CHECK(rig->presetCombo()->currentText() == qs(tab::OWN_PROMPT));
    CHECK(rig->promptTimer()->isActive()); // saved once typing pauses
    CHECK(QTest::qWaitFor([&rig] { return !rig->promptTimer()->isActive(); }, 3000));
    CHECK(savedPrompt(rig) == "Make it a checklist.");

    Q_EMIT rig->presetCombo()->activated(1);
    CHECK(savedPrompt(rig) == starters[1].instructions);
    const int own = rig->presetCombo()->findText(qs(tab::OWN_PROMPT));
    Q_EMIT rig->presetCombo()->activated(own);
    CHECK(rig->promptEdit()->toPlainText() == QStringLiteral("Make it a checklist."));
    CHECK(savedPrompt(rig) == "Make it a checklist.");
}

// from
// tests/test_walkthrough_buttons.py::test_a_prompt_changed_in_settings_shows_when_the_window_is_back
TEST_CASE("a prompt changed in settings shows when the window is back",
          "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig.settings->save_setting("walkthrough_write_up_instructions", "From Settings.");

    rig->loadSettings();

    CHECK(rig->promptEdit()->toPlainText() == QStringLiteral("From Settings."));
    CHECK(rig->presetCombo()->currentText() == qs(tab::OWN_PROMPT));
}

// from tests/test_walkthrough_buttons.py::test_typing_not_yet_saved_wins_over_reloading
TEST_CASE("typing not yet saved wins over reloading", "[test_walkthrough_buttons][widget]") {
    TabRig rig;
    rig.settings->save_setting("walkthrough_write_up_instructions", "Older.");
    rig->promptEdit()->setPlainText(QStringLiteral("Newer, still being typed."));

    rig->loadSettings();

    CHECK(rig->promptEdit()->toPlainText() == QStringLiteral("Newer, still being typed."));
    CHECK(savedPrompt(rig) == "Newer, still being typed.");
}

// load_settings shows the saved choices with every control's signals blocked: nothing is
// written back.
TEST_CASE("loading the settings writes none of them back", "[walkthrough_tab][widget]") {
    TabRig::Options options;
    options.settings = Json{{"walkthrough_write_up", true},
                            {"walkthrough_auto_mode", true},
                            {"walkthrough_write_up_instructions", "Mine."}};
    TabRig rig(options);
    const Json before = rig.saved();

    rig->loadSettings();
    rig->loadSettings();

    CHECK(rig.saved() == before);
    CHECK_FALSE(rig->promptTimer()->isActive());
}

// --- the agent labels (test_walkthrough_write_up_notice.py) --------------
// Python's two tests read Settings' agent combo; the tab labels its own agent menu and field from
// the same scan. The Settings page's combo is tests/ui_settings/test_walkthroughs_page.cpp.

// from tests/test_walkthrough_write_up_notice.py::test_agents_the_scan_did_not_find_are_marked
TEST_CASE("agents the scan did not find are marked", "[test_walkthrough_write_up_notice][widget]") {
    ScopedScanSeam restore;
    fv::agents::InstalledAgent codex;
    codex.id = "codex";
    codex.path = "C:/tools/codex.exe";
    codex.version = "0.30.0";
    codex.signed_in = true;
    fv::agents::ScanResult found{
        {"claude_code", std::nullopt}, {"codex", codex}, {"opencode_cli", std::nullopt}};
    fv::agent_picker::scanSeam().cached_agents = [found] {
        return std::optional<fv::agents::ScanResult>(found);
    };
    TabRig rig;

    const QStringList labels = menuTexts(rig->agentField());
    REQUIRE(labels.size() == 3);
    CHECK(labels[0] == QStringLiteral("Claude Code\tNot installed"));
    CHECK(labels[1].startsWith(QStringLiteral("Codex\t")));
    CHECK_FALSE(labels[1].endsWith(QStringLiteral("Not installed")));
    CHECK(labels[2] == QStringLiteral("OpenCode\tNot installed"));
    // The chosen agent, Claude Code, is not there: its field says so.
    CHECK(rig->agentField()->property("tone") == QVariant(QStringLiteral("warning")));
}

// from
// tests/test_walkthrough_write_up_notice.py::test_without_a_scan_one_runs_and_labels_stay_plain
TEST_CASE("without a scan one runs and labels stay plain",
          "[test_walkthrough_write_up_notice][widget]") {
    ScopedScanSeam restore;
    int scans = 0;
    fv::agent_picker::scanSeam().cached_agents = [] {
        return std::optional<fv::agents::ScanResult>();
    };
    fv::agent_picker::scanSeam().scan_installed_agents = [&scans](bool) {
        ++scans;
        return fv::agents::ScanResult{};
    };
    TabRig rig;
    rig->showAgents();
    rig->showAgents();
    rig.settle();

    CHECK(scans == 1);
    CHECK(statuses(rig) == QStringList(3, QStringLiteral("Checking\u2026")));
    CHECK(rig->agentField()->value() == QStringLiteral("Claude Code"));
    CHECK(rig->agentField()->property("tone").toString().isEmpty());
}

// A23-86 and A23-11: a screen reader hears what each switch does, which the visible row labels
// leave unsaid, and Follow my notes saves walkthrough_write_up_notes when toggled
// (walkthrough_tab.py:664-680, 794, 801).
TEST_CASE("the tab's switches are named for what they do and the notes switch is saved",
          "[widget]") {
    TabRig rig;

    CHECK(rig->writeUpSwitch()->accessibleName() ==
          QStringLiteral("Write walkthroughs up with an AI agent"));
    CHECK(rig->notesSwitch()->accessibleName() ==
          QStringLiteral("Follow notes left for the agent"));
    CHECK(rig->minimizeSwitch()->accessibleName() ==
          QStringLiteral("Minimize to tray when a walkthrough starts"));
    CHECK(rig->autoSwitch()->accessibleName() ==
          QStringLiteral("Auto Mode: capture clicks and drawings"));
    CHECK(rig->promptEdit()->accessibleName() == QStringLiteral("Write-up prompt"));
    CHECK(rig->presetCombo()->accessibleName() == QStringLiteral("Write-up style"));

    rig->notesSwitch()->setChecked(!rig->notesSwitch()->isChecked());
    CHECK(rig.saved().at("walkthrough_write_up_notes") == Json(rig->notesSwitch()->isChecked()));
    rig->notesSwitch()->setChecked(!rig->notesSwitch()->isChecked());
    CHECK(rig.saved().at("walkthrough_write_up_notes") == Json(rig->notesSwitch()->isChecked()));
}

} // namespace fvtest
