// The capture delay chip and its picker (tests/test_capture_delay.py), and the chip's painted
// text, countdown arc, arrow-key steps and setting fallback against OpenWhisper
// (tests/golden/ui_qt.widgets.capture_delay.json). No test reaches the desktop: capture
// exclusion goes through the fake Win32 desktop of ScopedFakeDesktop.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>

#include <QApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPoint>
#include <QTest>
#include <QWidget>

#include "base/json.h"
#include "golden/golden.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "ui_snap/affinity.h"
#include "ui_snap/capture_delay.h"
#include "ui_snap/testing/fake_desktop.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/palette.h"

#include "overlay_test_support.h"

using namespace fv::ui_snap;
using namespace fvtest;
using fv::Json;
using fv::ui_snap::testing::FakeWin32Desktop;
using fv::ui_snap::testing::ScopedFakeDesktop;

namespace {

std::int64_t savedDelay(const fv::MemorySettingsStore& settings) {
    return settings.get(fv::keys::walkthrough_capture_delay);
}

Json fallback(const Json& raw) {
    return fv::keys::walkthrough_capture_delay.read(
        Json{{std::string(fv::keys::walkthrough_capture_delay.key()), raw}});
}

int keyNamed(const std::string& name) {
    if (name == "Left") {
        return Qt::Key_Left;
    }
    if (name == "Right") {
        return Qt::Key_Right;
    }
    if (name == "Up") {
        return Qt::Key_Up;
    }
    return Qt::Key_Down;
}

} // namespace

// from tests/test_capture_delay.py::test_invalid_delay_settings_fall_back_to_instant
TEST_CASE("invalid delay settings fall back to instant", "[test_capture_delay]") {
    const auto [raw, expected] = GENERATE(table<Json, int>({
        {Json(nullptr), 0},
        {Json(true), 0},
        {Json("3"), 0},
        {Json(-1), 0},
        {Json(30), 0},
        {Json(0), 0},
        {Json(3), 3},
        {Json(10), 10},
    }));
    CAPTURE(raw.dump());
    CHECK(fallback(raw) == expected);
}

// from tests/test_capture_delay.py::
// test_live_picker_saves_delay_without_taking_focus_and_cancels_pending_capture
TEST_CASE("live picker saves delay without taking focus and cancels pending capture",
          "[test_capture_delay]") {
    ScopedFakeDesktop desktop;
    fv::MemorySettingsStore settings;
    SnapToolbar toolbar(nullptr, fv::ui::currentPalette(), SnapToolbar::Variant::Live);
    auto* chip = new CaptureDelay(&toolbar, &settings);
    toolbar.setCaptureDelay(chip);
    toolbar.show();
    Spy<int> chosen(chip, &CaptureDelay::changed);
    Spy<> cancelled(chip, &CaptureDelay::cancelled);

    CHECK(chip->focusPolicy() == Qt::NoFocus);
    click(chip, center(chip->rect()));
    DelayPicker* picker = chip->picker();
    REQUIRE(picker != nullptr);
    CHECK(picker->isVisible());
    CHECK(picker->windowFlags().testFlag(Qt::WindowDoesNotAcceptFocus));
    CHECK_FALSE(picker->grab().isNull()); // Exercise all the painted preset states.
    REQUIRE_FALSE(desktop.win32.affinityCalls.empty());
    CHECK(desktop.win32.affinityCalls.back() ==
          FakeWin32Desktop::AffinityCall{picker->winId(), kWdaExcludeFromCapture});
    CHECK(isKeptOut(picker));

    click(picker, DelayPicker::option(4).center());
    REQUIRE(chosen.size() == 1);
    CHECK(std::get<0>(chosen.calls[0]) == 5);
    CHECK(savedDelay(settings) == 5);
    CHECK(chip->seconds() == 5);
    CHECK(chip->picker() == nullptr);

    chip->setCountdown(2.4, 5);
    CHECK_FALSE(chip->grab().isNull());
    click(chip, QPointF(chip->width() - 14, 17));
    CHECK(cancelled.size() == 1);
    CHECK(chosen.size() == 1);
    toolbar.close();
}

// from tests/test_capture_delay.py::test_tab_delay_can_be_changed_with_arrow_keys
TEST_CASE("tab delay can be changed with arrow keys", "[test_capture_delay]") {
    fv::MemorySettingsStore settings;
    CaptureDelay chip(nullptr, &settings);
    CHECK(chip.focusPolicy() == Qt::StrongFocus);
    chip.setSeconds(0);
    QTest::keyClick(&chip, Qt::Key_Right);
    CHECK(chip.seconds() == 1);
    QTest::keyClick(&chip, Qt::Key_Left);
    CHECK(chip.seconds() == 0);
    CHECK(savedDelay(settings) == 0);
    chip.close();
}

// from tests/test_capture_delay.py::test_toolbar_wraps_delay_and_controls_on_small_screens
TEST_CASE("toolbar wraps delay and controls on small screens", "[test_capture_delay]") {
    SnapToolbar toolbar(nullptr, fv::ui::currentPalette(), SnapToolbar::Variant::Live);
    auto* chip = new CaptureDelay(&toolbar);
    toolbar.setCaptureDelay(chip);
    toolbar.fitToWidth(780);
    CHECK(toolbar.width() <= 780);
    // Font metrics decide which group wraps; the trailing controls must move to another row
    // even when the delay chip still fits beside the tools.
    CHECK(toolbar.buttonRect(QStringLiteral("drop")).top() >
          toolbar.buttonRect(QStringLiteral("tool")).top());
    CHECK(chip->geometry() == toolbar.buttonRect(QStringLiteral("delay")));
    toolbar.fitToWidth(1400);
    CHECK(toolbar.buttonRect(QStringLiteral("delay")).top() ==
          toolbar.buttonRect(QStringLiteral("tool")).top());
    toolbar.close();
}

// ---- golden vectors ----

TEST_CASE("the chip's text and countdown arc match OpenWhisper", "[golden][capture_delay]") {
    fv::golden::for_each("ui_qt.widgets.capture_delay", "paint", [](const fv::golden::Case& c) {
        const int seconds = c.arg<int>(0);
        const double remaining = std::max(0.0, c.arg<double>(1));
        const double total = c.arg<double>(2);
        const Json expected = c.result<Json>();
        CHECK(captureDelayText(seconds, remaining).toStdString() ==
              expected["text"].get<std::string>());
        if (remaining > 0) {
            CHECK(Json::array({90 * 16, captureDelayArc(remaining, total)}) == expected["arc"]);
        } else {
            CHECK(expected["arc"].is_null());
        }
    });
}

TEST_CASE("the chip's arrow keys step through the presets as OpenWhisper's",
          "[golden][capture_delay]") {
    fv::golden::for_each("ui_qt.widgets.capture_delay", "arrow", [](const fv::golden::Case& c) {
        const int key = keyNamed(c.arg<std::string>(1));
        CHECK(steppedCaptureDelay(c.arg<int>(0), key) == c.result<int>());

        CaptureDelay chip;
        chip.setSeconds(c.arg<int>(0));
        QKeyEvent press(QEvent::KeyPress, key, Qt::NoModifier);
        QApplication::sendEvent(&chip, &press);
        CHECK(chip.seconds() == c.result<int>());
    });
}

TEST_CASE("walkthrough_capture_delay falls back as OpenWhisper's", "[golden][capture_delay]") {
    fv::golden::for_each(
        "ui_qt.widgets.capture_delay", "resolve_walkthrough_capture_delay",
        [](const fv::golden::Case& c) { CHECK(fallback(c.arg<Json>(0)) == c.result<Json>()); });
}

TEST_CASE("an arrow from a delay that is not a preset is refused", "[capture_delay]") {
    CHECK_THROWS_AS(steppedCaptureDelay(4, Qt::Key_Right), std::out_of_range);
    // The chip's own arrow key never lets that escape its event handler: the key does nothing.
    fv::MemorySettingsStore settings;
    settings.save_setting(fv::keys::walkthrough_capture_delay.key(), 3);
    CaptureDelay chip(nullptr, &settings);
    chip.setSeconds(4);
    Spy<int> chosen(&chip, &CaptureDelay::changed);
    QKeyEvent press(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
    CHECK_NOTHROW(QApplication::sendEvent(&chip, &press));
    CHECK(chip.seconds() == 4);
    CHECK(chosen.size() == 0);
    CHECK(savedDelay(settings) == 3);
}

TEST_CASE("the chip names its delay for accessibility", "[capture_delay]") {
    CaptureDelay chip;
    CHECK(chip.accessibleName() == QStringLiteral("Capture delay: instant"));
    chip.setSeconds(3);
    CHECK(chip.accessibleName() == QStringLiteral("Capture delay: 3 seconds"));
    CHECK(captureDelayText(3, 0) == QStringLiteral(u"Delay · 3s"));
    CHECK(captureDelayText(0, 0) == QStringLiteral(u"Delay · Instant"));
    CHECK(captureDelayText(3, 2.4) == QStringLiteral("Snap in 2.4s"));
}
