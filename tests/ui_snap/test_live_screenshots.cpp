// The live toolbar and the capture delay chip and picker against OpenWhisper's renders
// (tools/parity/builders/a22.py: snap_live_toolbar and capture_delay; tests/reference/
// snap_live_toolbar/ and capture_delay/). Each case builds what the builder builds.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <string_view>

#include <QApplication>
#include <QColor>
#include <QPaintEvent>
#include <QPainter>
#include <QWidget>

#include "screenshot/screenshot.h"
#include "ui_snap/annotations.h"
#include "ui_snap/capture_delay.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {
namespace {

using namespace fv::ui_snap;

// The builder's LIVE_MARGIN, LIVE_DELAY, COUNTDOWN and PICKER_GAP.
constexpr int kMargin = 12;
constexpr int kDelay = 3;
constexpr double kRemaining = 1.2;
constexpr double kTotal = 3.0;
constexpr int kPickerGap = 7;

// The application styled the way OpenWhisper's QtApplication styles it, for one test case.
class AppTheme {
public:
    AppTheme(const QString& theme, int scalePercent) : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

private:
    fv::ui::ThemeManager manager_;
};

void paintGrey(QWidget* widget) {
    QPainter painter(widget);
    painter.fillRect(widget->rect(), QColor(96, 96, 96));
}

} // namespace

// The builder's _grey_canvas("LiveToolbarCanvas") and _grey_canvas("CaptureDelayCanvas").
class LiveToolbarCanvas : public QWidget {
    Q_OBJECT
protected:
    void paintEvent(QPaintEvent*) override { paintGrey(this); }
};

class CaptureDelayCanvas : public QWidget {
    Q_OBJECT
protected:
    void paintEvent(QPaintEvent*) override { paintGrey(this); }
};

namespace {

void expectLiveToolbar(const QString& theme, std::string_view variant) {
    AppTheme applied(theme, 100);
    LiveToolbarCanvas canvas;
    auto* toolbar = new SnapToolbar(&canvas, fv::ui::currentPalette(), SnapToolbar::Variant::Live);
    auto* chip = new CaptureDelay(toolbar);
    toolbar->setCaptureDelay(chip);
    chip->setSeconds(kDelay);
    toolbar->setState(SnapTool::Pen, 0, false, false);
    toolbar->setSnapEnabled(true);
    toolbar->setBoxInfill(false);
    toolbar->setKeepMarks(true);
    toolbar->setDropEnabled(true);
    toolbar->move(kMargin, kMargin);
    fv::screenshot::expect_matches(canvas, "snap_live_toolbar", variant);
}

void expectCaptureDelay(const QString& theme, std::string_view variant) {
    AppTheme applied(theme, 100);
    CaptureDelayCanvas canvas;
    auto* idle = new CaptureDelay(&canvas);
    idle->setSeconds(kDelay);
    auto* counting = new CaptureDelay(&canvas);
    counting->setSeconds(kDelay);
    counting->setCountdown(kRemaining, kTotal);
    idle->move(kMargin, kMargin);
    counting->move(2 * kMargin + idle->width(), kMargin);
    auto* picker = new DelayPicker(idle);
    // As the builder: a child of the canvas at the place open() would put it.
    picker->setParent(&canvas, picker->windowFlags() & ~Qt::WindowType_Mask);
    picker->move(kMargin, kMargin + idle->height() + kPickerGap);
    picker->show();
    // The picker's class is _DelayPicker in Python, a name C++ reserves; the rest of the tree is
    // compared, and the pixels.
    fv::screenshot::expect_matches(canvas, "capture_delay", variant, {.ignore_keys = {"class"}});
}

} // namespace

TEST_CASE("live toolbar matches the reference, dark", "[screenshot][snap_live_toolbar]") {
    expectLiveToolbar(QStringLiteral("dark"), "dark-windows11-dpr1-s100");
}

TEST_CASE("live toolbar matches the reference, light", "[screenshot][snap_live_toolbar]") {
    expectLiveToolbar(QStringLiteral("light"), "light-windows11-dpr1-s100");
}

TEST_CASE("capture delay chip and picker match the reference, dark",
          "[screenshot][capture_delay]") {
    expectCaptureDelay(QStringLiteral("dark"), "dark-windows11-dpr1-s100");
}

TEST_CASE("capture delay chip and picker match the reference, light",
          "[screenshot][capture_delay]") {
    expectCaptureDelay(QStringLiteral("light"), "light-windows11-dpr1-s100");
}

} // namespace fvtest

#include "test_live_screenshots.moc"
