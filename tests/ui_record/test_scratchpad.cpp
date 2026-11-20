// The floating Scratchpad: showing it, saving it, dictating into it (OpenWhisper's
// tests/test_personalize_s8_scratchpad.py, test_personalize_fix2_g3_scratchpad_quit.py and the
// Scratchpad test of test_personalize_foundation_ui_shell.py). The transform tests are M4's and
// the compositor-managed window is another platform's. Every window here is offscreen, writes to
// a data folder of its own and copies to a recording clipboard; the focus the desktop gives a
// window is stood in for by the window's activity probe.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <tuple>
#include <vector>

#include <QAbstractButton>
#include <QApplication>
#include <QCursor>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QScreen>
#include <QSignalSpy>
#include <QSizeGrip>
#include <QTest>
#include <QTextCursor>
#include <QTimer>
#include <QToolButton>

#include "scratchpad_support.h"
#include "ui_record/scratchpad.h"
#include "ui_widgets/eliding_label.h"

namespace {

using fv::ScratchpadWindow;
using fvtest::fire;
using fvtest::PadRig;
namespace fs = std::filesystem;

QRect availableScreen() {
    const QScreen* screen = QGuiApplication::screenAt(QCursor::pos());
    if (screen == nullptr) {
        screen = QGuiApplication::primaryScreen();
    }
    return screen->availableGeometry();
}

void moveCursorTo(ScratchpadWindow& pad, int position, int anchor = -1) {
    QTextCursor cursor = pad.editor()->textCursor();
    if (anchor >= 0) {
        cursor.setPosition(anchor);
        cursor.setPosition(position, QTextCursor::KeepAnchor);
    } else {
        cursor.setPosition(position);
    }
    pad.editor()->setTextCursor(cursor);
}

QString status(const ScratchpadWindow& pad) {
    return pad.statusLabel()->text();
}

} // namespace

// ---- TestWindow ----

// from
// tests/test_personalize_s8_scratchpad.py::TestWindow::test_created_on_first_use_and_kept_on_the_controller
TEST_CASE("the Scratchpad is created on first use and kept by the service", "[widget]") {
    PadRig rig;
    CHECK(rig.service->window() == nullptr);
    fv::toggleScratchpad(*rig.service);
    ScratchpadWindow* pad = rig.service->window();
    REQUIRE(pad != nullptr);
    CHECK(pad->isVisible());
    rig.service->window()->setActivityProbe([] { return false; });
    fv::toggleScratchpad(*rig.service);
    CHECK(rig.service->window() == pad);
}

// from
// tests/test_personalize_s8_scratchpad.py::TestWindow::test_classic_window_floats_above_without_quitting_the_app
TEST_CASE("the classic Scratchpad floats above without quitting the app", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    const Qt::WindowFlags flags = pad.windowFlags();
    CHECK(flags.testFlag(Qt::Tool));
    CHECK(flags.testFlag(Qt::FramelessWindowHint));
    CHECK(flags.testFlag(Qt::WindowStaysOnTopHint));
    CHECK_FALSE(pad.testAttribute(Qt::WA_QuitOnClose));
    CHECK(pad.testAttribute(Qt::WA_TranslucentBackground));
    CHECK(pad.property("fiorivoceHotkeyWindow").toBool());
    CHECK(pad.header() != nullptr);
    CHECK(pad.sizeGrip() != nullptr);
    CHECK(pad.editor()->placeholderText() == QStringLiteral("Dictate or type notes…"));
}

// from
// tests/test_personalize_s8_scratchpad.py::TestWindow::test_toggle_hides_only_when_it_has_focus
TEST_CASE("toggling hides the Scratchpad only when it has focus", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    rig.active = false;
    fv::toggleScratchpad(*rig.service);
    CHECK(pad.isVisible());
    rig.active = true;
    fv::toggleScratchpad(*rig.service);
    CHECK_FALSE(pad.isVisible());
    // Hidden, it shows again and takes the editor's focus.
    fv::toggleScratchpad(*rig.service);
    CHECK(pad.isVisible());
}

// from
// tests/test_personalize_s8_scratchpad.py::TestWindow::test_on_top_can_be_turned_off_and_is_remembered
TEST_CASE("on top can be turned off and is remembered", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.onTopButton()->setChecked(false);
    CHECK_FALSE(pad.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    CHECK(pad.isVisible());
    CHECK_FALSE(rig.settings.get(fv::keys::scratchpad_always_on_top));

    ScratchpadWindow again(rig.environment(), rig.copier());
    CHECK_FALSE(again.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    CHECK_FALSE(again.onTopButton()->isChecked());
}

TEST_CASE("on top is on until it is turned off", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.onTopButton()->isChecked());
    pad.setAlwaysOnTop(false);
    CHECK_FALSE(pad.onTopButton()->isChecked());
    pad.setAlwaysOnTop(true);
    CHECK(pad.windowFlags().testFlag(Qt::WindowStaysOnTopHint));
    CHECK(pad.isVisible());
    CHECK(rig.settings.get(fv::keys::scratchpad_always_on_top));
}

TEST_CASE("changing the window flags keeps the place and shows the window again", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.setGeometry(QRect(60, 70, 420, 330));
    pad.setAlwaysOnTop(false);
    CHECK(pad.isVisible());
    CHECK(pad.geometry() == QRect(60, 70, 420, 330));
}

TEST_CASE("the close button hides the Scratchpad and closing is accepted", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.closeButton()->click();
    CHECK_FALSE(pad.isVisible());
    pad.present();
    CHECK(pad.close());
    CHECK_FALSE(pad.isVisible());
}

TEST_CASE("the header carries the window with the pointer", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.move(100, 100);
    QFrame* header = pad.header();
    const QPoint grab = header->mapToGlobal(QPoint(20, 10));
    QMouseEvent press(QEvent::MouseButtonPress, QPointF(20, 10), QPointF(grab), Qt::LeftButton,
                      Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &press);
    CHECK(header->cursor().shape() == Qt::ClosedHandCursor);
    QMouseEvent drag(QEvent::MouseMove, QPointF(20, 10), QPointF(grab + QPoint(30, 40)),
                     Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &drag);
    CHECK(pad.frameGeometry().topLeft() == QPoint(130, 140));
    QMouseEvent release(QEvent::MouseButtonRelease, QPointF(20, 10), QPointF(grab + QPoint(30, 40)),
                        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(header, &release);
    CHECK(header->cursor().shape() == Qt::OpenHandCursor);
    // A move after the release carries nothing.
    QMouseEvent after(QEvent::MouseMove, QPointF(20, 10), QPointF(grab + QPoint(90, 90)),
                      Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(header, &after);
    CHECK(pad.frameGeometry().topLeft() == QPoint(130, 140));
}

// ---- TestGeometry ----

// from
// tests/test_personalize_s8_scratchpad.py::TestGeometry::test_saved_geometry_comes_back_clamped_to_the_screen
TEST_CASE("a saved geometry comes back clamped to the screen", "[widget]") {
    const QRect available = availableScreen();
    PadRig rig(fv::Json{{"scratchpad_geometry",
                         {{"x", available.right() + 5000},
                          {"y", available.top() - 900},
                          {"width", 430},
                          {"height", 330}}}});
    ScratchpadWindow& pad = rig.open();
    const QRect geometry = pad.geometry();
    CHECK(geometry.width() == 430);
    CHECK(geometry.height() == 330);
    CHECK(available.contains(geometry));
}

// from tests/test_personalize_s8_scratchpad.py::TestGeometry::test_moving_saves_the_geometry
TEST_CASE("moving saves the geometry", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.setGeometry(QRect(120, 140, 410, 320));
    CHECK(pad.geometryTimer()->isActive());
    CHECK(pad.geometryTimer()->interval() == ScratchpadWindow::kGeometrySaveMs);
    fire(pad.geometryTimer());
    const fv::Json saved = rig.settings.get(fv::keys::scratchpad_geometry);
    CHECK(saved == fv::Json{{"x", 120}, {"y", 140}, {"width", 410}, {"height", 320}});
}

// from
// tests/test_personalize_s8_scratchpad.py::TestGeometry::test_bad_saved_geometry_falls_back_to_a_default
TEST_CASE("a bad saved geometry falls back to a default", "[widget]") {
    PadRig rig(
        fv::Json{{"scratchpad_geometry", {{"x", "left"}, {"y", 1}, {"width", 2}, {"height", 3}}}});
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.width() >= pad.minimumSizeHint().width());
    CHECK(pad.height() >= pad.minimumSizeHint().height());
    // The default place, not the saved 2x3 lifted to the minimum.
    const QRect available = availableScreen();
    CHECK(pad.size() ==
          QSize(std::max(std::min(400, available.width()), pad.minimumSizeHint().width()),
                std::max(std::min(380, available.height()), pad.minimumSizeHint().height())));
}

TEST_CASE("with nothing saved the window opens 40 px from the right edge, centred", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    const QRect available = availableScreen();
    const int width = std::min(400, available.width());
    const int height = std::min(380, available.height());
    CHECK(pad.size() == QSize(std::max(width, pad.minimumSizeHint().width()),
                              std::max(height, pad.minimumSizeHint().height())));
    CHECK(pad.x() == std::min(available.right() - width - 40, available.right() - pad.width() + 1));
    CHECK(pad.y() == available.top() + (available.height() - height) / 2);
}

TEST_CASE("the default size follows the UI font scale", "[widget]") {
    PadRig rig(fv::Json::object(), QStringLiteral("dark"), 130);
    ScratchpadWindow& pad = rig.open();
    const QRect available = availableScreen();
    // round(400 * 1.3) x round(380 * 1.3), clamped to the screen, never under the minimum.
    const int width = std::min(520, available.width());
    const int height = std::min(494, available.height());
    CHECK(pad.size() == QSize(std::max(width, pad.minimumSizeHint().width()),
                              std::max(height, pad.minimumSizeHint().height())));
}

TEST_CASE("a number saved as text or a float is read as Python's int would", "[widget]") {
    const QRect available = availableScreen();
    PadRig rig(fv::Json{{"scratchpad_geometry",
                         {{"x", std::to_string(available.left() + 10)},
                          {"y", static_cast<double>(available.top() + 20) + 0.9},
                          {"width", "450"},
                          {"height", 340.7}}}});
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.geometry() == QRect(available.left() + 10, available.top() + 20, 450, 340));
}

TEST_CASE("hiding saves a geometry that changed at once", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.setGeometry(QRect(80, 90, 400, 320));
    CHECK(pad.geometryTimer()->isActive());
    pad.hide();
    CHECK_FALSE(pad.geometryTimer()->isActive());
    CHECK(rig.settings.get(fv::keys::scratchpad_geometry) ==
          fv::Json{{"x", 80}, {"y", 90}, {"width", 400}, {"height", 320}});
}

// ---- TestSaving ----

// from tests/test_personalize_s8_scratchpad.py::TestSaving::test_loads_saved_notes_on_first_show
TEST_CASE("the saved notes load on first show", "[widget]") {
    PadRig rig;
    rig.write("Shopping: café crème\r\noat milk");
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.text() == QStringLiteral("Shopping: café crème\noat milk"));
    CHECK(status(pad) == QStringLiteral("5 words"));
    CHECK_FALSE(pad.autosaveTimer()->isActive());
}

// from
// tests/test_personalize_s8_scratchpad.py::TestSaving::test_edits_autosave_atomically_as_utf8_with_lf
TEST_CASE("edits autosave atomically as UTF-8 with LF", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("Line one ✓\nLine two"));
    CHECK(pad.autosaveTimer()->isActive());
    CHECK(pad.autosaveTimer()->interval() == ScratchpadWindow::kAutosaveMs);
    fire(pad.autosaveTimer());
    rig.settle();
    CHECK(pad.saver().idle());
    CHECK(rig.saved() == "Line one \xE2\x9C\x93\nLine two");
    CHECK(rig.saved().find("\r\n") == std::string::npos);
    int leftovers = 0;
    for (const auto& entry : fs::directory_iterator(rig.file().parent_path())) {
        leftovers += entry.path().extension() == ".tmp" ? 1 : 0;
    }
    CHECK(leftovers == 0);
}

// from
// tests/test_personalize_s8_scratchpad.py::TestSaving::test_hiding_writes_pending_edits_at_once
TEST_CASE("hiding writes pending edits at once", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("Remember the milk"));
    pad.hide();
    CHECK(rig.saved() == "Remember the milk");
    CHECK_FALSE(pad.autosaveTimer()->isActive());
}

// from
// tests/test_personalize_s8_scratchpad.py::TestSaving::test_an_older_save_never_lands_over_a_newer_one
TEST_CASE("an older save never lands over a newer one", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("first"));
    fire(pad.autosaveTimer());
    // The strand has not run: a text is waiting there while the hide writes the newer one.
    pad.editor()->insertPlainText(QStringLiteral(" second"));
    pad.hide();
    rig.settle();
    CHECK(rig.saved() == "first second");
}

// from
// tests/test_personalize_s8_scratchpad.py::TestSaving::test_nothing_is_written_before_the_notes_are_loaded
TEST_CASE("nothing is written before the notes are loaded", "[widget]") {
    PadRig rig;
    ScratchpadWindow window(rig.environment(), rig.copier());
    window.flush();
    rig.settle();
    CHECK_FALSE(fs::exists(rig.file()));
}

TEST_CASE("the service writes pending notes and lets the dictation port go when it ends",
          "[widget]") {
    PadRig rig;
    bool released = false;
    rig.open().editor()->insertPlainText(QStringLiteral("Unsaved"));
    rig.service->setReleaseHook([&released] { released = true; });
    rig.service.reset();
    CHECK(released);
    CHECK(rig.saved() == "Unsaved");
}

TEST_CASE("quitting writes pending edits", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("Before the quit"));
    QMetaObject::invokeMethod(qApp, "aboutToQuit", Qt::DirectConnection);
    CHECK(rig.saved() == "Before the quit");
}

TEST_CASE("notes that cannot be read say so and are left alone", "[widget]") {
    PadRig rig;
    // A folder where the notes file should be: it exists, but cannot be read.
    fs::create_directory(rig.file());
    ScratchpadWindow& pad = rig.open();
    CHECK(status(pad) == QStringLiteral("Couldn't open your saved notes"));
    CHECK(pad.noticeTimer()->isActive());
    CHECK(pad.text().isEmpty());
    CHECK(pad.isLoaded());
}

TEST_CASE("invalid UTF-8 in the notes is replaced, not fatal", "[widget]") {
    PadRig rig;
    rig.write(std::string("ok \xFF done"));
    ScratchpadWindow& pad = rig.open();
    CHECK(pad.text().startsWith(QStringLiteral("ok ")));
    CHECK(pad.text().endsWith(QStringLiteral(" done")));
    CHECK(pad.text().contains(QChar(0xFFFD)));
}

TEST_CASE("loading the notes does not queue a save", "[widget]") {
    PadRig rig;
    rig.write("Old notes");
    ScratchpadWindow& pad = rig.open();
    CHECK_FALSE(pad.autosaveTimer()->isActive());
    pad.hide();
    CHECK(rig.saved() == "Old notes");
}

TEST_CASE("an edit restarts the autosave countdown", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("a"));
    CHECK(pad.autosaveTimer()->isSingleShot());
    CHECK(pad.autosaveTimer()->isActive());
    // The timer's slot submits the text; the write is the strand's.
    fire(pad.autosaveTimer());
    CHECK_FALSE(pad.saver().idle());
    rig.settle();
    CHECK(pad.saver().idle());
    CHECK(rig.saved() == "a");
}

// ---- TestDictation ----

// from
// tests/test_personalize_s8_scratchpad.py::TestDictation::test_insert_needs_a_visible_focused_scratchpad
TEST_CASE("a dictation needs a visible, focused Scratchpad", "[widget]") {
    PadRig rig;
    CHECK_FALSE(fv::insertIntoScratchpad(*rig.service, QStringLiteral("hello")));
    ScratchpadWindow& pad = rig.open();
    rig.active = false;
    CHECK_FALSE(fv::insertIntoScratchpad(*rig.service, QStringLiteral("hello")));
    rig.active = true;
    CHECK_FALSE(fv::insertIntoScratchpad(*rig.service, QString()));
    CHECK(fv::insertIntoScratchpad(*rig.service, QStringLiteral("hello")));
    pad.hide();
    CHECK_FALSE(fv::insertIntoScratchpad(*rig.service, QStringLiteral("hello")));
    CHECK(pad.text() == QStringLiteral("hello"));
}

// from
// tests/test_personalize_s8_scratchpad.py::TestDictation::test_dictation_lands_at_the_cursor_with_sensible_spacing
TEST_CASE("a dictation lands at the cursor with sensible spacing", "[widget]") {
    struct Row {
        const char* before;
        const char* after;
        const char* dictated;
        const char* expected;
    };
    const Row row = GENERATE(
        Row{"Hello", "", "world", "Hello world"}, Row{"Hello ", "", "world", "Hello world"},
        Row{"", "", "First note", "First note"}, Row{"Call (", ")", "Sam", "Call (Sam)"},
        Row{"Done", "", ".", "Done."}, Row{"Start ", "end", "middle", "Start middle end"},
        Row{"Line\n", "", "next", "Line\nnext"});
    INFO(row.before << " | " << row.after << " | " << row.dictated);
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QString::fromUtf8(row.before) + QString::fromUtf8(row.after));
    moveCursorTo(pad, static_cast<int>(QString::fromUtf8(row.before).size()));
    CHECK(fv::insertIntoScratchpad(*rig.service, QString::fromUtf8(row.dictated)));
    CHECK(pad.text() == QString::fromUtf8(row.expected));
    CHECK(pad.autosaveTimer()->isActive());
}

// from tests/test_personalize_s8_scratchpad.py::TestDictation::test_dictation_replaces_a_selection
TEST_CASE("a dictation replaces a selection", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Meet at noon today"));
    moveCursorTo(pad, 12, 8);
    fv::insertIntoScratchpad(*rig.service, QStringLiteral("three"));
    CHECK(pad.text() == QStringLiteral("Meet at three today"));
}

// from
// tests/test_personalize_s8_scratchpad.py::TestDictation::test_the_join_step_sees_the_text_around_the_cursor
TEST_CASE("the join step sees the text around the cursor", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    std::vector<fv::focus::TextContext> seen;
    pad.setDictationJoiner([&seen](std::string_view text, const fv::focus::TextContext& context) {
        seen.push_back(context);
        std::string lowered(text);
        for (char& c : lowered) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        return lowered;
    });
    pad.editor()->setPlainText(QStringLiteral("Emoji 😀 before"));
    pad.editor()->moveCursor(QTextCursor::End);
    fv::insertIntoScratchpad(*rig.service, QStringLiteral("And After"));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].before == "Emoji \xF0\x9F\x98\x80 before");
    CHECK(seen[0].after.empty());
    CHECK(seen[0].selected.empty());
    CHECK(seen[0].caret_known);
    CHECK(seen[0].selection_known);
    CHECK(seen[0].source == "scratchpad");
    CHECK(pad.text() == QStringLiteral("Emoji 😀 before and after"));
}

TEST_CASE("the context is cut in UTF-16 units, never through a surrogate pair", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    std::vector<fv::focus::TextContext> seen;
    pad.setDictationJoiner([&seen](std::string_view text, const fv::focus::TextContext& context) {
        seen.push_back(context);
        return std::string(text);
    });
    // 1 unit, 700 emoji of 2 units each and 1 unit: 1402 units before the cursor, so the 1000
    // read start between the halves of an emoji, whose second half is dropped.
    QString notes = QStringLiteral("a");
    for (int i = 0; i < 700; ++i) {
        notes += QStringLiteral("😀");
    }
    pad.editor()->setPlainText(notes + QStringLiteral("b"));
    pad.editor()->moveCursor(QTextCursor::End);
    fv::insertIntoScratchpad(*rig.service, QStringLiteral("x"));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].before.size() == 499 * 4 + 1);
    CHECK(seen[0].before.rfind("\xF0\x9F\x98\x80", 0) == 0);
    CHECK(seen[0].before.back() == 'b');
    // And 200 units after the selection.
    seen.clear();
    QString after;
    for (int i = 0; i < 150; ++i) {
        after += QStringLiteral("😀");
    }
    pad.editor()->setPlainText(QStringLiteral("start") + after);
    moveCursorTo(pad, 5);
    fv::insertIntoScratchpad(*rig.service, QStringLiteral("x"));
    REQUIRE(seen.size() == 1);
    CHECK(seen[0].after.size() == 100 * 4);
}

TEST_CASE("a join that fails leaves the dictation as spoken", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.setDictationJoiner([](std::string_view, const fv::focus::TextContext&) -> std::string {
        throw std::runtime_error("no");
    });
    pad.editor()->setPlainText(QStringLiteral("Hello"));
    pad.editor()->moveCursor(QTextCursor::End);
    CHECK(fv::insertIntoScratchpad(*rig.service, QStringLiteral("World")));
    CHECK(pad.text() == QStringLiteral("Hello World"));
}

TEST_CASE("the dictation goes in through the dictation port", "[widget]") {
    PadRig rig;
    fv::dictation::ScratchpadPort& port = *rig.service;
    CHECK_FALSE(port.is_focused());
    CHECK_FALSE(port.insert("nothing yet"));
    rig.open();
    CHECK(port.is_focused());
    CHECK(port.insert("Spoken words"));
    CHECK(rig.service->window()->text() == QStringLiteral("Spoken words"));
    rig.active = false;
    CHECK_FALSE(port.is_focused());
    CHECK_FALSE(port.insert("elsewhere"));
}

// from
// tests/test_personalize_foundation_ui_shell.py::TestControllerHooks::test_scratchpad_insert_answers_with_a_real_bool
TEST_CASE("the port's insert answers with exactly what the Scratchpad did", "[widget]") {
    PadRig rig;
    rig.open();
    const bool focused = GENERATE(true, false);
    rig.active = focused;
    const bool answered = rig.service->insert("note");
    CHECK(answered == focused);
    CHECK(rig.service->window()->text() == (focused ? QStringLiteral("note") : QString()));
}

// ---- show_text ----

TEST_CASE("show_text adds an answer on a line of its own without taking focus", "[widget]") {
    PadRig rig;
    rig.write("Old notes");
    fv::showScratchpadText(*rig.service, QStringLiteral("  The answer.  "));
    ScratchpadWindow* pad = rig.service->window();
    REQUIRE(pad != nullptr);
    // The notes loaded first, so the answer did not replace them.
    CHECK(pad->text() == QStringLiteral("Old notes\n\nThe answer."));
    CHECK(pad->isVisible());
    CHECK_FALSE(pad->testAttribute(Qt::WA_ShowWithoutActivating));
    CHECK(pad->autosaveTimer()->isActive());
    // Added to a window that is already shown: nothing is hidden and shown again.
    fv::showScratchpadText(*rig.service, QStringLiteral("Second."));
    CHECK(pad->text() == QStringLiteral("Old notes\n\nThe answer.\n\nSecond."));
}

TEST_CASE("an arriving answer shows the Scratchpad quietly and a user's open focuses its editor",
          "[widget]") {
    class ShowProbe final : public QObject {
    public:
        std::vector<bool> silentShows;
        bool eventFilter(QObject* watched, QEvent* event) override {
            if (event->type() == QEvent::Show) {
                auto* window = qobject_cast<QWidget*>(watched);
                silentShows.push_back(window->testAttribute(Qt::WA_ShowWithoutActivating));
            }
            return false;
        }
    } probe;
    PadRig rig;
    auto& pad = rig.service->ensureWindow();
    pad.installEventFilter(&probe);
    pad.setActivityProbe([] { return false; });
    SECTION("the first answer arrives before a normal open") {
        fv::showScratchpadText(*rig.service, QStringLiteral("first"));
        REQUIRE(pad.isVisible());
        CHECK(probe.silentShows == std::vector<bool>{true});
        pad.hide();
        fv::toggleScratchpad(*rig.service);
        CHECK(probe.silentShows == std::vector<bool>{true, false});
    }
    SECTION("adding to an already open pad does not show it again") {
        fv::toggleScratchpad(*rig.service);
        CHECK(pad.focusWidget() == pad.editor());
        fv::showScratchpadText(*rig.service, QStringLiteral("reply"));
        CHECK(pad.text().endsWith(QStringLiteral("reply")));
        CHECK(probe.silentShows == std::vector<bool>{false});
    }
}

TEST_CASE("show_text with no text does nothing, not even build the window", "[widget]") {
    PadRig rig;
    fv::showScratchpadText(*rig.service, QString());
    CHECK(rig.service->window() == nullptr);
}

TEST_CASE("an appended block is separated by the blank line the notes need", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.appendBlock(QStringLiteral(" first "));
    CHECK(pad.text() == QStringLiteral("first"));
    pad.appendBlock(QStringLiteral("second"));
    CHECK(pad.text() == QStringLiteral("first\n\nsecond"));
    pad.editor()->setPlainText(QStringLiteral("ends with a newline\n"));
    pad.appendBlock(QStringLiteral("third"));
    CHECK(pad.text() == QStringLiteral("ends with a newline\n\nthird"));
    pad.editor()->setPlainText(QStringLiteral("  \n "));
    pad.appendBlock(QStringLiteral("fourth"));
    CHECK(pad.text() == QStringLiteral("  \n fourth"));
}

TEST_CASE("an appended block leaves the user's selection where it was", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Keep this selected"));
    moveCursorTo(pad, 9, 5);
    pad.appendBlock(QStringLiteral("An answer"));
    CHECK(pad.editor()->textCursor().selectedText() == QStringLiteral("this"));
}

// ---- TestFooter ----

// from tests/test_personalize_s8_scratchpad.py::TestFooter::test_word_count_follows_the_text
TEST_CASE("the word count follows the text", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    CHECK(status(pad).isEmpty());
    pad.editor()->setPlainText(QStringLiteral("one"));
    CHECK(status(pad) == QStringLiteral("1 word"));
    pad.editor()->setPlainText(QStringLiteral("one two  three\nfour"));
    CHECK(status(pad) == QStringLiteral("4 words"));
}

TEST_CASE("a long text counts its words with a thousands separator", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("word ").repeated(1234));
    CHECK(status(pad) == QStringLiteral("1,234 words"));
}

// from
// tests/test_personalize_s8_scratchpad.py::TestFooter::test_copy_all_uses_the_apps_clipboard_helper
TEST_CASE("Copy all uses the app's clipboard helper", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.copyAll();
    CHECK(status(pad) == QStringLiteral("Nothing to copy yet"));
    CHECK(rig.copied.empty());
    pad.editor()->setPlainText(QStringLiteral("Keep this"));
    pad.copyAll();
    REQUIRE(rig.copied.size() == 1);
    CHECK(rig.copied[0] == QStringLiteral("Keep this"));
    CHECK(status(pad) == QStringLiteral("Copied"));
    CHECK(pad.noticeTimer()->interval() == ScratchpadWindow::kNoticeMs);
    fire(pad.noticeTimer());
    CHECK(status(pad) == QStringLiteral("2 words"));
}

TEST_CASE("Copy all says when the clipboard refused or there is none", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Keep this"));
    rig.copyWorks = false;
    pad.copyButton()->click();
    CHECK(status(pad) == QStringLiteral("Couldn't copy"));

    ScratchpadWindow without(rig.environment(), {});
    without.editor()->setPlainText(QStringLiteral("Keep this"));
    without.copyAll();
    CHECK(status(without) == QStringLiteral("Couldn't copy"));
}

TEST_CASE("a notice is not overwritten by the word count", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Keep this"));
    pad.editor()->moveCursor(QTextCursor::End);
    pad.copyAll();
    pad.editor()->insertPlainText(QStringLiteral(" and more"));
    CHECK(status(pad) == QStringLiteral("Copied"));
    fire(pad.noticeTimer());
    CHECK(status(pad) == QStringLiteral("4 words"));
}

// from tests/test_personalize_s8_scratchpad.py::TestFooter::test_clear_has_one_step_undo
TEST_CASE("Clear has a one-step undo", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Important thought"));
    pad.clearButton()->click();
    CHECK(pad.text().isEmpty());
    CHECK(pad.clearButton()->text() == QStringLiteral("Undo clear"));
    CHECK(pad.undoTimer()->isActive());
    CHECK(pad.undoTimer()->interval() == ScratchpadWindow::kUndoClearMs);
    pad.clearButton()->click();
    CHECK(pad.text() == QStringLiteral("Important thought"));
    CHECK(pad.clearButton()->text() == QStringLiteral("Clear"));
    CHECK_FALSE(pad.undoTimer()->isActive());
}

// from tests/test_personalize_s8_scratchpad.py::TestFooter::test_typing_after_clear_drops_the_undo
TEST_CASE("typing after Clear drops the undo", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Old"));
    pad.clearText();
    pad.editor()->insertPlainText(QStringLiteral("New"));
    CHECK(pad.clearButton()->text() == QStringLiteral("Clear"));
    pad.clearButton()->click();
    CHECK(pad.text().isEmpty());
}

// from tests/test_personalize_s8_scratchpad.py::TestFooter::test_undo_offer_expires
TEST_CASE("the undo offer expires", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Old"));
    pad.clearText();
    fire(pad.undoTimer());
    CHECK(pad.clearButton()->text() == QStringLiteral("Clear"));
    CHECK_FALSE(pad.clearedText().has_value());
}

TEST_CASE("clearing an empty Scratchpad offers nothing to undo", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.clearText();
    CHECK_FALSE(pad.clearedText().has_value());
    CHECK(pad.clearButton()->text() == QStringLiteral("Clear"));
    pad.undoClear();
    CHECK(pad.text().isEmpty());
}

TEST_CASE("clearing saves the empty notes, and the undo saves them back", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->setPlainText(QStringLiteral("Important thought"));
    pad.clearText();
    CHECK(pad.autosaveTimer()->isActive());
    pad.hide();
    CHECK(rig.saved().empty());
    pad.present();
    pad.undoClear();
    pad.hide();
    CHECK(rig.saved() == "Important thought");
}

TEST_CASE("the transform menu says there are no transforms until M4 fills it", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("No transforms yet"));
    CHECK_FALSE(pad.transformMenu()->actions()[0]->isEnabled());
    CHECK(pad.transformButton()->text() == QStringLiteral("Transform ▾"));
}

TEST_CASE("a menu filler fills the menu again every time it opens", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    int fills = 0;
    ScratchpadWindow* seen = nullptr;
    pad.setTransformMenuFiller([&](QMenu* menu, ScratchpadWindow& window) {
        ++fills;
        seen = &window;
        menu->addAction(QStringLiteral("Polish"));
    });
    pad.fillTransformMenu();
    pad.fillTransformMenu();
    CHECK(fills == 2);
    CHECK(seen == &pad);
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("Polish"));
}

TEST_CASE("a menu filler that fails leaves the menu saying there are none", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.setTransformMenuFiller([](QMenu* menu, ScratchpadWindow&) {
        menu->addAction(QStringLiteral("Half"));
        throw std::runtime_error("no transforms");
    });
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("No transforms yet"));
    CHECK_FALSE(pad.transformMenu()->actions()[0]->isEnabled());
}

TEST_CASE("a filler given to the service reaches the window now and later", "[widget]") {
    PadRig rig;
    rig.service->setTransformMenuFiller(
        [](QMenu* menu, ScratchpadWindow&) { menu->addAction(QStringLiteral("Later")); });
    ScratchpadWindow& pad = rig.open();
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("Later"));
    rig.service->setTransformMenuFiller(
        [](QMenu* menu, ScratchpadWindow&) { menu->addAction(QStringLiteral("Now")); });
    pad.fillTransformMenu();
    REQUIRE(pad.transformMenu()->actions().size() == 1);
    CHECK(pad.transformMenu()->actions()[0]->text() == QStringLiteral("Now"));
}

TEST_CASE("the footer buttons are styled as scratchpad actions", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    for (QToolButton* button : {pad.transformButton(), pad.copyButton(), pad.clearButton()}) {
        CHECK(button->property("scratchpadAction").toBool());
        CHECK(button->focusPolicy() == Qt::TabFocus);
    }
    CHECK(pad.surface()->property("framed").toBool());
    CHECK(pad.copyButton()->text() == QStringLiteral("Copy all"));
}

// from tests/test_personalize_s8_scratchpad.py::test_footer_fits_a_narrow_window_at_large_fonts
TEST_CASE("the footer fits a narrow window at large fonts", "[widget]") {
    PadRig rig(fv::Json::object(), QStringLiteral("dark"), 130);
    ScratchpadWindow window(rig.environment(), rig.copier());
    window.present();
    window.resize(300, 260);
    for (int i = 0; i < 8; ++i) {
        QApplication::processEvents();
    }
    CHECK(window.width() <= 460);
    for (QAbstractButton* button : window.findChildren<QAbstractButton*>()) {
        if (button->isVisible()) {
            CHECK(button->mapTo(&window, button->rect().bottomRight()).x() < window.width());
            CHECK(button->mapTo(&window, button->rect().topLeft()).x() >= 0);
        }
    }
    CHECK(window.editor()->height() > 40);
    window.hide();
}

// ---- the quit tests ----

namespace {

// Stands in for MainWindow after quit_application set _force_quit.
class QuittingWindow : public QWidget {
protected:
    void closeEvent(QCloseEvent* event) override { event->accept(); }
};

} // namespace

// from
// tests/test_personalize_fix2_g3_scratchpad_quit.py::test_closing_is_accepted_and_keeps_the_notes
TEST_CASE("closing the Scratchpad is accepted and keeps the notes", "[widget]") {
    PadRig rig;
    ScratchpadWindow& pad = rig.open();
    pad.editor()->insertPlainText(QStringLiteral("Call the plumber"));
    CHECK(pad.autosaveTimer()->isActive());

    // QApplication::closeAllWindows stops at the first close() that returns false.
    CHECK(pad.close());
    CHECK_FALSE(pad.isVisible());
    CHECK(rig.saved() == "Call the plumber");

    pad.present();
    CHECK(pad.isVisible());
    CHECK(pad.text() == QStringLiteral("Call the plumber"));
}

// from
// tests/test_personalize_fix2_g3_scratchpad_quit.py::test_closing_the_last_visible_window_does_not_quit
TEST_CASE("closing the last visible window does not quit", "[widget]") {
    // With the main window in the tray, Alt+F4 or the compositor's close only hides.
    PadRig rig;
    REQUIRE(QApplication::quitOnLastWindowClosed());
    ScratchpadWindow& pad = rig.open();
    QApplication::processEvents();

    QTimer::singleShot(0, &pad, [&pad] { pad.close(); });
    QTimer::singleShot(300, qApp, [] { QApplication::exit(5); });
    CHECK(QApplication::exec() == 5);
    CHECK_FALSE(pad.isVisible());
}

// from
// tests/test_personalize_fix2_g3_scratchpad_quit.py::test_quit_is_not_vetoed_by_an_open_scratchpad
TEST_CASE("quit is not vetoed by an open Scratchpad", "[widget]") {
    PadRig rig;
    // closeAllWindows walks top-level widgets in pointer-hash order, so build fresh windows in
    // alternating order to put the Scratchpad first in some runs.
    std::vector<std::tuple<int, bool, bool, bool>> outcomes;
    for (int attempt = 0; attempt < 12; ++attempt) {
        std::unique_ptr<ScratchpadWindow> pad;
        std::unique_ptr<QuittingWindow> main;
        if (attempt % 2 != 0) {
            pad = std::make_unique<ScratchpadWindow>(rig.environment(), rig.copier());
            main = std::make_unique<QuittingWindow>();
        } else {
            main = std::make_unique<QuittingWindow>();
            pad = std::make_unique<ScratchpadWindow>(rig.environment(), rig.copier());
        }
        main->show();
        pad->present();
        const std::string note = "note " + std::to_string(attempt);
        pad->editor()->setPlainText(QString::fromStdString(note));
        QApplication::processEvents();

        QTimer::singleShot(0, qApp, [] { QApplication::quit(); });
        QTimer watchdog;
        watchdog.setSingleShot(true);
        QObject::connect(&watchdog, &QTimer::timeout, [] { QApplication::exit(7); });
        watchdog.start(1000);
        const int code = QApplication::exec();
        watchdog.stop();

        outcomes.emplace_back(code, main->isVisible(), pad->isVisible(), rig.saved() == note);
    }
    CHECK(outcomes == std::vector<std::tuple<int, bool, bool, bool>>(
                          12, std::make_tuple(0, false, false, true)));
}
