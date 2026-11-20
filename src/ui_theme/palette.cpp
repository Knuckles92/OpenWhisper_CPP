#include "ui_theme/palette.h"

#include <string>
#include <utility>

#include "base/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "ui_theme/data/palettes.json.h"

namespace fv::ui {
namespace {

const py::Regex& tokenRe() {
    static const py::Regex re(R"(@([a-z][a-z0-9]*(?:-[a-z0-9]+)*))");
    return re;
}

const py::Regex& rgbTripleRe() {
    static const py::Regex re(R"(^\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*$)");
    return re;
}

struct BuiltIn {
    Palette dark;
    Palette light;
};

Palette fromJson(const Json& palettes, const char* name) {
    Palette::Tokens tokens;
    for (const auto& [token, value] : palettes.at(name).items()) {
        tokens.emplace(token, value.get<std::string>());
    }
    return Palette(name, std::move(tokens));
}

const BuiltIn& builtIn() {
    static const BuiltIn palettes = [] {
        const Json document = Json::parse(fv::embedded::ui_theme_palettes);
        const Json& all = document.at("palettes");
        return BuiltIn{fromJson(all, kDark), fromJson(all, kLight)};
    }();
    return palettes;
}

int parseInt(const std::string& digits) {
    // \d+ is Unicode Nd, which int() accepts as well.
    return static_cast<int>(py::py_int(digits).value_or(0));
}

const Palette* current = nullptr;

} // namespace

UnknownTokenError::UnknownTokenError(std::string_view token)
    : std::out_of_range("Unknown palette token '@" + std::string(token) + "'"), token_(token) {}

Palette::Palette(std::string name, Tokens tokens)
    : name_(std::move(name)), tokens_(std::move(tokens)) {}

std::string Palette::css(std::string_view token) const {
    const auto found = tokens_.find(token);
    if (found == tokens_.end()) {
        throw UnknownTokenError(token);
    }
    return found->second;
}

QString Palette::qCss(QStringView token) const {
    return QString::fromStdString(css(token.toString().toStdString()));
}

QColor Palette::color(std::string_view token, std::optional<int> alpha) const {
    const std::string value = css(token);
    QColor color;
    if (const std::optional<py::Match> triple = rgbTripleRe().match(value)) {
        color = QColor(parseInt(triple->group(1)), parseInt(triple->group(2)),
                       parseInt(triple->group(3)));
    } else if (value.starts_with("rgba(")) {
        // value[5:-1].split(","), each part stripped.
        const std::string inner = value.substr(5, value.size() - 6);
        QStringList parts = QString::fromStdString(inner).split(u',');
        for (QString& part : parts) {
            part = part.trimmed();
        }
        color = QColor(parts.value(0).toInt(), parts.value(1).toInt(), parts.value(2).toInt());
        color.setAlphaF(py::py_float(parts.value(3).toStdString()).value_or(0.0));
    } else {
        color = QColor::fromString(QString::fromStdString(value));
    }
    if (alpha) {
        color.setAlpha(*alpha);
    }
    return color;
}

std::string Palette::resolve(std::string_view text) const {
    if (text.find('@') == std::string_view::npos) {
        return std::string(text);
    }
    return tokenRe().sub([this](const py::Match& match) { return css(match.group(1)); }, text);
}

QString Palette::resolve(const QString& text) const {
    if (!text.contains(u'@')) {
        return text;
    }
    return QString::fromStdString(resolve(text.toStdString()));
}

std::string Palette::resolveLenient(std::string_view text, QStringList* unknown) const {
    if (text.find('@') == std::string_view::npos) {
        return std::string(text);
    }
    return tokenRe().sub(
        [this, unknown](const py::Match& match) {
            const std::string name = match.group(1);
            const auto found = tokens_.find(name);
            if (found != tokens_.end()) {
                return found->second;
            }
            if (unknown != nullptr) {
                unknown->append(QString::fromStdString(name));
            }
            return match.group(0);
        },
        text);
}

const Palette& darkPalette() {
    return builtIn().dark;
}

const Palette& lightPalette() {
    return builtIn().light;
}

const Palette& paletteFor(std::string_view theme) {
    return theme == kLight ? lightPalette() : darkPalette();
}

const Palette& paletteFor(const QString& theme) {
    return paletteFor(theme.toStdString());
}

const Palette& currentPalette() {
    return current != nullptr ? *current : darkPalette();
}

void setCurrentPalette(const Palette& palette) {
    current = &palette;
}

QString resolveTokens(const QString& text) {
    return currentPalette().resolve(text);
}

QColor tokenColor(std::string_view token, std::optional<int> alpha) {
    return currentPalette().color(token, alpha);
}

} // namespace fv::ui
