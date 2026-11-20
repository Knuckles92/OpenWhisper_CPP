// Ported from tests/test_ui_theme.py. Each test case runs in its own process under CTest, so the
// application's theme state starts fresh, as OpenWhisper's isolated_app_theme fixture ensures.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QFile>
#include <QLabel>
#include <QPalette>
#include <QRegularExpression>
#include <QStringList>
#include <QStyleHints>

#include <set>
#include <string>

#include "base/json.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme_manager.h"

using namespace fv::ui;

namespace {

QApplication& app() {
    return *qApp;
}

std::set<std::string> tokenNames(const Palette& palette) {
    std::set<std::string> names;
    for (const auto& [name, value] : palette.tokens()) {
        names.insert(name);
    }
    return names;
}

} // namespace

// from tests/test_ui_theme.py::TestPalette::test_both_themes_define_the_same_tokens
TEST_CASE("both themes define the same tokens", "[test_ui_theme]") {
    CHECK(tokenNames(darkPalette()) == tokenNames(lightPalette()));
    CHECK(darkPalette().tokens().size() == 96);
}

// from
// tests/test_ui_theme.py::TestPalette::test_resolve_substitutes_every_token_and_leaves_the_rest
TEST_CASE("resolve substitutes every token and leaves the rest", "[test_ui_theme]") {
    const std::string source = "QLabel { color: @text; background: rgba(@accent-rgb, 0.5); }";
    const std::string resolved = darkPalette().resolve(source);
    CHECK(resolved.find('@') == std::string::npos);
    CHECK(resolved.find(darkPalette().css("text")) != std::string::npos);
    CHECK(resolved.find("rgba(" + darkPalette().css("accent-rgb") + ", 0.5)") != std::string::npos);
    CHECK(lightPalette().resolve(source) != resolved);
}

// from tests/test_ui_theme.py::TestPalette::test_unknown_token_is_an_error_not_silent_text
TEST_CASE("unknown token is an error not silent text", "[test_ui_theme]") {
    try {
        (void)darkPalette().resolve(std::string_view("color: @no-such-token;"));
        FAIL("resolve accepted an unknown token");
    } catch (const UnknownTokenError& error) {
        CHECK(std::string(error.what()).find("@no-such-token") != std::string::npos);
        CHECK(error.token() == "no-such-token");
    }
}

// from tests/test_ui_theme.py::TestPalette::test_color_handles_hex_rgba_and_rgb_triples
TEST_CASE("color handles hex rgba and rgb triples", "[test_ui_theme]") {
    const Palette palette(
        "test", {{"hex", "#0a84ff"}, {"rgba", "rgba(28, 28, 30, 0.5)"}, {"rgb", "10, 132, 255"}});
    CHECK(palette.color("hex") == QColor::fromString(QStringLiteral("#0a84ff")));
    const QColor rgba = palette.color("rgba");
    CHECK(rgba.red() == 28);
    CHECK(rgba.green() == 28);
    CHECK(rgba.blue() == 30);
    CHECK(rgba.alpha() == 128);
    CHECK(palette.color("rgb", 40) == QColor(10, 132, 255, 40));
}

// from
// tests/test_ui_theme.py::TestPalette::test_theme_stylesheet_has_no_unresolved_tokens_in_either_theme
TEST_CASE("theme stylesheet has no unresolved tokens in either theme", "[test_ui_theme]") {
    for (const char* theme : {kDark, kLight}) {
        const ThemeManager manager(QString::fromLatin1(theme));
        const QString sheet = manager.stylesheet();
        CHECK_FALSE(sheet.isEmpty());
        CHECK_FALSE(sheet.contains(u'@'));
        CHECK(manager.currentTheme() == QString::fromLatin1(theme));
    }
}

// from tests/test_ui_theme.py::TestPalette::test_themes_differ_only_in_colours_and_icon_variants
// (the "classic" parameter; the "omarchy" one exercises the Linux desktop rewrite, not ported)
TEST_CASE("themes differ only in colours and icon variants", "[test_ui_theme]") {
    const QRegularExpression colour(
        QStringLiteral(R"(#[0-9a-fA-F]{3,8}\b|rgba?\([^)]*\)|\b\d{1,3},\s*\d{1,3},\s*\d{1,3}\b)"));
    const QRegularExpression lightIcon(QStringLiteral(R"(-light\.svg)"));
    QStringList sheets;
    for (const char* theme : {kDark, kLight}) {
        QString sheet = ThemeManager(QString::fromLatin1(theme)).stylesheet();
        sheet.replace(colour, QStringLiteral("C"));
        sheet.replace(lightIcon, QStringLiteral(".svg"));
        sheets << sheet;
    }
    CHECK(sheets.at(0) == sheets.at(1));
}

// from tests/test_ui_theme.py::TestPalette::test_light_icon_variants_exist
TEST_CASE("light icon variants exist", "[test_ui_theme]") {
    for (const char* name : {"chevron-down-slate", "chevron-down-gray", "chevron-up-gray"}) {
        const QString base = QStringLiteral(":/ui_theme/assets/tabler/") + QLatin1String(name);
        CHECK(QFile::exists(base + QStringLiteral(".svg")));
        CHECK(QFile::exists(base + QStringLiteral("-light.svg")));
    }
}

// from
// tests/test_ui_theme.py::TestApplyUiTheme::test_switching_restyles_the_app_and_widget_stylesheets
TEST_CASE("switching restyles the app and widget stylesheets", "[test_ui_theme]") {
    ThemeManager manager;
    applyUiTheme(QStringLiteral("dark"), app(), manager);
    QLabel label;
    setLocalStyleSheet(&label, QStringLiteral("QLabel { color: @text; }"));
    CHECK(label.styleSheet() ==
          QStringLiteral("QLabel { color: %1; }").arg(darkPalette().qCss(u"text")));

    const bool changed = applyUiTheme(QStringLiteral("light"), app(), manager);
    CHECK(changed);
    CHECK(&currentPalette() == &lightPalette());
    CHECK(label.styleSheet() ==
          QStringLiteral("QLabel { color: %1; }").arg(lightPalette().qCss(u"text")));
    CHECK(app().styleSheet().contains(lightPalette().qCss(u"bg")));
    CHECK(app().palette().color(QPalette::Window) == lightPalette().color("bg"));

    CHECK_FALSE(applyUiTheme(QStringLiteral("light"), app(), manager));
}

// from tests/test_ui_theme.py::TestApplyUiTheme::test_theme_manager_announces_changes_once
TEST_CASE("theme manager announces changes once", "[test_ui_theme]") {
    ThemeManager manager;
    QStringList seen;
    QObject::connect(&manager, &ThemeManager::themeChanged,
                     [&seen](const QString& name) { seen << name; });
    CHECK(manager.setTheme(QStringLiteral("light")));
    CHECK_FALSE(manager.setTheme(QStringLiteral("light")));
    CHECK(manager.setTheme(QStringLiteral("dark")));
    CHECK(seen == QStringList{QStringLiteral("light"), QStringLiteral("dark")});
}

// from tests/test_ui_theme.py::TestApplyUiTheme::test_system_preference_reads_the_platform_scheme
TEST_CASE("system preference reads the platform scheme", "[test_ui_theme]") {
    CHECK(resolveThemePreference(QStringLiteral("light"), &app()) == QStringLiteral("light"));
    CHECK(resolveThemePreference(QStringLiteral("dark"), &app()) == QStringLiteral("dark"));
    CHECK(resolveThemePreference(QStringLiteral("garbage"), &app()) == QStringLiteral("dark"));
    const QString system = resolveThemePreference(QStringLiteral("system"), &app());
    CHECK((system == QStringLiteral("dark") || system == QStringLiteral("light")));
    ThemeManager manager;
    applyUiTheme(QStringLiteral("system"), app(), manager);
    CHECK(currentUiThemePreference() == QStringLiteral("system"));
}

TEST_CASE("match system follows a platform scheme change", "[ui_theme]") {
    QStyleHints* hints = QGuiApplication::styleHints();
    ThemeManager manager;
    followPlatformColorScheme(app(), manager);
    applyUiTheme(QStringLiteral("system"), app(), manager);
    const QString system = resolveThemePreference(QStringLiteral("system"), &app());
    const QString other =
        system == QStringLiteral("light") ? QStringLiteral("dark") : QStringLiteral("light");

    // Knock the palette off the system one; a scheme change while following brings it back.
    manager.setTheme(other);
    Q_EMIT hints->colorSchemeChanged(hints->colorScheme());
    CHECK(manager.currentTheme() == system);

    // An explicit choice ignores scheme changes.
    applyUiTheme(other, app(), manager);
    Q_EMIT hints->colorSchemeChanged(hints->colorScheme());
    CHECK(manager.currentTheme() == other);
}

// from tests/test_ui_theme.py::TestThemeSetting::test_fresh_classic_install_is_dark
TEST_CASE("fresh classic install is dark", "[test_ui_theme]") {
    CHECK(resolveUiTheme(fv::Json::object()) == QStringLiteral("dark"));
}

// from tests/test_ui_theme.py::TestThemeSetting::test_invalid_saved_value_falls_back_to_default
TEST_CASE("invalid saved value falls back to default", "[test_ui_theme]") {
    CHECK(resolveUiTheme(fv::Json{{"ui_theme", "sepia"}}) == QStringLiteral("dark"));
    CHECK(resolveUiTheme(fv::Json{{"ui_theme", "light"}}) == QStringLiteral("light"));
    CHECK(resolveUiTheme(fv::Json{{"ui_theme", "system"}}) == QStringLiteral("system"));
    CHECK(resolveUiTheme(fv::Json{{"ui_theme", 3}}) == QStringLiteral("dark"));
    CHECK(resolveUiTheme(fv::Json{{"ui_theme", "omarchy"}}) == QStringLiteral("omarchy"));
}

// from tests/test_ui_theme.py::TestThemeSetting::test_palette_for_unknown_name_is_dark
TEST_CASE("palette for unknown name is dark", "[test_ui_theme]") {
    CHECK(&paletteFor(std::string_view("sepia")) == &darkPalette());
}
