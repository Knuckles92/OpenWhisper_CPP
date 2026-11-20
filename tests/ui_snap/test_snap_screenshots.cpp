// The Snap toolbar and overlay against OpenWhisper's renders (tools/parity/builders/a22.py:
// snap_toolbar and snap_overlay; tests/reference/snap_toolbar/ and snap_overlay/). Each case
// builds what the builder builds and drives it with the same events.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QImage>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QTest>
#include <QWidget>

#include "screenshot/screenshot.h"
#include "ui_snap/annotations.h"
#include "ui_snap/overlay.h"
#include "ui_snap/toolbar.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {
namespace {

using namespace fv::ui_snap;

// The builder's TOOLBAR_MARGIN, SNAP_IMAGE and OVERLAY_HINT.
constexpr int kMargin = 12;
constexpr QSize kSnapImage(960, 600);
constexpr char16_t kHint[] = u"S toggles snapping";

// The builder's _frozen_screen: a fixed gradient with two flat "windows" on it.
QImage frozenScreenImage() {
    QImage image(kSnapImage, QImage::Format_RGB32);
    QPainter painter(&image);
    QLinearGradient gradient(0, 0, kSnapImage.width(), kSnapImage.height());
    gradient.setColorAt(0.0, QColor(QStringLiteral("#2d5b8a")));
    gradient.setColorAt(0.55, QColor(QStringLiteral("#d9c7a8")));
    gradient.setColorAt(1.0, QColor(QStringLiteral("#3c5a3c")));
    painter.fillRect(image.rect(), gradient);
    painter.fillRect(QRect(80, 60, 360, 240), QColor(QStringLiteral("#f4efe9")));
    painter.fillRect(QRect(520, 300, 320, 200), QColor(QStringLiteral("#1f2933")));
    return image;
}

// The builder's _mouse: local and global positions alike.
void send(QWidget* widget, QEvent::Type kind, QPointF at, bool held) {
    const Qt::MouseButton button = kind == QEvent::MouseMove ? Qt::NoButton : Qt::LeftButton;
    QMouseEvent event(kind, at, at, button, held ? Qt::LeftButton : Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(widget, &event);
}

void dragThrough(QWidget* widget, const std::vector<QPointF>& points) {
    send(widget, QEvent::MouseButtonPress, points.front(), true);
    for (std::size_t index = 1; index < points.size(); ++index) {
        send(widget, QEvent::MouseMove, points[index], true);
    }
    send(widget, QEvent::MouseButtonRelease, points.back(), false);
}

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

} // namespace

// The builder's ToolbarCanvas.
class ToolbarCanvas : public QWidget {
    Q_OBJECT
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(96, 96, 96));
    }
};

namespace {

void expectToolbars(const QString& theme, std::string_view variant) {
    AppTheme applied(theme, 100);
    ToolbarCanvas canvas;
    auto* overlay = new SnapToolbar(&canvas, fv::ui::currentPalette());
    overlay->setState(SnapTool::Arrow, 0, true, false);
    overlay->setBoxInfill(false);
    auto* live = new SnapToolbar(&canvas, fv::ui::currentPalette(), SnapToolbar::Variant::Live);
    live->setState(std::nullopt, 1, false, false);
    live->setSnapEnabled(true);
    live->setKeepMarks(true);
    live->setDropEnabled(false);
    overlay->move(kMargin, kMargin);
    live->move(kMargin, 2 * kMargin + overlay->height());
    fv::screenshot::expect_matches(canvas, "snap_toolbar", variant);
}

void expectOverlays(const QString& theme, std::string_view variant) {
    AppTheme applied(theme, 100);
    QWidget canvas;
    const SnapOverlay::Options options{.hint = QString::fromUtf16(kHint)};
    auto* selecting = new SnapOverlay(frozenScreenImage(), nullptr, options);
    auto* annotating = new SnapOverlay(frozenScreenImage(), nullptr, options);
    int index = 0;
    for (SnapOverlay* overlay : {selecting, annotating}) {
        overlay->setParent(&canvas);
        overlay->move(0, index++ * kSnapImage.height());
        overlay->show();
    }

    send(selecting, QEvent::MouseButtonPress, QPointF(200, 150), true);
    send(selecting, QEvent::MouseMove, QPointF(520, 390), true);

    dragThrough(annotating, {{120, 80}, {840, 520}});
    const std::vector<std::tuple<SnapTool, int, std::vector<QPointF>>> marks{
        {SnapTool::Arrow, 0, {{160, 120}, {230, 160}, {300, 200}}},
        {SnapTool::Box, 1, {{340, 110}, {460, 190}}},
        {SnapTool::Ellipse, 2, {{500, 110}, {640, 190}}},
        {SnapTool::Pen, 3, {{160, 260}, {200, 300}, {240, 250}, {280, 310}, {300, 280}}},
        {SnapTool::Highlighter, 1, {{340, 280}, {410, 281}, {480, 282}}},
    };
    for (const auto& [tool, color, points] : marks) {
        annotating->setTool(tool);
        annotating->setColor(color);
        dragThrough(annotating, points);
    }
    annotating->setTool(SnapTool::Text);
    annotating->setColor(0);
    dragThrough(annotating, {{520, 280}});
    REQUIRE(annotating->labelEditor() != nullptr);
    QTest::keyClicks(annotating->labelEditor(), QStringLiteral("Click here"));
    QTest::keyClick(annotating->labelEditor(), Qt::Key_Return);
    annotating->setTool(SnapTool::Step);
    annotating->setColor(2);
    dragThrough(annotating, {{200, 420}});
    dragThrough(annotating, {{260, 420}});
    annotating->setTool(SnapTool::Redact);
    dragThrough(annotating, {{600, 380}, {720, 460}});
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    REQUIRE(annotating->annotations().size() == 9);

    fv::screenshot::expect_matches(canvas, "snap_overlay", variant);
}

} // namespace

TEST_CASE("snap toolbar matches the reference, dark", "[screenshot][snap_toolbar]") {
    expectToolbars(QStringLiteral("dark"), "dark-windows11-dpr1-s100");
}

TEST_CASE("snap toolbar matches the reference, light", "[screenshot][snap_toolbar]") {
    expectToolbars(QStringLiteral("light"), "light-windows11-dpr1-s100");
}

TEST_CASE("snap overlay matches the reference, dark", "[screenshot][snap_overlay]") {
    expectOverlays(QStringLiteral("dark"), "dark-windows11-dpr1-s100");
}

TEST_CASE("snap overlay matches the reference, light", "[screenshot][snap_overlay]") {
    expectOverlays(QStringLiteral("light"), "light-windows11-dpr1-s100");
}

} // namespace fvtest

#include "test_snap_screenshots.moc"
