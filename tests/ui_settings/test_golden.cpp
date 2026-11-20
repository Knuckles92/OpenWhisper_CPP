// Golden vectors of ui_qt.dialogs.settings_backup (_local_date, _date_label) and
// ui_qt.widgets.backup_calendar (_month_counts, is_planned, events);
// tools/parity/cases/<module>.py.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "history/calendar.h"
#include "pyport/datetime.h"
#include "ui_settings/backup_calendar.h"
#include "ui_settings/settings_backup.h"

namespace golden = fv::golden;
using fv::Json;
using fv::history::Day;

namespace {

// The vectors were made on a machine in Pacific time (pins.local_timezone).
const fv::FakeClock& pacific() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/Los_Angeles");
    return clock;
}

Day day_of(const std::string& iso) {
    const fv::py::Date date = fv::py::date_fromisoformat(iso);
    return Day{std::chrono::year{date.year} /
               std::chrono::month{static_cast<unsigned>(date.month)} /
               std::chrono::day{static_cast<unsigned>(date.day)}};
}

std::optional<Day> optional_day(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return day_of(value.get<std::string>());
}

} // namespace

TEST_CASE("golden ui_qt.dialogs.settings_backup _local_date", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_backup", "_local_date", [](const golden::Case& c) {
        const auto local = fv::backup_page::local_date(c.arg<Json>(0), pacific());
        const auto expected = c.result<std::optional<std::string>>();
        REQUIRE(local.has_value() == expected.has_value());
        if (expected) {
            CHECK(local->isoformat() == *expected);
        }
    });
}

TEST_CASE("golden ui_qt.dialogs.settings_backup _date_label", "[golden]") {
    golden::for_each("ui_qt.dialogs.settings_backup", "_date_label", [](const golden::Case& c) {
        CHECK(fv::backup_page::date_label(c.arg<Json>(0), pacific()) == c.result<std::string>());
    });
}

TEST_CASE("golden ui_qt.widgets.backup_calendar _month_counts", "[golden]") {
    golden::for_each("ui_qt.widgets.backup_calendar", "_month_counts", [](const golden::Case& c) {
        const auto counts = fv::backup_calendar::month_counts(
            optional_day(c.arg<Json>(0)), optional_day(c.arg<Json>(1)), c.arg<int>(2),
            *optional_day(c.arg<Json>(3)));
        std::vector<std::vector<int>> flat;
        for (const auto& [key, count] : counts) {
            flat.push_back({key.year, key.month, static_cast<int>(count)});
        }
        CHECK(flat == c.result<std::vector<std::vector<int>>>());
    });
}

TEST_CASE("golden ui_qt.widgets.backup_calendar is_planned", "[golden]") {
    golden::for_each("ui_qt.widgets.backup_calendar", "is_planned", [](const golden::Case& c) {
        CHECK(fv::backup_calendar::is_planned(*optional_day(c.arg<Json>(0)),
                                              optional_day(c.arg<Json>(1)),
                                              c.arg<int>(2)) == c.result<bool>());
    });
}

TEST_CASE("golden ui_qt.widgets.backup_calendar events", "[golden]") {
    golden::for_each("ui_qt.widgets.backup_calendar", "events", [](const golden::Case& c) {
        fv::BackupMonthGrid grid(pacific());
        grid.setDates(optional_day(c.arg<Json>(1)), optional_day(c.arg<Json>(2)), c.arg<int>(3));
        std::vector<std::vector<std::string>> found;
        for (const auto& [short_label, description] : grid.events(*optional_day(c.arg<Json>(0)))) {
            found.push_back({std::string(short_label), std::string(description)});
        }
        CHECK(found == c.result<std::vector<std::vector<std::string>>>());
    });
}
