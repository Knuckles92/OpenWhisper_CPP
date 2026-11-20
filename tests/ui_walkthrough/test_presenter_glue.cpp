// The UI controller's walkthrough glue (tests/test_walkthrough_buttons.py, "the UI controller
// passes it all on" and "the column stands in for the overlay"; ui_walkthrough/
// walkthrough_presenter.h). Python's tests call UIController's methods on a SimpleNamespace of
// mocks; here the presenter runs over a real offscreen overlay and column and plain functions.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <QWidget>

#include "panel_support.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_walkthrough/walkthrough_presenter.h"

using fv::WalkthroughPresenter;

namespace {

// What the parts were asked, in order.
struct Recorder {
    std::vector<std::string> calls;
    std::vector<QWidget*> keptOut;
    bool visible = true;
    bool ticked = false;
    bool writing = false;
    QList<QWidget*> drawing;
};

struct Glue {
    Glue() {
        parts.overlay = &overlay;
        parts.panel = &column.panel;
        parts.setTrayActive = [this](bool on) {
            seen.calls.push_back(std::string("tray:") + (on ? "1" : "0"));
        };
        parts.setWindowActive = [this](bool on) {
            seen.calls.push_back(std::string("window:") + (on ? "1" : "0"));
        };
        parts.windowVisible = [this] {
            return seen.visible;
        };
        parts.minimizeToTray = [this] {
            seen.calls.emplace_back("minimize");
        };
        parts.minimizeTicked = [this] {
            return seen.ticked;
        };
        parts.writingUp = [this] {
            return seen.writing;
        };
        parts.keepOutOfScreenshots = [this](QWidget* window, bool on) {
            if (on) {
                seen.keptOut.push_back(window);
            }
            return true;
        };
        parts.drawingWindows = [this] {
            return seen.drawing;
        };
        parts.onWalkthrough = [this](const std::string& source) {
            seen.calls.push_back("walkthrough:" + source);
        };
        parts.onScreenshot = [this](const std::string& source) {
            seen.calls.push_back("screenshot:" + source);
        };
    }
    ~Glue() {
        overlay.setStandIn({});
        overlay.hide();
    }
    Glue(const Glue&) = delete;
    Glue& operator=(const Glue&) = delete;

    WalkthroughPresenter& presenter() {
        if (!made) {
            made = std::make_unique<WalkthroughPresenter>(parts);
        }
        return *made;
    }

    fvtest::PanelFixture column;
    fv::WaveformOverlay overlay;
    Recorder seen;
    WalkthroughPresenter::Parts parts;
    std::unique_ptr<WalkthroughPresenter> made;
};

} // namespace

// from tests/test_walkthrough_buttons.py::test_the_controller_shows_the_walkthrough_everywhere
TEST_CASE("the presenter shows the walkthrough everywhere", "[widget]") {
    Glue glue;

    glue.presenter().setWalkthroughActive(true);

    CHECK(glue.seen.calls == std::vector<std::string>{"tray:1", "window:1"});
    CHECK(glue.overlay.walkthroughControls());
    CHECK(glue.column.panel.showsRecording());
    // Kept out of screenshots once shown, where the native window exists.
    CHECK(glue.seen.keptOut == std::vector<QWidget*>{&glue.column.panel});

    glue.presenter().setWalkthroughActive(false);
    CHECK(glue.seen.calls == std::vector<std::string>{"tray:1", "window:1", "tray:0", "window:0"});
    CHECK_FALSE(glue.overlay.walkthroughControls());
}

// from
// tests/test_walkthrough_buttons.py::test_a_walkthrough_starting_minimizes_the_window_to_the_tray_when_ticked
TEST_CASE("a walkthrough starting minimizes the window to the tray when ticked", "[widget]") {
    struct Row {
        bool ticked, visible, active, minimized;
    };
    const Row row = GENERATE(Row{true, true, true, true}, Row{false, true, true, false},
                             // Already in the tray: nothing to minimize.
                             Row{true, false, true, false},
                             // Finishing leaves the window where the user put it.
                             Row{true, true, false, false});
    Glue glue;
    glue.seen.ticked = row.ticked;
    glue.seen.visible = row.visible;

    glue.presenter().setWalkthroughActive(row.active);

    const bool minimized = std::find(glue.seen.calls.begin(), glue.seen.calls.end(), "minimize") !=
                           glue.seen.calls.end();
    CHECK(minimized == row.minimized);
}

// from tests/test_walkthrough_buttons.py::test_snaps_keep_the_walkthrough_column_out_while_it_shows
TEST_CASE("snaps keep the walkthrough column out while it shows", "[widget]") {
    Glue glue;
    QWidget drawn;
    glue.seen.drawing = {&drawn};
    glue.presenter().showPanel(true);
    REQUIRE(glue.column.panel.isVisible());
    REQUIRE(glue.overlay.isWindow());

    const auto hooks = glue.presenter().snapHooks();
    const QList<QPointer<QWidget>> keepOut = hooks.keepOut();

    REQUIRE(keepOut.size() == 3);
    CHECK(keepOut[0] == &glue.overlay);
    CHECK(keepOut[1] == &glue.column.panel);
    CHECK(keepOut[2] == &drawn);
    // The overlay stays in sight, parked above the snap.
    const auto floating = hooks.floatAbove();
    REQUIRE(floating.size() == 1);
    CHECK(floating[0].window == &glue.overlay);

    // A hidden column is not in the shot to begin with.
    glue.column.panel.hide();
    glue.seen.drawing.clear();
    CHECK(glue.presenter().snapKeepOut() == QList<QPointer<QWidget>>{&glue.overlay});
}

// from
// tests/test_walkthrough_buttons.py::test_snaps_keep_the_overlay_out_only_when_it_is_its_own_window
// (the embedded case, kept here as the column test's counterpart)
TEST_CASE("an overlay inside another window is neither kept out nor parked", "[widget]") {
    QWidget host;
    fv::WaveformOverlay embedded(&host, fv::WaveformOverlay::Mode::Preview);
    WalkthroughPresenter::Parts parts;
    parts.overlay = &embedded;
    WalkthroughPresenter presenter(parts);

    CHECK(presenter.snapKeepOut().isEmpty());
    CHECK(presenter.snapFloatAbove().empty());
}

// from tests/test_walkthrough_buttons.py::test_buttons_reach_the_walkthrough_as_the_user
TEST_CASE("buttons reach the walkthrough as the user", "[widget]") {
    Glue glue;

    glue.presenter().onWindowWalkthrough();
    glue.presenter().onOverlaySnap();
    glue.presenter().onOverlayFinish();

    CHECK(glue.seen.calls ==
          std::vector<std::string>{"walkthrough:ui", "screenshot:ui", "walkthrough:ui"});
    glue.presenter().onTrayWalkthrough();
    glue.presenter().onTrayScreenshot();
    CHECK(glue.seen.calls.back() == "screenshot:tray");
}

// from
// tests/test_walkthrough_buttons.py::test_the_controller_lets_the_column_stand_in_for_the_overlay
TEST_CASE("the presenter lets the column stand in for the overlay", "[widget]") {
    Glue glue;

    glue.presenter().showPanel(true);

    REQUIRE(glue.column.panel.showsRecording());
    CHECK(glue.presenter().panelStandsIn());
    // The overlay asks the presenter: a recording shown now is the column's.
    glue.overlay.showAtCursor(fv::WaveformOverlay::kStateRecording);
    CHECK_FALSE(glue.overlay.isVisible());
    // During the AI write-up the walkthrough's job is over: a dictation made then gets the
    // overlay, and cancelling it leaves the column and its Cancel.
    glue.seen.writing = true;
    CHECK_FALSE(glue.presenter().panelStandsIn());
    glue.presenter().dismissPanelForJob();
    CHECK(glue.column.panel.showsRecording());
    glue.seen.writing = false;
    glue.presenter().dismissPanelForJob();
    CHECK_FALSE(glue.column.panel.showsRecording());
}

TEST_CASE("without a column the overlay keeps the recording", "[widget]") {
    Glue glue;
    glue.parts.panel = nullptr;

    glue.presenter().setWalkthroughActive(true);
    glue.presenter().dismissPanelForJob();

    CHECK_FALSE(glue.presenter().panelStandsIn());
    CHECK(glue.seen.keptOut.empty());
    glue.overlay.showAtCursor(fv::WaveformOverlay::kStateRecording);
    CHECK(glue.overlay.isVisible());
}
