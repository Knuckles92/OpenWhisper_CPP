// Ported from tests/test_ui_font_scale.py, plus the golden vectors of ui_qt.utils.font_scale.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFont>
#include <QLabel>

#include <string>

#include "golden/golden.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme_manager.h"

using namespace fv::ui;

TEST_CASE("scaleQssFonts replays the golden vectors", "[golden][font_scale]") {
    fv::golden::for_each(
        "ui_qt.utils.font_scale", "scale_qss_fonts", [](const fv::golden::Case& c) {
            const auto sheet = c.arg<std::string>(0);
            const auto scale = c.arg<double>(1);
            CHECK(scaleQssFonts(std::string_view(sheet), scale) == c.result<std::string>());
            CHECK(scaleQssFonts(QString::fromStdString(sheet), scale) ==
                  QString::fromStdString(c.result<std::string>()));
        });
}

// from
// tests/test_ui_font_scale.py::TestScaleQssFonts::test_multiplies_integer_px_and_leaves_other_lengths
TEST_CASE("multiplies integer px and leaves other lengths", "[test_ui_font_scale]") {
    CHECK(scaleQssFonts(
              QStringLiteral("QLabel { font-size: 14px; min-height: 14px; padding: 8px; }"), 1.3) ==
          QStringLiteral("QLabel { font-size: 18px; min-height: 14px; padding: 8px; }"));
}

// from tests/test_ui_font_scale.py::TestScaleQssFonts::test_identity_scale_returns_the_same_string
TEST_CASE("identity scale returns the same string", "[test_ui_font_scale]") {
    const QString source = QStringLiteral("font-size: 14px;");
    const QString result = scaleQssFonts(source, 1.0);
    // Python checks identity (`is`); a shared QString is the same data, not a copy.
    CHECK(result.constData() == source.constData());
}

// from tests/test_ui_font_scale.py::TestScaleQssFonts::test_scales_pt_and_decimal_px
TEST_CASE("scales pt and decimal px", "[test_ui_font_scale]") {
    CHECK(scaleQssFonts(QStringLiteral("font-size: 10pt;"), 1.5) ==
          QStringLiteral("font-size: 15pt;"));
    CHECK(scaleQssFonts(QStringLiteral("font-size: 10.5px;"), 2.0) ==
          QStringLiteral("font-size: 21px;"));
}

// from
// tests/test_ui_font_scale.py::TestApplyUiFontScale::test_rewrites_widget_stylesheet_from_the_original
TEST_CASE("rewrites widget stylesheet from the original", "[test_ui_font_scale]") {
    QApplication& app = *qApp;
    QLabel label;
    setLocalStyleSheet(&label, QStringLiteral("QLabel { font-size: 10px; }"));
    // A sheet set directly, not through setLocalStyleSheet, is adopted as its own source.
    QLabel raw;
    raw.setStyleSheet(QStringLiteral("QLabel { font-size: 10px; }"));

    applyUiFontScale(130, app);
    CHECK(label.styleSheet().contains(QStringLiteral("13px")));
    CHECK(raw.styleSheet().contains(QStringLiteral("13px")));
    applyUiFontScale(115, app);
    CHECK(label.styleSheet().contains(QStringLiteral("12px")));
    CHECK(raw.styleSheet().contains(QStringLiteral("12px")));
    applyUiFontScale(100, app);
    CHECK(label.styleSheet().contains(QStringLiteral("10px")));
    CHECK(raw.styleSheet().contains(QStringLiteral("10px")));
    CHECK(label.property(prop::unscaledStyleSheet).toString() ==
          QStringLiteral("QLabel { font-size: 10px; }"));
}

// from tests/test_ui_font_scale.py::TestApplyUiFontScale::test_theme_stylesheet_font_sizes_scale
TEST_CASE("theme stylesheet font sizes scale", "[test_ui_font_scale]") {
    QApplication& app = *qApp;
    ThemeManager manager;
    const QString designed = manager.stylesheet();
    CHECK(designed.contains(QStringLiteral("font-size: 14px")));
    const QString scaledSheet = manager.scaledStylesheet(1.3);
    CHECK(scaledSheet.count(QStringLiteral("font-size: 18px")) ==
          designed.count(QStringLiteral("font-size: 14px")));
    applyUiFontScale(130, app, manager);
    CHECK(app.styleSheet().contains(QStringLiteral("font-size: 18px")));
    CHECK(currentUiFontScalePercent() == 130);
    CHECK(currentUiFontScale() == 1.3);
}

TEST_CASE("the app font follows the scale with half-to-even rounding", "[font_scale]") {
    QApplication& app = *qApp;
    for (const auto& [percent, points] :
         {std::pair{90, 9}, std::pair{100, 10}, std::pair{115, 12}, std::pair{130, 13}}) {
        applyUiFontScale(percent, app);
        CHECK(app.font().family() == QStringLiteral("Segoe UI"));
        CHECK(app.font().pointSize() == points);
    }
}

TEST_CASE("scaled sizes follow the active scale", "[font_scale]") {
    QApplication& app = *qApp;
    applyUiFontScale(115, app);
    CHECK(scaled(320) == 368);
    applyUiFontScale(130, app);
    CHECK(scaled(400) == 520);
    applyUiFontScale(90, app);
    CHECK(scaled(5) == 4); // 4.5 rounds to the even 4
    applyUiFontScale(100, app);
    CHECK(scaled(46) == 46);
}

TEST_CASE("a sheet set directly over a local sheet becomes its source", "[font_scale]") {
    QApplication& app = *qApp;
    QLabel label;
    setLocalStyleSheet(&label, QStringLiteral("QLabel { font-size: 10px; }"));
    label.setStyleSheet(QStringLiteral("QLabel { font-size: 20px; }"));
    applyUiFontScale(130, app);
    CHECK(label.styleSheet() == QStringLiteral("QLabel { font-size: 26px; }"));
    applyUiFontScale(100, app);
    CHECK(label.styleSheet() == QStringLiteral("QLabel { font-size: 20px; }"));
    CHECK(label.property(prop::unscaledStyleSheet).toString() ==
          QStringLiteral("QLabel { font-size: 20px; }"));
}

TEST_CASE("an unknown token in a local sheet is kept, not thrown", "[font_scale]") {
    QLabel label;
    setLocalStyleSheet(&label, QStringLiteral("QLabel { color: @no-such-token; }"));
    CHECK(label.styleSheet() == QStringLiteral("QLabel { color: @no-such-token; }"));
}
