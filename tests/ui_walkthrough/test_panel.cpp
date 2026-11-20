// The live walkthrough column (ui_qt/widgets/walkthrough_panel.py), from
// tests/test_walkthrough_panel.py and tests/test_drag_to_move.py. A FakeClock stands for the
// patched time.monotonic() and drives the 16 ms frames.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <variant>
#include <vector>

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QHelpEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPoint>
#include <QScreen>
#include <QSignalSpy>
#include <QTest>
#include <QToolTip>
#include <QWheelEvent>

#include "panel_support.h"
#include "ui_walkthrough/screenshot_flight.h"
#include "ui_walkthrough/walkthrough_icons.h"

namespace fvtest {
namespace {

using fv::WalkthroughPanel;
using fv::ui_walkthrough::kHoldS;
using fv::ui_walkthrough::kThumbPx;
using fv::ui_walkthrough::Paragraph;
using fv::ui_walkthrough::ShotItem;
using State = WalkthroughPanel::State;

int tickCount(const WalkthroughPanel& panel) {
    int count = 0;
    for (const auto& item : panel.column().items) {
        if (const auto* paragraph = std::get_if<Paragraph>(&item)) {
            count += static_cast<int>(paragraph->ticks.size());
        }
    }
    return count;
}

const Paragraph& lastParagraph(const WalkthroughPanel& panel) {
    return std::get<Paragraph>(panel.column().items.back());
}

std::vector<int> shotNumbers(const WalkthroughPanel& panel) {
    std::vector<int> numbers;
    for (const auto& item : panel.column().items) {
        if (const auto* shot = std::get_if<ShotItem>(&item)) {
            numbers.push_back(shot->number);
        }
    }
    return numbers;
}

// Every signal the footer and the hover tools send, as OpenWhisper's _signals records them.
struct Heard {
    explicit Heard(WalkthroughPanel& panel) {
        QObject::connect(&panel, &WalkthroughPanel::snapRequested,
                         [this] { events.emplace_back("snap"); });
        QObject::connect(&panel, &WalkthroughPanel::undoRequested,
                         [this] { events.emplace_back("undo"); });
        QObject::connect(&panel, &WalkthroughPanel::cancelRequested,
                         [this] { events.emplace_back("cancel"); });
        QObject::connect(&panel, &WalkthroughPanel::finishRequested,
                         [this] { events.emplace_back("done"); });
        QObject::connect(&panel, &WalkthroughPanel::moveRequested, [this](int number, int step) {
            events.push_back("move " + std::to_string(number) + " " + std::to_string(step));
        });
        QObject::connect(&panel, &WalkthroughPanel::removeRequested, [this](int number) {
            events.push_back("remove " + std::to_string(number));
        });
    }
    std::vector<std::string> events;
};

std::vector<bool> heldOf(const QSignalSpy& spy) {
    std::vector<bool> held;
    for (const auto& arguments : spy) {
        held.push_back(arguments.at(0).toBool());
    }
    return held;
}

} // namespace

// --- the panel through a walkthrough ---

// from
// tests/test_walkthrough_panel.py::test_a_walkthrough_fills_the_column_then_it_fades_once_ready
TEST_CASE_METHOD(PanelFixture, "a walkthrough fills the column then it fades once ready",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    CHECK(panel.isVisible());
    CHECK(panel.state() == State::Recording);

    run(1.0, kLoud);
    panel.addShot(1, image());
    run(0.6, kLoud);
    panel.grab(); // paints every part without error
    CHECK(panel.column().shots() == 1);

    panel.finish();
    CHECK(panel.state() == State::Writing);
    panel.hear(kLoud); // after Done the voice writes no more
    const int ticks = tickCount(panel);
    run(1.0);
    CHECK(tickCount(panel) == ticks);
    panel.grab();

    panel.complete(1, 75.0);
    CHECK(panel.summary() == QStringLiteral(u"1:15 · 1 screenshot"));
    run(kHoldS - 0.5);
    CHECK(panel.isVisible());
    run(1.5);
    CHECK_FALSE(panel.isVisible());
    CHECK(panel.state() == State::Hidden);
}

// from tests/test_walkthrough_panel.py::test_a_canceled_walkthrough_fades_at_once
TEST_CASE_METHOD(PanelFixture, "a canceled walkthrough fades at once",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    run(0.5, kLoud);
    panel.finish();
    panel.dismiss();
    run(0.6);
    CHECK_FALSE(panel.isVisible());
}

// from tests/test_walkthrough_panel.py::test_screenshots_are_kept_as_thumbnails
TEST_CASE_METHOD(PanelFixture, "screenshots are kept as thumbnails",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.addShot(1, image(3840, 2160));
    const ShotItem* shot = panel.column().shot(1);
    REQUIRE(shot != nullptr);
    CHECK(shot->image->width() == kThumbPx);
}

// from tests/test_walkthrough_panel.py::test_a_new_walkthrough_starts_an_empty_column
TEST_CASE_METHOD(PanelFixture, "a new walkthrough starts an empty column",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    run(0.5, kLoud);
    panel.addShot(1, image());
    panel.finish();

    panel.begin();
    CHECK(panel.column().shots() == 0);
    CHECK(lastParagraph(panel).ticks.empty());
}

// from tests/test_walkthrough_panel.py::
//     test_the_column_never_takes_focus_but_takes_the_pointer_to_be_dragged
TEST_CASE_METHOD(PanelFixture, "the column never takes focus but takes the pointer to be dragged",
                 "[test_walkthrough_panel][widget]") {
    const Qt::WindowFlags flags = panel.windowFlags();
    CHECK(flags.testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK_FALSE(flags.testFlag(Qt::WindowTransparentForInput));
    CHECK(panel.testAttribute(Qt::WA_ShowWithoutActivating));
}

TEST_CASE_METHOD(PanelFixture, "the column is a frameless tool window that explains its buttons",
                 "[walkthrough_panel][widget]") {
    CHECK(panel.windowType() == Qt::Tool);
    CHECK(panel.windowFlags().testFlag(Qt::FramelessWindowHint));
    CHECK(panel.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    CHECK(panel.testAttribute(Qt::WA_TranslucentBackground));
    // Its buttons explain themselves though the column is never active.
    CHECK(panel.testAttribute(Qt::WA_AlwaysShowToolTips));
    CHECK_FALSE(panel.testAttribute(Qt::WA_QuitOnClose));
    CHECK(panel.hasMouseTracking());
}

TEST_CASE_METHOD(PanelFixture, "the column opens at the right edge of the screen, centred",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    REQUIRE(screen != nullptr);
    const QRect area = screen->availableGeometry();
    const int height = static_cast<int>(std::min(820.0, area.height() * 0.74)) + 2 * 14;
    CHECK(panel.size() == QSize(296 + 2 * 14, height));
    CHECK(panel.minimumSize() == panel.maximumSize());
    // 22 px from the edge to the card; the shadow reaches past it.
    CHECK(panel.x() == area.right() - (296 + 28) - 22 + 14 + 1);
    CHECK(panel.y() == area.top() + (area.height() - height) / 2);
}

// from tests/test_walkthrough_panel.py::test_a_tall_column_scrolls_to_keep_the_newest_in_view
TEST_CASE_METHOD(PanelFixture, "a tall column scrolls to keep the newest in view",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    for (int number = 1; number <= 6; ++number) {
        run(0.4, kLoud);
        panel.addShot(number, image(900, 1600));
    }
    for (int i = 0; i < 60; ++i) {
        panel.grab();
        run(0.03);
    }
    CHECK(panel.scroll() > 0.0);
}

// --- the column's buttons ---

// from tests/test_walkthrough_panel.py::test_the_footer_snaps_undoes_cancels_and_finishes
TEST_CASE_METHOD(PanelFixture, "the footer snaps undoes cancels and finishes",
                 "[test_walkthrough_panel][widget]") {
    Heard heard(panel);
    panel.begin();
    panel.grab();

    click(panel, footerButton("undo")); // no screenshot to undo yet
    panel.addShot(1, image(), 3.0);
    for (const char* key : {"snap", "undo", "cancel", "done"}) {
        click(panel, footerButton(key));
    }

    CHECK(heard.events == std::vector<std::string>{"snap", "undo", "cancel", "done"});
}

// from tests/test_walkthrough_panel.py::test_while_it_is_written_only_cancel_is_left
TEST_CASE_METHOD(PanelFixture, "while it is written only cancel is left",
                 "[test_walkthrough_panel][widget]") {
    Heard heard(panel);
    panel.begin();
    panel.finish();
    run(1.0);

    click(panel, footerButton("snap"));
    // Cancel has grown over the row Undo and Done were in.
    CHECK(panel.footerControl(QStringLiteral("cancel")).rect.width() ==
          Catch::Approx(panel.inner().width()));
    click(panel, footerButton("done"));

    CHECK(heard.events == std::vector<std::string>{"cancel"});
}

// from tests/test_walkthrough_panel.py::test_hovering_a_screenshot_offers_to_move_or_remove_it
TEST_CASE_METHOD(PanelFixture, "hovering a screenshot offers to move or remove it",
                 "[test_walkthrough_panel][widget]") {
    Heard heard(panel);
    panel.begin();
    panel.addShot(1, image(), 2.0);
    panel.addShot(2, image(), 5.0);
    run(1.0);
    panel.grab();

    const auto drawn = panel.shotRect(2);
    REQUIRE(drawn);
    panel.setHover(drawn->center());
    QPointF up;
    QPointF remove;
    std::vector<std::string> enabled;
    for (const auto& tool : panel.shotTools(2)) {
        if (!tool.enabled) {
            continue;
        }
        const QString name = tool.key.section(u':', 0, 0);
        enabled.push_back(name.toStdString());
        (name == QStringLiteral("up") ? up : remove) = tool.rect.center();
    }
    // The last screenshot can't go lower.
    CHECK(enabled == std::vector<std::string>{"remove", "up"});
    click(panel, up);
    click(panel, remove);

    CHECK(heard.events == std::vector<std::string>{"move 2 -1", "remove 2"});
}

// from tests/test_walkthrough_panel.py::test_the_board_follows_what_the_walkthrough_did
TEST_CASE_METHOD(PanelFixture, "the board follows what the walkthrough did",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.addShot(1, image(), 2.0);
    panel.addShot(2, image(), 5.0);

    panel.moveShot(2, -1);
    CHECK(shotNumbers(panel) == std::vector<int>{2, 1});
    panel.removeShot(1);
    CHECK(panel.column().shots() == 1);
    panel.grab();
}

// from tests/test_walkthrough_panel.py::test_dragging_anywhere_but_a_button_moves_the_column
TEST_CASE_METHOD(PanelFixture, "dragging anywhere but a button moves the column",
                 "[test_walkthrough_panel][widget]") {
    panel.begin();
    panel.grab();
    const QPointF header(panel.inner().left() + 10, panel.inner().top() + 10);

    CHECK(panel.hit(header).isEmpty());
    CHECK(panel.hit(footerButton("done")) == QStringLiteral("done"));
}

// from tests/test_walkthrough_panel.py::test_every_state_paints
TEST_CASE_METHOD(PanelFixture, "every state paints", "[test_walkthrough_panel][widget]") {
    panel.begin();
    for (const int number : {1, 2, 3}) {
        run(0.4, kLoud);
        panel.addShot(number, image(), number * 2.0);
    }
    run(1.0);
    panel.grab();
    REQUIRE_FALSE(panel.shotRects().empty());
    panel.setHover(panel.shotRects().front().second.center());
    CHECK(panel.hoverShot() != 0);
    panel.grab();
    panel.finish();
    run(0.2);
    panel.grab();
    panel.complete(3, 9.0);
    panel.grab();
}

TEST_CASE_METHOD(PanelFixture, "a press counts only when it is let go over the same control",
                 "[walkthrough_panel][widget]") {
    Heard heard(panel);
    panel.begin();
    panel.addShot(1, image(), 2.0);
    panel.grab();
    const QPoint done = footerButton("done").toPoint();
    const QPoint undo = footerButton("undo").toPoint();
    const QPoint header = QPointF(panel.inner().left() + 10, panel.inner().top() + 10).toPoint();

    // Pressed on Done, let go over the header or over Undo: nothing happens.
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, done);
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, header);
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, done);
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, undo);
    CHECK(heard.events.empty());
    // A right click is no click.
    QTest::mouseClick(&panel, Qt::RightButton, Qt::NoModifier, done);
    CHECK(heard.events.empty());

    click(panel, done);
    CHECK(heard.events == std::vector<std::string>{"done"});
}

TEST_CASE_METHOD(PanelFixture, "a column whose document never comes gives up after ten minutes",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    run(0.5, kLoud);
    panel.finish();
    run(1.0);
    // Just short of GIVE_UP_S in WRITING: still waiting.
    clock.advance(std::chrono::milliseconds(598'900));
    run(0.032);
    CHECK(panel.isVisible());
    CHECK(panel.state() == State::Writing);
    // Then it fades over FADE_S like any other end.
    run(0.2);
    CHECK(panel.isVisible());
    run(0.3);
    CHECK_FALSE(panel.isVisible());
    CHECK(panel.state() == State::Hidden);
}

TEST_CASE_METHOD(PanelFixture, "a finished column holds its summary, then fades",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    run(0.5);
    panel.complete(1, 4.0);
    CHECK(panel.state() == State::Done);
    run(kHoldS - 0.1);
    CHECK(panel.isVisible());
    // The fade takes FADE_S once the hold is over.
    run(0.3);
    CHECK(panel.isVisible());
    run(0.2);
    CHECK_FALSE(panel.isVisible());
}

TEST_CASE_METHOD(PanelFixture, "a flight lands when the column hides",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    panel.addShot(1, image(), 2.0, QRect(0, 0, 400, 300));
    REQUIRE(panel.flight() != nullptr);
    REQUIRE(panel.flight()->number() == 1);
    panel.hide();
    CHECK(panel.flight()->number() == 0);
    CHECK_FALSE(panel.flight()->isVisible());
}

// --- Hold for note ---

TEST_CASE_METHOD(PanelFixture, "a note ends when the button comes up where the column never heard",
                 "[walkthrough_panel][widget]") {
    QSignalSpy held(&panel, &WalkthroughPanel::noteHeld);
    panel.begin();
    panel.grab();
    const QPoint button = footerButton("note").toPoint();
    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, button);
    REQUIRE(panel.column().holding);

    // Still held: moving keeps the note.
    QMouseEvent dragging(QEvent::MouseMove, QPointF(button), QPointF(panel.mapToGlobal(button)),
                         Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&panel, &dragging);
    CHECK(panel.column().holding);
    // The release went to another window: the next move, with no button down, ends it.
    QMouseEvent free(QEvent::MouseMove, QPointF(button), QPointF(panel.mapToGlobal(button)),
                     Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&panel, &free);

    CHECK_FALSE(panel.column().holding);
    CHECK(heldOf(held) == std::vector<bool>{true, false});
    // Nothing is left pressed: a later release ends no second note.
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, button);
    CHECK(heldOf(held) == std::vector<bool>{true, false});
}

// from tests/test_walkthrough_panel.py::test_holding_the_button_writes_a_note_until_it_is_let_go
TEST_CASE_METHOD(PanelFixture, "holding the button writes a note until it is let go",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy held(&panel, &WalkthroughPanel::noteHeld);
    panel.begin();
    panel.grab();
    const QPoint button = footerButton("note").toPoint();

    QTest::mousePress(&panel, Qt::LeftButton, Qt::NoModifier, button);
    run(0.5, kLoud);
    panel.grab();
    // Let go off the button: the note still ends.
    QTest::mouseRelease(&panel, Qt::LeftButton, Qt::NoModifier, button + button / 4);
    run(0.3, kLoud);

    CHECK(heldOf(held) == std::vector<bool>{true, false});
    const auto& items = panel.column().items;
    REQUIRE(items.size() >= 2);
    const auto& note = std::get<Paragraph>(items[items.size() - 2]);
    const auto& after = std::get<Paragraph>(items.back());
    CHECK(note.note);
    CHECK_FALSE(note.ticks.empty());
    CHECK_FALSE(after.note);
    CHECK_FALSE(after.ticks.empty());
    panel.grab();
}

// from tests/test_walkthrough_panel.py::test_without_an_ai_write_up_there_is_no_one_to_read_a_note
TEST_CASE_METHOD(PanelFixture, "without an ai write up there is no one to read a note",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy held(&panel, &WalkthroughPanel::noteHeld);
    panel.begin();
    panel.setNotesOn(false);
    panel.grab();

    click(panel, footerButton("note"));

    CHECK(held.isEmpty());
}

// from tests/test_walkthrough_panel.py::test_done_while_held_ends_the_note
TEST_CASE_METHOD(PanelFixture, "done while held ends the note",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy held(&panel, &WalkthroughPanel::noteHeld);
    panel.begin();
    panel.hold(true);

    panel.finish();

    CHECK(heldOf(held) == std::vector<bool>{true, false});
    CHECK_FALSE(panel.column().holding);
}

// --- Pause ---

// from tests/test_walkthrough_panel.py::
//     test_pause_asks_and_the_paused_column_stops_its_clock_and_its_ticks
TEST_CASE_METHOD(PanelFixture, "pause asks and the paused column stops its clock and its ticks",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy asked(&panel, &WalkthroughPanel::pauseRequested);
    panel.begin();
    panel.grab();

    click(panel, footerButton("pause"));
    REQUIRE(asked.size() == 1);
    CHECK(asked.at(0).at(0).toBool());
    // OpenWhisper pins the recording clock at 10 s here, then lets it run on to 25 s.
    clock.advance(std::chrono::seconds(10));
    panel.setPaused(true);
    const auto ticks = lastParagraph(panel).ticks.size();
    clock.advance(std::chrono::seconds(15));
    run(0.5, kLoud);

    CHECK(panel.recorded() == Catch::Approx(10.0));
    CHECK(lastParagraph(panel).ticks.size() == ticks);
    panel.grab();

    click(panel, footerButton("pause"));
    REQUIRE(asked.size() == 2);
    CHECK_FALSE(asked.at(1).at(0).toBool());
    panel.setPaused(false);
    clock.advance(std::chrono::seconds(2));
    CHECK(panel.recorded() == Catch::Approx(12.0));
}

// from tests/test_walkthrough_panel.py::test_pausing_ends_a_note_and_no_note_starts_while_paused
TEST_CASE_METHOD(PanelFixture, "pausing ends a note and no note starts while paused",
                 "[test_walkthrough_panel][widget]") {
    QSignalSpy held(&panel, &WalkthroughPanel::noteHeld);
    panel.begin();
    panel.hold(true);

    panel.setPaused(true);
    panel.hold(true);

    CHECK(heldOf(held) == std::vector<bool>{true, false});
    CHECK_FALSE(panel.footerControl(QStringLiteral("note")).enabled);
}

// from tests/test_walkthrough_panel.py::
//     test_the_column_stands_in_for_the_overlay_only_while_it_records_or_writes
TEST_CASE_METHOD(PanelFixture,
                 "the column stands in for the overlay only while it records or writes",
                 "[test_walkthrough_panel][widget]") {
    CHECK_FALSE(panel.showsRecording());
    panel.begin();
    CHECK(panel.showsRecording());
    panel.finish();
    CHECK(panel.showsRecording());

    panel.complete(0, 3.0);

    // Its last seconds on screen don't hide the next dictation's overlay.
    CHECK(panel.isVisible());
    CHECK_FALSE(panel.showsRecording());
}

// from tests/test_walkthrough_panel.py::test_draw_is_off_while_auto_mode_is_paused
TEST_CASE("draw is off while auto mode is paused", "[test_walkthrough_panel][widget]") {
    FakeDesktopScope desktop;
    fv::FakeClock clock;
    WalkthroughPanel panel(clock);
    panel.begin();
    panel.setAutoMode(true);
    CHECK(panel.footerControl(QStringLiteral("snap")).enabled);
    panel.setPaused(true);
    // Live drawing is off while paused; the manual menu and Resume stay.
    CHECK_FALSE(panel.footerControl(QStringLiteral("snap")).enabled);
    CHECK(panel.footerControl(QStringLiteral("snap-menu")).enabled);
    CHECK(panel.footerControl(QStringLiteral("pause")).enabled);
    panel.setAutoMode(false);
    CHECK(panel.footerControl(QStringLiteral("snap")).enabled);
    panel.stopFrames();
    panel.close();
}

// --- dragging (tests/test_drag_to_move.py) ---

namespace {

void send(QWidget& widget, QEvent::Type kind, QPoint local) {
    const Qt::MouseButtons held =
        kind == QEvent::MouseButtonRelease ? Qt::MouseButtons(Qt::NoButton) : Qt::LeftButton;
    QMouseEvent event(kind, QPointF(local), QPointF(widget.mapToGlobal(local)), Qt::LeftButton,
                      held, Qt::NoModifier);
    QApplication::sendEvent(&widget, &event);
}

// Press at start, carry the pointer by, release; positions follow the window.
void drag(QWidget& widget, QPoint start, QPoint by) {
    send(widget, QEvent::MouseButtonPress, start);
    // The grab point stays under the pointer, so the event's local position is where it was
    // pressed, at a global spot by further on.
    const QPoint globalEnd = widget.mapToGlobal(start) + by;
    QMouseEvent move(QEvent::MouseMove, QPointF(start), QPointF(globalEnd), Qt::NoButton,
                     Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(&widget, &move);
    send(widget, QEvent::MouseButtonRelease, start);
}

} // namespace

// from tests/test_drag_to_move.py::
//     test_the_column_can_be_dragged_and_the_next_walkthrough_opens_at_the_edge
TEST_CASE_METHOD(PanelFixture,
                 "the column can be dragged and the next walkthrough opens at the edge",
                 "[test_drag_to_move][widget]") {
    panel.begin();
    const QPoint edge = panel.pos();

    drag(panel, QPoint(80, 80), QPoint(-60, 0));
    CHECK(panel.pos() == edge + QPoint(-60, 0));

    panel.begin();
    CHECK(panel.pos() == edge);
}

// --- the screenshot's flight ---

TEST_CASE_METHOD(PanelFixture, "a screenshot flies from where it was taken into its place",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    run(0.5);
    panel.grab();
    panel.addShot(1, image(3840, 2160), 2.0, QRect(100, 100, 640, 360));
    fv::ScreenshotFlight* flight = panel.flight();
    REQUIRE(flight != nullptr);
    CHECK(flight->number() == 1);
    CHECK(flight->isVisible());
    // Bounded even for a 4K screen, with the source and the slot inside its window.
    CHECK(flight->image().width() <= 1600);
    CHECK(flight->image().height() <= 1000);
    CHECK(flight->testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(flight->windowFlags().testFlag(Qt::WindowTransparentForInput));
    CHECK(flight->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
    // Its destination is reserved while it travels: the slot has the shot's rectangle.
    panel.grab();
    REQUIRE(panel.shotRect(1));

    run(0.3);
    CHECK(flight->progress() > 0.0);
    CHECK(flight->progress() < 1.0);
    CHECK(flight->geometry().contains(flight->cardRect().toAlignedRect()));
    run(0.5);
    CHECK(flight->number() == 0);
    CHECK_FALSE(flight->isVisible());
}

TEST_CASE_METHOD(PanelFixture, "a flight lands at removal, at the next capture and on cancel",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    panel.addShot(1, image(), 2.0, QRect(0, 0, 400, 300));
    REQUIRE(panel.flight() != nullptr);
    CHECK(panel.flight()->number() == 1);
    panel.removeShot(1);
    CHECK(panel.flight()->number() == 0);

    panel.addShot(2, image(), 3.0, QRect(0, 0, 400, 300));
    CHECK(panel.flight()->number() == 2);
    panel.addShot(3, image(), 4.0);
    CHECK(panel.flight()->number() == 0);

    panel.addShot(4, image(), 5.0, QRect(0, 0, 400, 300));
    CHECK(panel.flight()->number() == 4);
    panel.dismiss();
    CHECK(panel.flight()->number() == 0);
}

TEST_CASE_METHOD(PanelFixture, "nothing flies from an empty rectangle or a hidden column",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    panel.addShot(1, image(), 2.0, QRect());
    CHECK(panel.flight() == nullptr);
    panel.hide();
    panel.addShot(2, image(), 3.0, QRect(0, 0, 400, 300));
    CHECK(panel.flight() == nullptr);
    CHECK(panel.column().shots() == 2);
    // A null picture is no screenshot.
    panel.addShot(3, QImage(), 4.0);
    CHECK(panel.column().shots() == 2);
}

// --- what the column says ---

TEST_CASE_METHOD(PanelFixture, "the header says what the walkthrough is doing",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    CHECK(panel.windowTitle() == QStringLiteral("Walkthrough"));
    CHECK(panel.objectName() == QStringLiteral("walkthroughPanel"));
    panel.complete(2, 3725.0);
    CHECK(panel.summary() == QStringLiteral(u"1:02:05 · 2 screenshots"));
    // Only a walkthrough recording or being written completes.
    panel.complete(5, 1.0);
    CHECK(panel.summary() == QStringLiteral(u"1:02:05 · 2 screenshots"));
    panel.dismiss();
    CHECK(panel.summary().isEmpty());
}

namespace {

// What the column says when the pointer rests at point: the tooltip it shows, or "".
QString tooltipAt(WalkthroughPanel& panel, QPointF point) {
    QHelpEvent help(QEvent::ToolTip, point.toPoint(), panel.mapToGlobal(point.toPoint()));
    QApplication::sendEvent(&panel, &help);
    // With nothing to say the column ignores the event; a hidden tip lingers for a moment, so
    // its text would be the last one's.
    const QString text = help.isAccepted() ? QToolTip::text() : QString();
    QToolTip::hideText();
    return text;
}

} // namespace

TEST_CASE_METHOD(PanelFixture, "each control explains itself when the pointer rests on it",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    panel.setSnapMode(QStringLiteral("delayed"));
    panel.addShot(1, image(), 2.0);
    run(1.0);
    panel.grab();
    CHECK(tooltipAt(panel, footerButton("snap")) ==
          QStringLiteral("Take a screenshot: in 3 seconds"));
    CHECK(tooltipAt(panel, footerButton("snap-menu")) == QStringLiteral("Choose what Snap takes"));
    CHECK(tooltipAt(panel, footerButton("undo")) ==
          QStringLiteral("Undo the last screenshot; its words stay"));
    CHECK(tooltipAt(panel, footerButton("pause")) ==
          QStringLiteral("Pause: nothing is recorded until you resume"));
    CHECK(tooltipAt(panel, footerButton("cancel")) ==
          QStringLiteral("Cancel the walkthrough: nothing is kept"));
    CHECK(tooltipAt(panel, footerButton("done")) == QStringLiteral("Finish the walkthrough"));
    CHECK(tooltipAt(panel, footerButton("note")).startsWith(QStringLiteral("Hold while you talk")));
    // Nothing to say over the header.
    CHECK(tooltipAt(panel, QPointF(panel.inner().left() + 10, panel.inner().top() + 10)).isEmpty());

    const auto shot = panel.shotRect(1);
    REQUIRE(shot);
    panel.setHover(shot->center());
    for (const auto& tool : panel.shotTools(1)) {
        if (tool.key.startsWith(QStringLiteral("remove"))) {
            CHECK(tooltipAt(panel, tool.rect.center()) ==
                  QStringLiteral("Remove the screenshot; its words join the next one's"));
        }
    }

    panel.setPaused(true);
    CHECK(tooltipAt(panel, footerButton("pause")) == QStringLiteral("Resume recording"));
    panel.setPaused(false);
    panel.setNotesOn(false);
    CHECK(tooltipAt(panel, footerButton("note")) ==
          QStringLiteral("Turn on AI write-up on the Walkthrough tab to leave notes for it"));
    panel.setAutoMode(true);
    CHECK(tooltipAt(panel, footerButton("snap")) ==
          QStringLiteral("Draw on the live screen with the pen. Release to save a screenshot."));
    CHECK(tooltipAt(panel, footerButton("snap-menu")) ==
          QStringLiteral("Take a screenshot manually"));

    panel.finish();
    run(1.0);
    CHECK(tooltipAt(panel, footerButton("cancel")) ==
          QStringLiteral("Cancel: nothing is kept, unless the AI is already writing it up; then "
                         "it's kept as spoken"));
}

TEST_CASE("every glyph is a path on the 24-unit grid", "[walkthrough_icons][widget]") {
    for (const char* name :
         {"camera", "up", "down", "x", "undo", "check", "note", "pause", "play", "copy"}) {
        CAPTURE(name);
        const QPainterPath path = fv::walkthroughIconPath(name);
        CHECK_FALSE(path.isEmpty());
        CHECK(QRectF(0, 0, 24, 24).contains(path.boundingRect()));
    }
    CHECK(fv::walkthroughIconPath("star").isEmpty());
    // Painting an unknown glyph draws nothing rather than failing.
    QImage canvas(24, 24, QImage::Format_ARGB32_Premultiplied);
    canvas.fill(Qt::transparent);
    QPainter painter(&canvas);
    fv::paintWalkthroughIcon(painter, "star", QPointF(12, 12), 24, Qt::white);
    fv::paintWalkthroughIcon(painter, "pause", QPointF(12, 12), 24, Qt::white);
    painter.end();
    CHECK(canvas.pixelColor(8, 12).alpha() == 255); // the pause glyph's left bar, filled
    CHECK(canvas.pixelColor(12, 12).alpha() == 0);
}

TEST_CASE_METHOD(PanelFixture, "the wheel scrolls back and a new screenshot brings the newest",
                 "[walkthrough_panel][widget]") {
    panel.begin();
    for (int number = 1; number <= 6; ++number) {
        run(0.4, kLoud);
        panel.addShot(number, image(900, 1600));
    }
    // Each frame glides 16/180 of the way, so 200 frames arrive.
    for (int i = 0; i < 200; ++i) {
        panel.grab();
    }
    const double newest = panel.scroll();
    REQUIRE(newest > 0.0);
    QWheelEvent wheel(QPointF(100, 100), panel.mapToGlobal(QPointF(100, 100)), QPoint(),
                      QPoint(0, 240), Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
    QApplication::sendEvent(&panel, &wheel);
    for (int i = 0; i < 200; ++i) {
        panel.grab();
    }
    CHECK(panel.scroll() == Catch::Approx(newest - 120.0).margin(0.5));
    panel.addShot(7, image(900, 1600));
    for (int i = 0; i < 200; ++i) {
        panel.grab();
    }
    CHECK(panel.scroll() > newest);
}

} // namespace fvtest
