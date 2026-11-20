#include "ui_theme/font_scale.h"

#include <QApplication>
#include <QFont>
#include <QPointer>
#include <QStyleHints>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "base/log.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "ui_theme/local_style.h"
#include "ui_theme/palette.h"
#include "ui_theme/theme_manager.h"

namespace fv::ui {
namespace {

constexpr log::Logger logger("ui_qt.utils.font_scale");

constexpr int kBaseAppPointSize = 10;
constexpr char kAppFontFamily[] = "Segoe UI";
constexpr char kUiThemeKey[] = "ui_theme";

int currentPercent = 100;
QString themePreference = QString::fromLatin1(kUiThemeDark);
QPointer<ThemeManager> themeManager;

const py::Regex& fontSizeRe() {
    static const py::Regex re(R"((font-size:\s*)(\d+(?:\.\d+)?)(px|pt))", py::IGNORECASE);
    return re;
}

std::string scaledNumber(const std::string& number, double scale) {
    const double value = py::py_float(number).value_or(0.0) * scale;
    if (number.find('.') != std::string::npos) {
        // f"{value:.1f}".rstrip("0").rstrip(".")
        return py::rstrip(py::rstrip(py::format_fixed(value, 1), "0"), ".");
    }
    const auto rounded = static_cast<std::int64_t>(py::round_half_even(value));
    return std::to_string(std::max<std::int64_t>(1, rounded));
}

} // namespace

std::string scaleQssFonts(std::string_view stylesheet, double scale) {
    if (stylesheet.empty() || std::abs(scale - 1.0) < 1e-6) {
        return std::string(stylesheet);
    }
    return fontSizeRe().sub(
        [scale](const py::Match& match) {
            return match.group(1) + scaledNumber(match.group(2), scale) + match.group(3);
        },
        stylesheet);
}

QString scaleQssFonts(const QString& stylesheet, double scale) {
    if (stylesheet.isEmpty() || std::abs(scale - 1.0) < 1e-6) {
        return stylesheet;
    }
    return QString::fromStdString(scaleQssFonts(stylesheet.toStdString(), scale));
}

QString resolveStylesheet(const QString& source, std::optional<double> scale) {
    QStringList unknown;
    const std::string resolved = currentPalette().resolveLenient(source.toStdString(), &unknown);
    if (!unknown.isEmpty()) {
        logger.error("Unknown palette tokens in a widget stylesheet: @{}",
                     unknown.join(QStringLiteral(", @")).toStdString());
    }
    return QString::fromStdString(scaleQssFonts(resolved, scale.value_or(currentUiFontScale())));
}

double currentUiFontScale() {
    return currentPercent / 100.0;
}

int currentUiFontScalePercent() {
    return currentPercent;
}

int scaled(int px) {
    return static_cast<int>(py::round_half_even(px * currentUiFontScale()));
}

void applyUiFontScale(int percent, QApplication& app, ThemeManager& manager) {
    themeManager = &manager;
    applyUiFontScale(percent, app);
}

void applyUiFontScale(int percent, QApplication& app) {
    currentPercent = percent;
    const double scale = currentPercent / 100.0;
    if (themeManager != nullptr) {
        const QString sheet = themeManager->scaledStylesheet(scale);
        if (!sheet.isEmpty()) {
            app.setStyleSheet(sheet);
        }
    }
    const int pointSize =
        std::max(6, static_cast<int>(py::round_half_even(kBaseAppPointSize * scale)));
    app.setFont(QFont(QString::fromLatin1(kAppFontFamily), pointSize));
    resolveLocalStyleSheets(app, scale);
}

QString resolveThemePreference(const QString& preference, const QApplication* app) {
    if (preference == QLatin1StringView(kUiThemeLight)) {
        return QString::fromLatin1(kLight);
    }
    if (preference != QLatin1StringView(kUiThemeSystem) || app == nullptr) {
        return QString::fromLatin1(kDark);
    }
    return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Light
               ? QString::fromLatin1(kLight)
               : QString::fromLatin1(kDark);
}

QString currentUiThemePreference() {
    return themePreference;
}

bool applyUiTheme(const QString& preference, QApplication& app, ThemeManager& manager) {
    themeManager = &manager;
    themePreference = preference;
    const bool followSystem = preference == QLatin1StringView(kUiThemeSystem);
    if (followSystem) {
        QGuiApplication::styleHints()->unsetColorScheme();
    }
    const bool changed = manager.setTheme(resolveThemePreference(preference, &app));
    manager.applyPlatformPalette(app, !followSystem);
    if (changed) {
        applyUiFontScale(currentPercent, app);
    }
    return changed;
}

void followPlatformColorScheme(QApplication& app, ThemeManager& manager) {
    QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &manager,
                     [&app, &manager](Qt::ColorScheme) {
                         if (themePreference == QLatin1StringView(kUiThemeSystem)) {
                             applyUiTheme(QString::fromLatin1(kUiThemeSystem), app, manager);
                         }
                     });
}

QString resolveUiTheme(const Json& settings) {
    // ChoiceSetting.read: a stored string among the choices, anything else the default.
    if (settings.is_object()) {
        const auto found = settings.find(kUiThemeKey);
        if (found != settings.end() && found->is_string()) {
            const std::string value = found->get<std::string>();
            for (const char* choice :
                 {kUiThemeDark, kUiThemeLight, kUiThemeSystem, kUiThemeOmarchy}) {
                if (value == choice) {
                    return QString::fromStdString(value);
                }
            }
        }
    }
    return QString::fromLatin1(kUiThemeDark);
}

} // namespace fv::ui
