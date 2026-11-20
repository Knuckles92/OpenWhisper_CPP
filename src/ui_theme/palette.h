#pragma once

#include <functional>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <QColor>
#include <QString>
#include <QStringList>

namespace fv::ui {

// Theme names, as the palette data and ThemeManager spell them.
inline constexpr char kDark[] = "dark";
inline constexpr char kLight[] = "light";

// A token the palette does not define: Python's KeyError, so a typo cannot ship as literal
// "@name" text that Qt silently ignores.
class UnknownTokenError : public std::out_of_range {
public:
    explicit UnknownTokenError(std::string_view token);
    const std::string& token() const { return token_; }

private:
    std::string token_;
};

// Semantic colour tokens for one theme. theme.qss and widget-local sheets name colours by role
// ("@surface", "@text-secondary"); resolve() substitutes this theme's value for each. Tokens
// ending in "-rgb" are bare "r, g, b" triples for rgba(@accent-rgb, 0.12). The built-in
// palettes come from data/palettes.json (tools/gen/palettes.py). Equality is by name and
// token values, as Python's frozen dataclass compares.
class Palette {
public:
    using Tokens = std::map<std::string, std::string, std::less<>>;

    Palette(std::string name, Tokens tokens);

    const std::string& name() const { return name_; }
    QString qName() const { return QString::fromStdString(name_); }
    bool isDark() const { return name_ == kDark; }
    const Tokens& tokens() const { return tokens_; }

    // The stylesheet value of token ("#1c1c1e", "10, 132, 255"). Throws UnknownTokenError.
    std::string css(std::string_view token) const;
    QString qCss(QStringView token) const;

    // token as a colour: "#hex", "rgba(r, g, b, a)" (a in 0-1, through QColor::setAlphaF) and
    // "r, g, b" triples (opaque). alpha, in 0-255, replaces the token's own. Throws
    // UnknownTokenError.
    QColor color(std::string_view token, std::optional<int> alpha = std::nullopt) const;

    // Every "@token" in text substituted; text without "@" is returned as it is. Throws
    // UnknownTokenError.
    std::string resolve(std::string_view text) const;
    QString resolve(const QString& text) const;

    // As resolve, but an unknown token is left in place and its name added to unknown: a style
    // path never throws (architecture 8.1).
    std::string resolveLenient(std::string_view text, QStringList* unknown) const;

    friend bool operator==(const Palette&, const Palette&) = default;

private:
    std::string name_;
    Tokens tokens_;
};

const Palette& darkPalette();
const Palette& lightPalette();
// The built-in palette of a theme name; any other name is the dark one.
const Palette& paletteFor(std::string_view theme);
const Palette& paletteFor(const QString& theme);

// The palette the application is styled with (dark until set). Painted widgets read it, and
// widget-local sheets resolve against it. It must outlive its use as current: pass one of the
// built-in palettes. Affinity: GUI thread.
const Palette& currentPalette();
void setCurrentPalette(const Palette& palette);
// currentPalette().resolve(text) and currentPalette().color(token, alpha).
QString resolveTokens(const QString& text);
QColor tokenColor(std::string_view token, std::optional<int> alpha = std::nullopt);

} // namespace fv::ui
