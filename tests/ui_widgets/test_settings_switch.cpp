// SettingsSwitch and AnimatedProgressBar: the two widgets that paint with the typed theme.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <tuple>

#include <QApplication>
#include <QImage>
#include <QSignalSpy>
#include <QTest>

#include "app_theme.h"
#include "ui_theme/theme.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/settings_switch.h"

namespace {

QImage grabOf(QWidget& widget) {
    widget.show();
    QApplication::processEvents();
    return widget.grab().toImage();
}

} // namespace

TEST_CASE("SettingsSwitch size hint follows the font scale", "[widget]") {
    const auto [percent, width, height] =
        GENERATE(std::tuple{90, 41, 25}, std::tuple{100, 46, 28}, std::tuple{115, 53, 32},
                 std::tuple{130, 60, 36});
    const fvtest::AppTheme theme(QStringLiteral("dark"), percent);
    fv::SettingsSwitch toggle;
    CHECK(toggle.sizeHint() == QSize(width, height));
    CHECK(toggle.sizePolicy().horizontalPolicy() == QSizePolicy::Fixed);
    CHECK(toggle.sizePolicy().verticalPolicy() == QSizePolicy::Fixed);
}

TEST_CASE("a switch is a checkable button with the theme's name and a hand cursor", "[widget]") {
    fv::SettingsSwitch toggle;
    CHECK(toggle.objectName() == QStringLiteral("settingsSwitch"));
    CHECK(toggle.isCheckable());
    CHECK(toggle.cursor().shape() == Qt::PointingHandCursor);
    toggle.show();
    QSignalSpy toggled(&toggle, &QAbstractButton::toggled);
    QTest::mouseClick(&toggle, Qt::LeftButton);
    CHECK(toggle.isChecked());
    REQUIRE(toggled.count() == 1);
    CHECK(toggled.takeFirst().at(0).toBool());
}

TEST_CASE("the size hint is recomputed when the scale changes", "[widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"), 100);
    fv::SettingsSwitch toggle;
    CHECK(toggle.sizeHint() == QSize(46, 28));
    fv::ui::applyUiFontScale(130, *qApp, theme.manager());
    CHECK(toggle.sizeHint() == QSize(60, 36));
}

TEST_CASE("an on switch draws the accent track, an off one does not", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    const QColor accent = fv::ui::currentTheme().accent;

    fv::SettingsSwitch on;
    on.setChecked(true);
    on.resize(on.sizeHint());
    const QImage onImage = grabOf(on);
    // The track's left end, clear of the knob (which sits at the right when on).
    CHECK(onImage.pixelColor(7, onImage.height() / 2) == accent);

    fv::SettingsSwitch off;
    off.resize(off.sizeHint());
    const QImage offImage = grabOf(off);
    CHECK(offImage.pixelColor(offImage.width() - 8, offImage.height() / 2) != accent);

    // Disabled, an on switch keeps its knob position but never shows the accent track.
    fv::SettingsSwitch disabled;
    disabled.setChecked(true);
    disabled.setEnabled(false);
    disabled.resize(disabled.sizeHint());
    const QImage disabledImage = grabOf(disabled);
    CHECK(disabledImage.pixelColor(7, disabledImage.height() / 2) != accent);
}

TEST_CASE("AnimatedProgressBar starts indeterminate, empty, and 8 px tall", "[widget]") {
    fv::AnimatedProgressBar bar;
    CHECK(bar.objectName() == QStringLiteral("animatedProgressBar"));
    CHECK(bar.isIndeterminate());
    CHECK(bar.fraction() == 0.0);
    CHECK(bar.height() == 8);
    CHECK(bar.minimumHeight() == 8);
    CHECK(bar.maximumHeight() == 8);
    CHECK(bar.testAttribute(Qt::WA_TransparentForMouseEvents));
    CHECK(fv::AnimatedProgressBar(nullptr, 12).maximumHeight() == 12);
}

TEST_CASE("AnimatedProgressBar clamps fractions and leaves the sweep for a value", "[widget]") {
    fv::AnimatedProgressBar bar;
    bar.setFraction(1.7, false);
    CHECK_FALSE(bar.isIndeterminate());
    CHECK(bar.fraction() == 1.0);
    bar.setFraction(-0.3, false);
    CHECK(bar.fraction() == 0.0);
    bar.setFraction(0.4);
    CHECK(bar.fraction() == 0.4);
    bar.setIndeterminate();
    CHECK(bar.isIndeterminate());
    bar.reset();
    CHECK_FALSE(bar.isIndeterminate());
    CHECK(bar.fraction() == 0.0);
}

TEST_CASE("AnimatedProgressBar ticks only while shown and eases 18 percent per tick", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::AnimatedProgressBar bar;
    bar.resize(300, 8);
    bar.setFraction(1.0);

    // Hidden: no timer, so nothing moves however long it waits.
    QTest::qWait(60);
    bar.show();
    QApplication::processEvents();
    const QImage first = bar.grab().toImage();
    // Eased from empty: after a few ticks the fill has grown but is short of the target.
    QTest::qWait(120);
    const QImage later = bar.grab().toImage();
    auto filled = [](const QImage& image) {
        int count = 0;
        for (int x = 0; x < image.width(); ++x) {
            if (image.pixelColor(x, 4) != image.pixelColor(0, 4)) {
                ++count;
            }
        }
        return count;
    };
    CHECK(filled(later) > filled(first));

    bar.hide();
    const QImage hiddenAt = bar.grab().toImage();
    QTest::qWait(80);
    CHECK(bar.grab().toImage() == hiddenAt);
}

TEST_CASE("a determinate bar paints its fill in the accent gradient", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::AnimatedProgressBar bar;
    bar.resize(300, 8);
    bar.setFraction(1.0, false);
    const QImage image = grabOf(bar);
    // A full bar ends in the second gradient colour, an empty one is the faint track.
    const fv::ui::Theme& colours = fv::ui::currentTheme();
    const QColor end = image.pixelColor(image.width() - 8, 4);
    CHECK(std::abs(end.blue() - colours.accentCyan.blue()) < 40);

    fv::AnimatedProgressBar empty;
    empty.resize(300, 8);
    empty.setFraction(0.0, false);
    const QImage track = grabOf(empty);
    CHECK(track.pixelColor(150, 4) != colours.accent);
}
