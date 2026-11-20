// The golden vectors of ui_qt.utils.palette.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>

#include "golden/golden.h"
#include "ui_theme/palette.h"

using fv::ui::Palette;

namespace {

constexpr char kModule[] = "ui_qt.utils.palette";

// A theme name for the built-in palettes, or {token: value} for Palette("test", tokens).
Palette paletteArg(const fv::golden::Case& c) {
    const fv::Json& which = c.args().at(0);
    if (which.is_string()) {
        return fv::ui::paletteFor(which.get<std::string>());
    }
    Palette::Tokens tokens;
    for (const auto& [token, value] : which.items()) {
        tokens.emplace(token, value.get<std::string>());
    }
    return Palette("test", std::move(tokens));
}

} // namespace

TEST_CASE("Palette resolve replays the golden vectors", "[golden][palette]") {
    fv::golden::for_each(kModule, "resolve", [](const fv::golden::Case& c) {
        const Palette palette = paletteArg(c);
        const auto text = c.arg<std::string>(1);
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(palette.resolve(std::string_view(text)), fv::ui::UnknownTokenError);
        } else {
            CHECK(palette.resolve(std::string_view(text)) == c.result<std::string>());
            CHECK(palette.resolve(QString::fromStdString(text)) ==
                  QString::fromStdString(c.result<std::string>()));
        }
    });
}

TEST_CASE("Palette css replays the golden vectors", "[golden][palette]") {
    fv::golden::for_each(kModule, "css", [](const fv::golden::Case& c) {
        const Palette palette = paletteArg(c);
        const auto token = c.arg<std::string>(1);
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(palette.css(token), fv::ui::UnknownTokenError);
        } else {
            CHECK(palette.css(token) == c.result<std::string>());
        }
    });
}

TEST_CASE("Palette color replays the golden vectors", "[golden][palette]") {
    fv::golden::for_each(kModule, "color", [](const fv::golden::Case& c) {
        const Palette palette = paletteArg(c);
        const auto token = c.arg<std::string>(1);
        std::optional<int> alpha;
        if (c.arg_count() > 2) {
            alpha = c.arg<std::optional<int>>(2);
        }
        if (c.raises()) {
            CHECK(c.raises_type() == "KeyError");
            CHECK_THROWS_AS(palette.color(token, alpha), fv::ui::UnknownTokenError);
            return;
        }
        const QColor color = palette.color(token, alpha);
        const fv::Json& expected = c.raw().at("result");
        CHECK(color.isValid() == expected.at(0).get<bool>());
        CHECK(color.name(QColor::HexArgb).toStdString() == expected.at(1).get<std::string>());
    });
}

TEST_CASE("paletteFor replays the golden vectors", "[golden][palette]") {
    fv::golden::for_each(kModule, "palette_for", [](const fv::golden::Case& c) {
        const Palette& palette = fv::ui::paletteFor(c.arg<std::string>(0));
        const fv::Json& expected = c.raw().at("result");
        CHECK(palette.name() == expected.at(0).get<std::string>());
        CHECK(palette.isDark() == expected.at(1).get<bool>());
    });
}
