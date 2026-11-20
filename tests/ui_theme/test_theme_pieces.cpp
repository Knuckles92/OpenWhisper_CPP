// The rest of the theme library: typed colours, url rewriting, the QPalette mapping, style
// properties, tooltips and icons.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QDirIterator>
#include <QEvent>
#include <QFile>
#include <QImage>
#include <QLabel>
#include <QPalette>
#include <QProxyStyle>
#include <QStyle>
#include <QStyleFactory>
#include <QToolTip>

#include <set>
#include <string>

#include "ui_theme/app_icon.h"
#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme.h"
#include "ui_theme/theme_manager.h"
#include "ui_theme/tooltip_style.h"

using namespace fv::ui;

TEST_CASE("Theme has one colour per palette colour token", "[ui_theme]") {
    std::set<std::string> members;
#define FV_COLLECT(member, token) members.insert(token);
    FV_THEME_COLOURS(FV_COLLECT)
#undef FV_COLLECT
    std::set<std::string> tokens;
    for (const auto& [token, value] : darkPalette().tokens()) {
        if (token != "icon-suffix") {
            tokens.insert(token);
        }
    }
    CHECK(members == tokens);

    const Theme dark = Theme::fromPalette(darkPalette());
    CHECK(dark.bg == darkPalette().color("bg"));
    CHECK(dark.accentRgb == QColor(10, 132, 255));
    CHECK(dark.overlayBg.alpha() == darkPalette().color("overlay-bg").alpha());
}

TEST_CASE("currentTheme follows the current palette", "[ui_theme]") {
    setCurrentPalette(lightPalette());
    CHECK(currentTheme().bg == lightPalette().color("bg"));
    setCurrentPalette(darkPalette());
    CHECK(currentTheme().bg == darkPalette().color("bg"));
}

TEST_CASE("constructing a ThemeManager makes its palette current", "[ui_theme]") {
    const ThemeManager light(QStringLiteral("light"));
    CHECK(&currentPalette() == &lightPalette());
    const ThemeManager sepia(QStringLiteral("sepia"));
    CHECK(sepia.currentTheme() == QStringLiteral("dark"));
    CHECK(&currentPalette() == &darkPalette());
}

TEST_CASE("relative asset urls point into the resource tree", "[ui_theme]") {
    CHECK(absolutizeUrls(QStringLiteral("image: url(ui_qt/assets/check.svg);")) ==
          QStringLiteral("image: url(:/ui_theme/assets/check.svg);"));
    CHECK(absolutizeUrls(QStringLiteral("qproperty-icon: url( 'ui_qt/assets/tabler/x.svg' );")) ==
          QStringLiteral("qproperty-icon: url(:/ui_theme/assets/tabler/x.svg);"));
    for (const QString unchanged :
         {QStringLiteral("url(:/other/x.svg)"), QStringLiteral("url(/abs/x.svg)"),
          QStringLiteral("url(C:/x.svg)"), QStringLiteral("url(\"C:/x.svg\")"),
          QStringLiteral("url('/x.svg')")}) {
        CHECK(absolutizeUrls(unchanged) == unchanged);
    }
}

TEST_CASE("every url of the theme names a bundled asset", "[ui_theme]") {
    const ThemeManager manager;
    const QString sheet = manager.stylesheet();
    qsizetype at = 0;
    int count = 0;
    while ((at = sheet.indexOf(QStringLiteral("url("), at)) >= 0) {
        const qsizetype end = sheet.indexOf(u')', at);
        const QString path = sheet.mid(at + 4, end - at - 4);
        INFO(path.toStdString());
        CHECK(path.startsWith(QStringLiteral(":/ui_theme/assets/")));
        CHECK(QFile::exists(path));
        at = end;
        ++count;
    }
    CHECK(count > 0);
}

TEST_CASE("the QPalette maps roles and greys disabled text", "[ui_theme]") {
    const QPalette palette = buildQPalette(darkPalette());
    CHECK(palette.color(QPalette::Active, QPalette::Window) == darkPalette().color("bg"));
    CHECK(palette.color(QPalette::Active, QPalette::ToolTipBase) ==
          darkPalette().color("surface-hover"));
    CHECK(palette.color(QPalette::Active, QPalette::Shadow) == QColor(0, 0, 0));
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        CHECK(palette.color(QPalette::Disabled, role) == darkPalette().color("text-muted"));
        CHECK(palette.color(QPalette::Active, role) == darkPalette().color("text"));
    }
}

TEST_CASE("setStyleProperty changes and repolishes only on a new value", "[ui_theme]") {
    // Counts what repolish asks of the widget's style: unpolish then polish.
    struct CountingStyle : QProxyStyle {
        int unpolished = 0;
        int polished = 0;
        using QProxyStyle::QProxyStyle;
        void unpolish(QWidget* widget) override {
            ++unpolished;
            QProxyStyle::unpolish(widget);
        }
        void polish(QWidget* widget) override {
            ++polished;
            QProxyStyle::polish(widget);
        }
        using QProxyStyle::polish;
        using QProxyStyle::unpolish;
    };
    auto* style = new CountingStyle(QStyleFactory::create(QApplication::style()->name()));
    QLabel label;
    label.setStyle(style); // the label does not own it
    label.ensurePolished();
    const int polishedBefore = style->polished;

    setStyleProperty(&label, prop::tone, QStringLiteral("danger"));
    CHECK(label.property(prop::tone).toString() == QStringLiteral("danger"));
    CHECK(style->unpolished == 1);
    CHECK(style->polished == polishedBefore + 1);

    setStyleProperty(&label, prop::tone, QStringLiteral("danger"));
    CHECK(style->unpolished == 1);
    CHECK(style->polished == polishedBefore + 1);

    setStyleProperty(&label, prop::tone, QStringLiteral("ok"));
    CHECK(label.property(prop::tone).toString() == QStringLiteral("ok"));
    CHECK(style->unpolished == 2);
    CHECK(style->polished == polishedBefore + 2);
    label.setStyle(nullptr);
    delete style;
}

TEST_CASE("setStyleProperty makes a property selector match", "[ui_theme]") {
    QLabel label;
    setLocalStyleSheet(
        &label,
        QStringLiteral(R"(QLabel { color: #000000; } QLabel[tone="danger"] { color: #ff0000; })"));
    label.ensurePolished();
    CHECK(label.palette().color(QPalette::WindowText) == QColor(0, 0, 0));
    setStyleProperty(&label, prop::tone, QStringLiteral("danger"));
    CHECK(label.palette().color(QPalette::WindowText) == QColor(255, 0, 0));
    setStyleProperty(&label, prop::tone, QStringLiteral("ok"));
    CHECK(label.palette().color(QPalette::WindowText) == QColor(0, 0, 0));
}

TEST_CASE("the tooltip style shortens the wake-up delay", "[ui_theme]") {
    QApplication& app = *qApp;
    const bool hasWindows11 =
        QStyleFactory::keys().contains(QString::fromLatin1(kBaseStyle), Qt::CaseInsensitive);
    const QString baseName =
        hasWindows11 ? QString::fromLatin1(kBaseStyle) : QApplication::style()->name();
    installTooltipStyle(app);
    QStyle* style = QApplication::style();
    REQUIRE(qobject_cast<SnappyTooltipStyle*>(style) != nullptr);
    CHECK(style->styleHint(QStyle::SH_ToolTip_WakeUpDelay) == 200);
    CHECK(qobject_cast<SnappyTooltipStyle*>(style)->baseStyle()->name().compare(
              baseName, Qt::CaseInsensitive) == 0);
#ifdef Q_OS_WIN
    // The style the reference screenshots pin.
    CHECK(hasWindows11);
#endif
}

TEST_CASE("the tooltip filter makes the tip window translucent and shadowless", "[ui_theme]") {
    // tooltip_filter.py:34-56 (A02-19): a polished QTipLabel gets a translucent background and
    // no native drop shadow, so the QSS radius shows; other widgets are left alone.
    QApplication& app = *qApp;
    RoundedTooltipFilter filter;
    app.installEventFilter(&filter);
    QLabel other(QStringLiteral("not a tip"));
    other.ensurePolished();
    CHECK_FALSE(other.testAttribute(Qt::WA_TranslucentBackground));
    QWidget anchor;
    anchor.resize(80, 40);
    anchor.show();
    QToolTip::showText(QPoint(20, 20), QStringLiteral("Hint"), &anchor);
    QWidget* tip = nullptr;
    for (QWidget* widget : QApplication::topLevelWidgets()) {
        if (widget->inherits("QTipLabel")) {
            tip = widget;
        }
    }
    REQUIRE(tip != nullptr);
    if (QGuiApplication::platformName().startsWith(QStringLiteral("wayland"))) {
        CHECK(tip->styleSheet() == QStringLiteral("QToolTip { border-radius: 0px; }"));
    } else {
        CHECK(tip->testAttribute(Qt::WA_TranslucentBackground));
        CHECK(tip->windowFlags().testFlag(Qt::NoDropShadowWindowHint));
    }
    QToolTip::hideText();
    app.removeEventFilter(&filter);
}

TEST_CASE("the app icon is drawn at every size", "[ui_theme]") {
    const QIcon icon = appIcon();
    CHECK(icon.availableSizes().size() == static_cast<qsizetype>(kAppIconSizes.size()));
    for (const int size : kAppIconSizes) {
        const QImage image = renderAppPixmap(size).toImage();
        CHECK(image.width() == size);
        // Corners are transparent (rounded square); the centre is the gradient or the glyph.
        CHECK(qAlpha(image.pixel(0, 0)) == 0);
        CHECK(qAlpha(image.pixel(size / 2, size / 2)) == 255);
    }
}

TEST_CASE("Tabler icons load from the resources", "[ui_theme]") {
    int svgs = 0;
    QDirIterator it(QStringLiteral(":/ui_theme/assets/tabler"), {QStringLiteral("*.svg")});
    while (it.hasNext()) {
        it.next();
        ++svgs;
    }
    CHECK(svgs == 58);
    CHECK_FALSE(tablerIcon(QStringLiteral("download-blue.svg")).isNull());
    CHECK(tablerPixmap(QStringLiteral("download-blue.svg"), 14).size() == QSize(14, 14));
    const QIcon design = designIcon(QStringLiteral("check-green.svg"));
    CHECK_FALSE(design.pixmap(24, 24, QIcon::Disabled).isNull());
    CHECK(QFile::exists(QStringLiteral(":/ui_theme/assets/check.svg")));
    CHECK(QFile::exists(QStringLiteral(":/ui_theme/assets/spin-chevron-up.svg")));
    CHECK(QFile::exists(QStringLiteral(":/ui_theme/assets/spin-chevron-down.svg")));
}
