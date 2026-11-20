// The Stats window's words and chart levels against OpenWhisper's helpers
// (tests/golden/ui_qt.dialogs.stats_dialog.json).
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "golden/golden.h"
#include "history/stats_summary.h"
#include "pyport/datetime.h"
#include "ui_history/stats_charts.h"
#include "ui_history/stats_dialog.h"

namespace golden = fv::golden;
namespace ds = fv::dictation_stats;
namespace sv = fv::stats_view;

namespace {

constexpr char kModule[] = "ui_qt.dialogs.stats_dialog";

ds::Day dayFrom(const std::string& iso) {
    const fv::py::Date date = fv::py::date_fromisoformat(iso);
    return ds::Day{std::chrono::year{date.year},
                   std::chrono::month{static_cast<unsigned>(date.month)},
                   std::chrono::day{static_cast<unsigned>(date.day)}};
}

fv::Json pair(const std::pair<std::string, std::string>& made) {
    return fv::Json::array({made.first, made.second});
}

} // namespace

TEST_CASE("golden stats_dialog _days", "[golden]") {
    golden::for_each(kModule, "_days", [](const golden::Case& c) {
        CHECK(sv::days(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_dialog _count", "[golden]") {
    golden::for_each(kModule, "_count", [](const golden::Case& c) {
        const std::string one = c.arg<std::string>(1);
        const std::string many = c.arg_count() > 2 ? c.arg<std::string>(2) : std::string();
        CHECK(sv::count(c.arg<std::int64_t>(0), one, many) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_dialog _duration", "[golden]") {
    golden::for_each(kModule, "_duration", [](const golden::Case& c) {
        CHECK(sv::duration(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_dialog _short_day and _long_day", "[golden]") {
    golden::for_each(kModule, "_short_day", [](const golden::Case& c) {
        CHECK(sv::short_day(dayFrom(c.arg<std::string>(0))) == c.result<std::string>());
    });
    golden::for_each(kModule, "_long_day", [](const golden::Case& c) {
        CHECK(sv::long_day(dayFrom(c.arg<std::string>(0))) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_dialog _trend", "[golden]") {
    golden::for_each(kModule, "_trend", [](const golden::Case& c) {
        ds::PeriodStats period;
        period.key = c.arg<std::string>(0);
        period.days = c.arg<std::optional<int>>(1);
        period.words = c.arg<std::int64_t>(2);
        period.previous_words = c.arg<std::optional<std::int64_t>>(3);
        std::optional<ds::Day> first;
        if (const std::optional<std::string> iso = c.arg<std::optional<std::string>>(4)) {
            first = dayFrom(*iso);
        }
        CHECK(pair(sv::trend(period, first)) == c.result<fv::Json>());
    });
}

TEST_CASE("golden stats_dialog _streak_detail", "[golden]") {
    golden::for_each(kModule, "_streak_detail", [](const golden::Case& c) {
        ds::StatsSummary summary;
        summary.dictated_today = c.arg<bool>(0);
        summary.day_streak = c.arg<std::int64_t>(1);
        summary.longest_streak = c.arg<std::int64_t>(2);
        CHECK(sv::streak_detail(summary) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_dialog _feature_text", "[golden]") {
    golden::for_each(kModule, "_feature_text", [](const golden::Case& c) {
        ds::FeatureStats feature;
        feature.key = c.arg<std::string>(0);
        feature.count = c.arg<std::int64_t>(1);
        feature.seconds = c.arg<double>(2);
        feature.words = c.arg<std::int64_t>(3);
        feature.shots = c.arg<std::int64_t>(4);
        for (const fv::Json& top : c.arg<fv::Json>(5)) {
            feature.top.emplace_back(top.at(0).get<std::string>(), top.at(1).get<std::int64_t>());
        }
        CHECK(pair(sv::feature_text(feature)) == c.result<fv::Json>());
    });
}

TEST_CASE("golden stats_dialog _TREND_TEXT", "[golden]") {
    golden::for_each(kModule, "trend_text", [](const golden::Case& c) {
        CHECK(sv::trend_text(c.arg<std::string>(0), c.arg<std::int64_t>(1)) ==
              c.result<std::string>());
    });
}

TEST_CASE("golden stats_charts words_text and hour_text", "[golden]") {
    golden::for_each(kModule, "words_text", [](const golden::Case& c) {
        CHECK(sv::words_text(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
    golden::for_each(kModule, "hour_text", [](const golden::Case& c) {
        CHECK(sv::hour_text(c.arg<int>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden stats_charts level", "[golden]") {
    golden::for_each(kModule, "level", [](const golden::Case& c) {
        std::vector<std::int64_t> words;
        for (const fv::Json& day : c.arg<fv::Json>(0)) {
            words.push_back(day.at(1).get<std::int64_t>());
        }
        CHECK(sv::level(sv::quartile_thresholds(words), c.arg<std::int64_t>(1)) == c.result<int>());
    });
}

TEST_CASE("golden stats_charts HourGrid peak", "[golden]") {
    golden::for_each(kModule, "peak", [](const golden::Case& c) {
        sv::HourWords grid{};
        const fv::Json rows = c.arg<fv::Json>(0);
        for (std::size_t weekday = 0; weekday < 7; ++weekday) {
            for (std::size_t hour = 0; hour < 24; ++hour) {
                grid[weekday][hour] = rows.at(weekday).at(hour).get<std::int64_t>();
            }
        }
        const std::optional<std::pair<int, int>> peak = sv::grid_peak(grid);
        const fv::Json made =
            peak ? fv::Json::array({peak->first, peak->second}) : fv::Json(nullptr);
        CHECK(made == c.result<fv::Json>());
    });
}
