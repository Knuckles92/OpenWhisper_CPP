#include "ui_theme/theme_manager.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QStyleHints>

#include <optional>
#include <string>
#include <string_view>

#include "base/log.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "ui_theme/font_scale.h"

namespace fv::ui {
namespace {

constexpr log::Logger logger("ui_qt.utils.theme_manager");

constexpr char kThemePath[] = ":/ui_theme/theme.qss";
constexpr char kResourceRoot[] = ":/ui_theme/";
// OpenWhisper's sheet names its assets relative to the bundle root, whose ui_qt/ folder is this
// library's resource tree.
constexpr std::string_view kBundleFolder = "ui_qt/";

// An unquoted or quoted url() that is not absolute: no drive letter, no leading ":" or "/".
const py::Regex& relativeUrlRe() {
    static const py::Regex re(R"(url\(\s*(?!['"]?[a-zA-Z]:)(?!['"]?[:/])([^)]+?)\s*\))");
    return re;
}

// Universal newlines, as Python's read_text gives the template.
QString universalNewlines(QString text) {
    text.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    text.replace(u'\r', u'\n');
    return text;
}

std::optional<QString> readResource(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return std::nullopt;
    }
    return universalNewlines(QString::fromUtf8(file.readAll()));
}

// FioriVoce-only rules: :/<dir>/<dir>.qss of every other ui directory, in sorted order.
QString featureSheets() {
    QString sheets;
    const QStringList dirs =
        QDir(QStringLiteral(":/")).entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    for (const QString& dir : dirs) {
        if (dir == QStringLiteral("ui_theme")) {
            continue;
        }
        const QString path = QStringLiteral(":/%1/%1.qss").arg(dir);
        if (!QFile::exists(path)) {
            continue;
        }
        if (const std::optional<QString> text = readResource(path)) {
            sheets += u'\n';
            sheets += *text;
        } else {
            logger.error("Error loading stylesheet from {}", path.toStdString());
        }
    }
    return sheets;
}

} // namespace

QString absolutizeUrls(const QString& stylesheet) {
    const std::string text = stylesheet.toStdString();
    return QString::fromStdString(relativeUrlRe().sub(
        [](const py::Match& match) {
            const std::string relative = py::strip(match.group(1), "'\"");
            if (!relative.starts_with(kBundleFolder)) {
                return match.group(0);
            }
            return "url(" + std::string(kResourceRoot) + relative.substr(kBundleFolder.size()) +
                   ")";
        },
        text));
}

ThemeManager::ThemeManager(const QString& theme, QObject* parent)
    : QObject(parent), palette_(&paletteFor(theme)) {
    setCurrentPalette(*palette_);
    loadStylesheet();
}

void ThemeManager::loadStylesheet() {
    const std::optional<QString> theme = readResource(QString::fromLatin1(kThemePath));
    if (!theme) {
        logger.error("Stylesheet not found at {}; UI will be unstyled", kThemePath);
        return;
    }
    template_ = absolutizeUrls(*theme + featureSheets());
}

QString ThemeManager::stylesheet() const {
    QStringList unknown;
    const QString resolved =
        QString::fromStdString(palette_->resolveLenient(template_.toStdString(), &unknown));
    if (!unknown.isEmpty()) {
        logger.error("Unknown palette tokens in the application stylesheet: @{}",
                     unknown.join(QStringLiteral(", @")).toStdString());
    }
    return resolved;
}

QString ThemeManager::scaledStylesheet(double scale) const {
    return scaleQssFonts(stylesheet(), scale);
}

bool ThemeManager::setTheme(const QString& name) {
    const Palette& palette = paletteFor(name);
    if (palette == *palette_) {
        return false;
    }
    palette_ = &palette;
    setCurrentPalette(palette);
    Q_EMIT themeChanged(palette.qName());
    return true;
}

void ThemeManager::applyPlatformPalette(QApplication& app, bool pinColorScheme) {
    app.setPalette(buildQPalette(*palette_));
    if (!pinColorScheme) {
        return;
    }
    app.styleHints()->setColorScheme(palette_->isDark() ? Qt::ColorScheme::Dark
                                                        : Qt::ColorScheme::Light);
}

QPalette buildQPalette(const Palette& palette) {
    struct RoleToken {
        QPalette::ColorRole role;
        const char* token;
    };
    static constexpr RoleToken roles[] = {
        {QPalette::Window, "bg"},           {QPalette::WindowText, "text"},
        {QPalette::Base, "surface"},        {QPalette::AlternateBase, "bg"},
        {QPalette::Text, "text"},           {QPalette::Button, "surface"},
        {QPalette::ButtonText, "text"},     {QPalette::ToolTipBase, "surface-hover"},
        {QPalette::ToolTipText, "text"},    {QPalette::PlaceholderText, "text-secondary"},
        {QPalette::Highlight, "accent"},    {QPalette::HighlightedText, "on-accent"},
        {QPalette::Link, "accent"},         {QPalette::LinkVisited, "accent-soft"},
        {QPalette::Mid, "border"},          {QPalette::Dark, "border-strong"},
        {QPalette::Light, "surface-hover"}, {QPalette::Midlight, "surface-hover"},
        {QPalette::Shadow, "shadow-rgb"},
    };
    QPalette qpalette;
    for (const RoleToken& entry : roles) {
        qpalette.setColor(entry.role, palette.color(entry.token));
    }
    const QColor disabled = palette.color("text-muted");
    for (const QPalette::ColorRole role :
         {QPalette::WindowText, QPalette::Text, QPalette::ButtonText}) {
        qpalette.setColor(QPalette::Disabled, role, disabled);
    }
    return qpalette;
}

} // namespace fv::ui
