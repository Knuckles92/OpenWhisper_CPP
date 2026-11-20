// The History calendar's words and tints against OpenWhisper's helpers
// (tests/golden/ui_qt.widgets.history_calendar.json).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <QColor>

#include "golden/golden.h"
#include "pyport/datetime.h"
#include "ui_history/history_calendar.h"
#include "ui_theme/palette.h"

namespace golden = fv::golden;
namespace cv = fv::calendar_view;
namespace hc = fv::history;

namespace {

constexpr char kModule[] = "ui_qt.widgets.history_calendar";

std::map<std::string, std::int64_t, std::less<>> counts(const fv::Json& object) {
    std::map<std::string, std::int64_t, std::less<>> result;
    for (const auto& [kind, count] : object.items()) {
        result[kind] = count.get<std::int64_t>();
    }
    return result;
}

hc::Day dayFrom(const std::string& iso) {
    const fv::py::Date date = fv::py::date_fromisoformat(iso);
    return hc::Day{std::chrono::year{date.year},
                   std::chrono::month{static_cast<unsigned>(date.month)},
                   std::chrono::day{static_cast<unsigned>(date.day)}};
}

// Sets the current palette for one case and puts the previous one back.
class ScopedPalette {
public:
    explicit ScopedPalette(const std::string& theme) : previous_(fv::ui::currentPalette()) {
        fv::ui::setCurrentPalette(fv::ui::paletteFor(theme));
    }
    ~ScopedPalette() { fv::ui::setCurrentPalette(previous_); }
    ScopedPalette(const ScopedPalette&) = delete;
    ScopedPalette& operator=(const ScopedPalette&) = delete;

private:
    const fv::ui::Palette& previous_;
};

} // namespace

TEST_CASE("golden history_calendar plural", "[golden]") {
    golden::for_each(kModule, "plural", [](const golden::Case& c) {
        const std::optional<std::string> many = c.arg<std::optional<std::string>>(2);
        CHECK(cv::plural(c.arg<std::int64_t>(0), c.arg<std::string>(1),
                         many ? std::optional<std::string_view>(*many) : std::nullopt) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden history_calendar kind_breakdown", "[golden]") {
    golden::for_each(kModule, "kind_breakdown", [](const golden::Case& c) {
        CHECK(cv::kind_breakdown(counts(c.arg<fv::Json>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden history_calendar clock", "[golden]") {
    golden::for_each(kModule, "clock", [](const golden::Case& c) {
        CHECK(cv::clock(fv::py::fromisoformat(c.arg<std::string>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden history_calendar pick_lines", "[golden]") {
    golden::for_each(kModule, "pick_lines", [](const golden::Case& c) {
        const fv::Json pairs = c.arg<fv::Json>(0);
        std::vector<std::string> ids;
        std::vector<std::string> kindNames;
        for (const fv::Json& pair : pairs) {
            ids.push_back(pair.at(0).get<std::string>());
            kindNames.push_back(pair.at(1).get<std::string>());
        }
        const std::vector<std::string_view> kinds(kindNames.begin(), kindNames.end());
        std::vector<std::string> picked;
        for (const std::size_t index : cv::pick_line_indexes(kinds, c.arg<std::int64_t>(1))) {
            picked.push_back(ids[index]);
        }
        CHECK(picked == c.result<std::vector<std::string>>());
    });
}

TEST_CASE("golden history_calendar day_tooltip", "[golden]") {
    golden::for_each(kModule, "day_tooltip", [](const golden::Case& c) {
        const fv::Json summary = c.arg<fv::Json>(1);
        std::optional<hc::DaySummary> made;
        if (!summary.is_null()) {
            made = hc::DaySummary{summary.at(0).get<std::int64_t>(), summary.at(1).get<double>(),
                                  counts(summary.at(2))};
        }
        CHECK(cv::day_tooltip(dayFrom(c.arg<std::string>(0)), made ? &*made : nullptr,
                              c.arg<std::optional<std::int64_t>>(2)) == c.result<std::string>());
    });
}

TEST_CASE("golden history_calendar _heat", "[golden]") {
    golden::for_each(kModule, "_heat", [](const golden::Case& c) {
        const ScopedPalette palette(c.arg<std::string>(2));
        const QColor color = cv::heat(c.arg<std::int64_t>(0), c.arg<std::int64_t>(1));
        CHECK(color.name(QColor::HexArgb).toStdString() == c.result<std::string>());
    });
}
