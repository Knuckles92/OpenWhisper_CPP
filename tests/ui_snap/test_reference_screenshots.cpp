// The Snap glyphs, swatch rings, chrome and marks against the canvas OpenWhisper painted through
// its own functions (tools/parity/builders/a22.py, tests/reference/snap_glyphs/).
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <string_view>

#include <QApplication>
#include <QColor>
#include <QGuiApplication>
#include <QImage>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QWidget>

#include "screenshot/screenshot.h"
#include "ui_snap/annotations.h"
#include "ui_snap/chrome.h"
#include "ui_snap/icons.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {
namespace {

using namespace fv::ui_snap;

// The builder's GLYPHS, CANVAS, IMAGE, IMAGE_AT, HINT and BADGE.
constexpr std::array<std::string_view, 14> kGlyphs{
    "arrow", "box",  "ellipse", "redact", "pen",  "highlighter", "text",
    "step",  "undo", "redo",    "keep",   "drop", "snap",        "clear"};
constexpr QSize kCanvas(664, 482);
constexpr QSize kImage(480, 320);
constexpr QPoint kImageAt(12, 150);

std::vector<Annotation> marks() {
    const QColor coral(QString::fromLatin1(kSwatches[0].color));
    const QColor amber(QString::fromLatin1(kSwatches[1].color));
    const QColor blue(QString::fromLatin1(kSwatches[2].color));
    const QColor green(QString::fromLatin1(kSwatches[3].color));
    return {
        Annotation::arrow(QPointF(30, 120), QPointF(150, 40), coral),
        Annotation::box(QPointF(180, 30), QPointF(280, 100), amber, kBoxWidth, true),
        Annotation::ellipse(QPointF(300, 30), QPointF(440, 110), blue),
        Annotation::stroke({QPointF(30, 160), QPointF(60, 140), QPointF(90, 180), QPointF(120, 150),
                            QPointF(150, 170)},
                           green),
        Annotation::stroke({QPointF(180, 150), QPointF(300, 155)}, amber, true),
        Annotation::label(QPointF(30, 220), QStringLiteral("Click here first"), coral),
        Annotation::step(QPointF(200, 250), 3, blue),
        Annotation::step(QPointF(250, 250), 1, amber),
        Annotation::redaction(QPointF(300, 200), QPointF(360, 260)),
    };
}

} // namespace

// The builder's SnapGlyphs canvas: a test widget, painted through the ported functions.
class SnapGlyphs : public QWidget {
    Q_OBJECT
protected:
    void paintEvent(QPaintEvent*) override {
        const fv::ui::Palette& palette = fv::ui::currentPalette();
        const QFont font = QGuiApplication::font();
        QPainter painter(this);
        QColor background = palette.color("overlay-bg");
        background.setAlpha(255);
        painter.fillRect(QRect(QPoint(0, 0), kCanvas), background);
        const QColor text = palette.color("overlay-text");
        const QColor accent = palette.color("accent");
        for (int index = 0; index < static_cast<int>(kGlyphs.size()); ++index) {
            paintSnapIcon(painter, kGlyphs[index], QRectF(12 + index * 28, 12, 20, 20), text);
            paintSnapIcon(painter, kGlyphs[index], QRectF(12 + index * 46, 44, 40, 40), accent);
        }
        painter.setRenderHint(QPainter::Antialiasing);
        for (int index = 0; index < static_cast<int>(kSwatches.size()); ++index) {
            const QPointF center(30 + index * 32, 112);
            painter.setPen(QPen(text, 2));
            painter.setBrush(Qt::NoBrush);
            painter.drawEllipse(center, 11, 11);
            painter.setPen(QPen(QColor(0, 0, 0, 60), 1));
            painter.setBrush(QColor(QString::fromLatin1(kSwatches[index].color)));
            painter.drawEllipse(center, 8, 8);
        }
        const HintPill pill(QStringLiteral("Drag to select"), font, palette);
        pill.paint(painter, QRect(QPoint(150, 96), pill.size()));
        const QFont badgeFont = chromeFont(font, 12);
        const QString badgeText = QStringLiteral(u"1280 × 720");
        const QRect badge(QPoint(504, 160), badgeSize(badgeText, badgeFont));
        paintBadge(painter, badge, badgeText, badgeFont, palette);
        QImage image(kImage, QImage::Format_ARGB32_Premultiplied);
        image.fill(QColor(128, 128, 128));
        {
            QPainter onImage(&image);
            onImage.setRenderHint(QPainter::Antialiasing);
            onImage.setRenderHint(QPainter::TextAntialiasing);
            for (const Annotation& mark : marks()) {
                paintAnnotation(onImage, mark);
            }
            paintFrame(onImage, QRect(380, 200, 80, 90));
        }
        painter.drawImage(kImageAt, image);
    }
};

namespace {

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

void expectCanvas(const QString& theme, int scale, std::string_view variant) {
    AppTheme applied(theme, scale);
    SnapGlyphs canvas;
    fv::screenshot::expect_matches(canvas, "snap_glyphs", variant);
}

} // namespace

TEST_CASE("snap glyphs and marks match the reference, dark", "[screenshot][snap_glyphs]") {
    expectCanvas(QStringLiteral("dark"), 100, "dark-windows11-dpr1-s100");
}

TEST_CASE("snap glyphs and marks match the reference, light", "[screenshot][snap_glyphs]") {
    expectCanvas(QStringLiteral("light"), 100, "light-windows11-dpr1-s100");
}

TEST_CASE("snap glyphs and marks match the reference, dark at 130 percent",
          "[screenshot][snap_glyphs]") {
    expectCanvas(QStringLiteral("dark"), 130, "dark-windows11-dpr1-s130");
}

} // namespace fvtest

#include "test_reference_screenshots.moc"
