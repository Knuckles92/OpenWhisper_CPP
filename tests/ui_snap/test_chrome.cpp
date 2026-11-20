// The overlay chrome's sizes and the toolbar glyphs (chrome.py, icons.py); their pixels are
// compared with OpenWhisper's in test_reference_screenshots.cpp.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>

#include <QApplication>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QImage>
#include <QPainter>

#include "ui_snap/chrome.h"
#include "ui_snap/icons.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/palette.h"

using namespace fv::ui_snap;

namespace {

class FontScale {
public:
    explicit FontScale(int percent) { fv::ui::applyUiFontScale(percent, *qApp); }
    ~FontScale() { fv::ui::applyUiFontScale(100, *qApp); }
    FontScale(const FontScale&) = delete;
    FontScale& operator=(const FontScale&) = delete;
};

QImage glyph(std::string_view name, int size = 20) {
    QImage image(size, size, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    paintSnapIcon(painter, name, QRectF(0, 0, size, size), Qt::white);
    return image;
}

int inked(const QImage& image) {
    int count = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            count += qAlpha(image.pixel(x, y)) != 0 ? 1 : 0;
        }
    }
    return count;
}

} // namespace

TEST_CASE("chrome type follows the UI text size, half to even", "[snap_chrome]") {
    const QFont base = QGuiApplication::font();
    CHECK(chromeFont(base, 12.5).pixelSize() == 12);
    CHECK(chromeFont(base, 11.5, QFont::DemiBold).pixelSize() == 12);
    CHECK(chromeFont(base, 11.5, QFont::DemiBold).weight() == QFont::DemiBold);
    CHECK(chromeFont(base, 3).pixelSize() == 8);
    FontScale larger(130);
    CHECK(chromeFont(base, 12.5).pixelSize() == 16);
    CHECK(chromeFont(base, 12).pixelSize() == 16);
}

TEST_CASE("the size readout pads its text", "[snap_chrome]") {
    const QFont font = chromeFont(QGuiApplication::font(), 12);
    const QFontMetrics metrics(font);
    const QString text = QStringLiteral(u"1280 × 720");
    CHECK(badgeSize(text, font) ==
          QSize(metrics.horizontalAdvance(text) + 18, metrics.height() + 8));
}

TEST_CASE("the hint pill fits its pieces", "[snap_chrome]") {
    const QFont base = QGuiApplication::font();
    const QFontMetrics text(chromeFont(base, 12.5));
    const QFontMetrics keys(chromeFont(base, 11.5, QFont::DemiBold));
    const int keyWidths = keys.horizontalAdvance(QStringLiteral("Enter")) +
                          keys.horizontalAdvance(QStringLiteral("Esc")) + 4 * HintPill::kKeyPad;
    const int words = text.horizontalAdvance(QStringLiteral("to keep")) +
                      text.horizontalAdvance(QStringLiteral(u"·")) +
                      text.horizontalAdvance(QStringLiteral("to cancel"));
    const int height = std::max(text.height(), keys.height() + 4) + 12;

    const HintPill bare(QString(), base, fv::ui::darkPalette());
    CHECK(bare.size() ==
          QSize(keyWidths + words + 4 * HintPill::kGap + 2 * HintPill::kPadX, height));

    const HintPill hinted(QStringLiteral("  Drag to select  "), base, fv::ui::darkPalette());
    const int hint = text.horizontalAdvance(QStringLiteral("Drag to select")) +
                     text.horizontalAdvance(QStringLiteral(u"·"));
    CHECK(hinted.size().width() == bare.size().width() + hint + 2 * HintPill::kGap);
}

TEST_CASE("a long hint is cut short so the keys fit", "[snap_chrome]") {
    const QFont base = QGuiApplication::font();
    const HintPill bare(QString(), base, fv::ui::darkPalette());
    const QString hint(
        QStringLiteral("Select the part of the screen you want to keep ").repeated(4));

    const HintPill unlimited(hint, base, fv::ui::darkPalette());
    const HintPill cut(hint, base, fv::ui::darkPalette(), bare.size().width() + 200);
    const HintPill tooNarrow(hint, base, fv::ui::darkPalette(), bare.size().width() + 30);

    CHECK(unlimited.size().width() > bare.size().width() + 200);
    CHECK(cut.size().width() <= bare.size().width() + 200);
    CHECK(cut.size().width() > bare.size().width());
    CHECK(tooNarrow.size() == bare.size());
}

TEST_CASE("every glyph draws, and region is the snap glyph", "[snap_icons]") {
    CHECK(snapIconNames().size() == 15);
    for (const std::string_view name : snapIconNames()) {
        CAPTURE(std::string(name));
        CHECK(inked(glyph(name)) > 0);
    }
    CHECK(glyph("region") == glyph("snap"));
    CHECK(glyph("undo") != glyph("redo"));
}

TEST_CASE("an unknown glyph is an error", "[snap_icons]") {
    QImage image(20, 20, QImage::Format_ARGB32_Premultiplied);
    QPainter painter(&image);
    CHECK_THROWS_AS(paintSnapIcon(painter, "lasso", QRectF(0, 0, 20, 20), Qt::white),
                    std::out_of_range);
}
