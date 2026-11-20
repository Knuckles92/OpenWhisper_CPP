// Golden vectors of services.history_export, services.history_calendar and
// services.dictation_stats.load_summary (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "golden/golden.h"
#include "history/calendar.h"
#include "history/calendar_source.h"
#include "history/dictation_stats.h"
#include "history/export.h"
#include "history/stats_summary.h"
#include "history_test_support.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "testkit/testkit.h"

namespace golden = fv::golden;
namespace history = fv::history;
namespace stats = fv::dictation_stats;
using fv::Json;

namespace {

constexpr std::string_view kPacific = "America/Los_Angeles";

// The vectors were made on a machine in Pacific time (pins.local_timezone).
const fv::FakeClock& pacific_clock() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), kPacific);
    return clock;
}

// render_export_document's pinned now: 2026-03-15 14:30:05 Pacific daylight time.
const fv::FakeClock& export_clock() {
    static const fv::FakeClock clock(std::chrono::sys_days{std::chrono::year{2026} / 3 / 15} +
                                         std::chrono::hours{21} + std::chrono::minutes{30} +
                                         std::chrono::seconds{5},
                                     kPacific);
    return clock;
}

// The zone of an hour offset (timezone(timedelta(hours=offset))), or Pacific for None.
std::string zone_name(const Json& offset) {
    if (offset.is_null()) {
        return std::string(kPacific);
    }
    const int hours = offset.get<int>();
    if (hours == 0) {
        return "Etc/UTC";
    }
    return hours > 0 ? std::format("Etc/GMT-{}", hours) : std::format("Etc/GMT+{}", -hours);
}

const std::chrono::time_zone& zone_of(const Json& offset) {
    return *std::chrono::locate_zone(zone_name(offset));
}

template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

std::optional<std::string> text_field(const Json& fields, const char* name) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->is_null()) {
        return std::nullopt;
    }
    return found->get<std::string>();
}

std::optional<double> real_field(const Json& fields, const char* name) {
    const auto found = fields.find(name);
    if (found == fields.end() || found->is_null()) {
        return std::nullopt;
    }
    return golden::decode<double>(*found);
}

// A history row from a dict of its attributes (missing ones are None).
fv::HistoryEntry entry_from(const Json& fields) {
    fv::HistoryEntry entry;
    entry.id = text_field(fields, "id").value_or("");
    entry.text = text_field(fields, "text").value_or("");
    entry.raw_text = text_field(fields, "raw_text");
    entry.timestamp = text_field(fields, "timestamp").value_or("");
    entry.model = text_field(fields, "model").value_or("");
    entry.audio_file = text_field(fields, "audio_file");
    entry.transcription_time = real_field(fields, "transcription_time");
    entry.audio_duration = real_field(fields, "audio_duration");
    if (const auto size = fields.find("file_size"); size != fields.end() && !size->is_null()) {
        entry.file_size = size->get<std::int64_t>();
    }
    entry.cleanup_provider = text_field(fields, "cleanup_provider");
    entry.cleanup_model = text_field(fields, "cleanup_model");
    entry.source_name = text_field(fields, "source_name");
    entry.title = text_field(fields, "title");
    entry.origin_device_id = text_field(fields, "origin_device_id");
    entry.origin_device_name = text_field(fields, "origin_device_name");
    entry.app_id = text_field(fields, "app_id");
    entry.app_name = text_field(fields, "app_name");
    entry.app_category = text_field(fields, "app_category");
    entry.cleanup_level = text_field(fields, "cleanup_level");
    entry.entry_kind = text_field(fields, "entry_kind");
    entry.cleaned_text = text_field(fields, "cleaned_text");
    entry.language = text_field(fields, "language");
    entry.stored_on = text_field(fields, "stored_on");
    return entry;
}

Json serialized(const Json& fields) {
    return history::serialize_history_entry(entry_from(fields), pacific_clock());
}

history::ExportOptions toggles(const golden::Case& c, std::size_t first) {
    return {.include_cleaned = c.arg<bool>(first), .include_raw = c.arg<bool>(first + 1)};
}

std::optional<fv::py::DateTime> bound(const Json& text) {
    if (text.is_null()) {
        return std::nullopt;
    }
    return fv::py::fromisoformat(text.get<std::string>());
}

std::string iso(history::Day day) {
    return std::format("{:04}-{:02}-{:02}", static_cast<int>(day.year()),
                       static_cast<unsigned>(day.month()), static_cast<unsigned>(day.day()));
}

history::Day day_from(std::string_view text) {
    const fv::py::Date date = fv::py::date_fromisoformat(text);
    return history::Day{std::chrono::year{date.year},
                        std::chrono::month{static_cast<unsigned>(date.month)},
                        std::chrono::day{static_cast<unsigned>(date.day)}};
}

Json place_json(const history::Place& place) {
    return Json::array({place.scope, place.name});
}

std::optional<history::Place> place_from(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    return history::Place{value[0].get<std::string>(), value[1].get<std::string>()};
}

Json location_json(const history::CalendarItem& item) {
    const auto [chip, tooltip] = item.location();
    return Json::array({chip, tooltip});
}

std::optional<history::KindSet> kinds_from(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    history::KindSet kinds;
    for (const Json& kind : value) {
        kinds.insert(kind.get<std::string>());
    }
    return kinds;
}

Json by_kind_json(const std::map<std::string, std::int64_t, std::less<>>& by_kind) {
    Json result = Json::object();
    for (const auto& [kind, count] : by_kind) {
        result[kind] = count;
    }
    return result;
}

// The case file's _index.
Json index_json(const golden::Case& c) {
    const auto& zone = zone_of(c.arg<Json>(1));
    std::vector<history::Mark> marks;
    for (const Json& record : c.arg<Json>(0)) {
        const auto stamp = record[1].is_null()
                               ? std::nullopt
                               : std::optional<std::string>(record[1].get<std::string>());
        const auto when = history::local_wall_time(
            stamp ? std::optional<std::string_view>(*stamp) : std::nullopt, zone);
        if (!when) {
            continue;
        }
        const auto source = record[2].is_null()
                                ? std::nullopt
                                : std::optional<std::string>(record[2].get<std::string>());
        const auto kind_text = record[3].is_null()
                                   ? std::nullopt
                                   : std::optional<std::string>(record[3].get<std::string>());
        auto [kind, _label] = history::classify_source(source, kind_text);
        const std::string origin = record[4].is_null() ? "" : record[4].get<std::string>();
        marks.push_back(
            {*when, kind,
             origin.empty() ? history::here() : history::Place{std::string(history::FROM), origin},
             record[0].get<std::string>(),
             record[5].is_null() ? 0.0 : golden::decode<double>(record[5])});
    }
    const history::CalendarIndex index(marks);
    struct Filter {
        std::optional<history::KindSet> kinds;
        std::optional<history::Place> place;
    };
    std::vector<Filter> filters = {{}};
    for (const Json& query : c.arg<Json>(4)) {
        filters.push_back({kinds_from(query["kinds"]), place_from(query["place"])});
    }

    Json result = Json::object();
    Json marks_json = Json::array();
    Json milestones = Json::object();
    for (const auto& mark : index.marks()) {
        marks_json.push_back(Json::array({mark.record_id, mark.when.isoformat(), mark.kind,
                                          place_json(mark.place), mark.seconds}));
        if (const auto ordinal = index.milestone(mark.record_id)) {
            milestones[mark.record_id] = *ordinal;
        }
    }
    result["marks"] = marks_json;
    Json places = Json::array();
    for (const auto& place : index.places()) {
        places.push_back(place_json(place));
    }
    result["places"] = places;
    result["streak"] = index.streak(day_from(c.arg<std::string>(2)));
    result["milestones"] = milestones;
    Json active = Json::array();
    for (const auto& day : index.active_days()) {
        active.push_back(iso(day));
    }
    result["active_days"] = active;
    Json selected = Json::array();
    Json month_counts = Json::array();
    for (const auto& filter : filters) {
        Json ids = Json::array();
        for (const auto& mark : index.select(filter.kinds, filter.place)) {
            ids.push_back(mark.record_id);
        }
        selected.push_back(ids);
        Json counts = Json::array();
        for (const auto& [key, count] : index.month_counts(filter.kinds, filter.place)) {
            counts.push_back(Json::array({key.year, key.month, count}));
        }
        month_counts.push_back(counts);
    }
    result["selected"] = selected;
    result["month_counts"] = month_counts;
    Json months = Json::object();
    for (const Json& key : c.arg<Json>(3)) {
        const int year = key[0].get<int>();
        const int month = key[1].get<int>();
        Json filtered = Json::array();
        for (const auto& filter : filters) {
            Json days = Json::object();
            for (const auto& [day, summary] : index.days(year, month, filter.kinds, filter.place)) {
                days[iso(day)] =
                    Json::array({summary.count, summary.seconds, by_kind_json(summary.by_kind)});
            }
            const auto month_stats = index.month_stats(year, month, filter.kinds, filter.place);
            filtered.push_back({
                {"days", days},
                {"count", month_stats.count},
                {"seconds", month_stats.seconds},
                {"active_days", month_stats.active_days},
                {"busiest", month_stats.busiest ? Json::array({iso(month_stats.busiest->first),
                                                               month_stats.busiest->second})
                                                : Json(nullptr)},
                {"by_kind", by_kind_json(month_stats.by_kind)},
            });
        }
        Json milestone_days = Json::object();
        for (const auto& [day, ordinal] : index.milestone_days(year, month)) {
            milestone_days[iso(day)] = ordinal;
        }
        months[std::format("{:04}-{:02}", year, month)] = {{"filtered", filtered},
                                                           {"milestone_days", milestone_days}};
    }
    result["months"] = months;
    Json nearest = Json::array();
    for (const Json& query : c.arg<Json>(4)) {
        for (const Json& key : query["keys"]) {
            const auto found =
                index.nearest_month({key[0].get<int>(), key[1].get<int>()},
                                    kinds_from(query["kinds"]), place_from(query["place"]));
            nearest.push_back(found ? Json::array({found->year, found->month}) : Json(nullptr));
        }
    }
    result["nearest"] = nearest;
    return result;
}

Json pairs_json(const std::vector<std::pair<std::string, std::int64_t>>& items) {
    Json result = Json::array();
    for (const auto& [key, value] : items) {
        result.push_back(Json::array({key, value}));
    }
    return result;
}

Json bars_json(const std::vector<std::pair<history::Day, std::int64_t>>& items) {
    Json result = Json::array();
    for (const auto& [day, value] : items) {
        result.push_back(Json::array({iso(day), value}));
    }
    return result;
}

template <class Array> Json array_json(const Array& values) {
    Json result = Json::array();
    for (const auto& value : values) {
        result.push_back(value);
    }
    return result;
}

Json optional_json(const std::optional<std::int64_t>& value) {
    return value ? Json(*value) : Json(nullptr);
}

// The case file's _period_dict.
Json period_json(const stats::PeriodStats& period) {
    Json grid = Json::array();
    for (const auto& day : period.grid) {
        grid.push_back(array_json(day));
    }
    Json features = Json::array();
    for (const auto& feature : period.features) {
        features.push_back({{"key", feature.key},
                            {"count", feature.count},
                            {"seconds", feature.seconds},
                            {"words", feature.words},
                            {"shots", feature.shots},
                            {"top", pairs_json(feature.top)},
                            {"trend", array_json(feature.trend)}});
    }
    return {{"key", period.key},
            {"days", period.days ? Json(*period.days) : Json(nullptr)},
            {"words", period.words},
            {"previous_words", optional_json(period.previous_words)},
            {"dictations", period.dictations},
            {"rewrites", period.rewrites},
            {"average_wpm", period.average_wpm},
            {"words_cleaned_up", period.words_cleaned_up},
            {"top_apps", pairs_json(period.top_apps)},
            {"bars", bars_json(period.bars)},
            {"hours", array_json(period.hours)},
            {"weekdays", array_json(period.weekdays)},
            {"grid", grid},
            {"categories", pairs_json(period.categories)},
            {"cleanup_levels", pairs_json(period.cleanup_levels)},
            {"languages", pairs_json(period.languages)},
            {"minutes_saved", period.minutes_saved()},
            {"features", features}};
}

stats::EntryFields stat_fields(const Json& given) {
    stats::EntryFields fields;
    fields.id = text_field(given, "id");
    fields.timestamp = text_field(given, "timestamp");
    fields.text = text_field(given, "text");
    fields.raw_text = text_field(given, "raw_text");
    fields.cleaned_text = text_field(given, "cleaned_text");
    fields.audio_duration = real_field(given, "audio_duration");
    fields.entry_kind = text_field(given, "entry_kind");
    fields.source_name = text_field(given, "source_name");
    fields.app_name = text_field(given, "app_name");
    fields.app_category = text_field(given, "app_category");
    fields.cleanup_level = text_field(given, "cleanup_level");
    fields.language = text_field(given, "language");
    fields.origin_device_id = text_field(given, "origin_device_id");
    fields.spoken_text = text_field(given, "spoken_text");
    if (const auto counts = given.find("stats_counts"); counts != given.end()) {
        fields.stats_counts = *counts;
    }
    return fields;
}

// The case file's _load_summary.
Json summary_json(const golden::Case& c) {
    fv::history_test::TestDatabase db;
    stats::StatsRepository repository(db.database());
    repository.reset();
    for (const Json& fields : c.arg<Json>(0)) {
        repository.record(stat_fields(fields), nullptr);
    }
    const fv::FakeClock clock(fv::FakeClock::default_utc(), zone_name(c.arg<Json>(2)));
    const auto summary = stats::load_summary(repository, clock, day_from(c.arg<std::string>(1)));
    Json words = Json::object();
    for (const auto& [day, count] : summary.activity) {
        if (count != 0) {
            words[iso(day)] = count;
        }
    }
    Json periods = Json::array();
    for (const auto& period : summary.periods) {
        periods.push_back(period_json(period));
    }
    return {
        {"words_all_time", summary.words_all_time},
        {"entries", summary.entries},
        {"day_streak", summary.day_streak},
        {"longest_streak", summary.longest_streak},
        {"dictated_today", summary.dictated_today},
        {"first_day", summary.first_day ? Json(iso(*summary.first_day)) : Json(nullptr)},
        {"activity",
         {{"length", summary.activity.size()},
          {"first",
           summary.activity.empty() ? Json(nullptr) : Json(iso(summary.activity.front().first))},
          {"last",
           summary.activity.empty() ? Json(nullptr) : Json(iso(summary.activity.back().first))},
          {"words", words}}},
        {"periods", periods},
    };
}

} // namespace

TEST_CASE("golden services.history_export", "[golden]") {
    golden::for_each("services.history_export", "serialize_history_entry",
                     [](const golden::Case& c) {
                         CHECK(golden::equal(c.result<Json>(), serialized(c.arg<Json>(0))));
                     });
    golden::for_each("services.history_export", "render_entry_document", [](const golden::Case& c) {
        CHECK(history::render_entry_document(serialized(c.arg<Json>(0)), c.arg<std::string>(1),
                                             toggles(c, 2)) == c.result<std::string>());
    });
    golden::for_each(
        "services.history_export", "render_export_document", [](const golden::Case& c) {
            std::vector<Json> entries;
            for (const Json& row : c.arg<Json>(0)) {
                entries.push_back(serialized(row));
            }
            CHECK(history::render_export_document(entries, c.arg<std::string>(1), toggles(c, 2),
                                                  export_clock()) == c.result<std::string>());
        });
    golden::for_each("services.history_export", "entry_file_stem", [](const golden::Case& c) {
        const Json entry = c.arg<Json>(0);
        if (c.raises()) {
            CHECK(raised([&] { history::entry_file_stem(entry, pacific_clock()); }) ==
                  c.raises_type());
        } else {
            CHECK(history::entry_file_stem(entry, pacific_clock()) == c.result<std::string>());
        }
    });
    golden::for_each("services.history_export", "filter_export_entries", [](const golden::Case& c) {
        const auto entries = c.arg<std::vector<Json>>(0);
        std::vector<std::string> ids;
        for (const Json& kept :
             history::filter_export_entries(entries, bound(c.arg<Json>(1)), bound(c.arg<Json>(2)),
                                            c.arg<bool>(3), pacific_clock())) {
            ids.push_back(kept.at("id").get<std::string>());
        }
        CHECK(ids == c.result<std::vector<std::string>>());
    });
    golden::for_each("services.history_export", "write_per_entry_files", [](const golden::Case& c) {
        fv::testkit::TempDataRoot folder;
        for (const auto& name : c.arg<std::vector<std::string>>(2)) {
            fv::history_test::write_file(folder.path() / fv::path_from_utf8(name), "keep me");
        }
        std::vector<Json> entries;
        for (const Json& row : c.arg<Json>(0)) {
            entries.push_back(serialized(row));
        }
        const auto written = history::write_per_entry_files(entries, c.arg<std::string>(1),
                                                            folder.path(), {}, pacific_clock());
        Json names = Json::array();
        for (const auto& path : written) {
            CHECK(path.parent_path() == folder.path());
            names.push_back(fv::path_to_utf8(path.filename()));
        }
        Json files = Json::object();
        for (const auto& name : fv::history_test::list_names(folder.path())) {
            files[name] = fv::history_test::read_file(folder.path() / fv::path_from_utf8(name));
        }
        Json expected = c.result<Json>();
#ifndef _WIN32
        // The vectors were written on Windows, whose text mode ends lines with
        // CRLF.
        for (auto& [name, text] : expected["files"].items()) {
            std::string content = text.get<std::string>();
            for (auto at = content.find("\r\n"); at != std::string::npos;
                 at = content.find("\r\n", at)) {
                content.erase(at, 1);
            }
            text = content;
        }
#endif
        CHECK(golden::equal(expected["written"], names));
        CHECK(golden::equal(expected["files"], files));
    });
    golden::for_each("services.history_export", "_cleanup_label", [](const golden::Case& c) {
        CHECK(history::cleanup_label(c.arg<Json>(0)) == c.result<std::string>());
    });
}

TEST_CASE("golden services.history_calendar", "[golden]") {
    const std::string module = "services.history_calendar";
    golden::for_each(module, "local_wall_time", [](const golden::Case& c) {
        const auto value = c.arg<std::optional<std::string>>(0);
        const auto& zone = zone_of(c.arg<Json>(1));
        const auto call = [&] {
            return history::local_wall_time(
                value ? std::optional<std::string_view>(*value) : std::nullopt, zone);
        };
        if (c.raises()) {
            CHECK(raised(call) == c.raises_type());
            return;
        }
        const auto moment = call();
        CHECK((moment ? std::optional<std::string>(moment->isoformat()) : std::nullopt) ==
              c.result<std::optional<std::string>>());
    });
    golden::for_each(module, "classify_source", [](const golden::Case& c) {
        const auto source = c.arg<std::optional<std::string>>(0);
        const auto kind = c.arg<std::optional<std::string>>(1);
        const auto [found_kind, label] = history::classify_source(source, kind);
        CHECK(Json::array({found_kind, label}) == c.result<Json>());
    });
    golden::for_each(module, "record_place", [](const golden::Case& c) {
        CHECK(place_json(history::record_place(c.arg<Json>(0))) == c.result<Json>());
    });
    golden::for_each(module, "place_label", [](const golden::Case& c) {
        CHECK(history::place_label(place_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each(module, "is_milestone", [](const golden::Case& c) {
        CHECK(history::is_milestone(c.arg<std::int64_t>(0)) == c.result<bool>());
    });
    golden::for_each(module, "ordinal_suffix", [](const golden::Case& c) {
        CHECK(history::ordinal_suffix(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "milestone_text", [](const golden::Case& c) {
        CHECK(history::milestone_text(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
    golden::for_each(module, "shift_month", [](const golden::Case& c) {
        const Json key = c.arg<Json>(0);
        const auto shifted =
            history::shift_month({key[0].get<int>(), key[1].get<int>()}, c.arg<std::int64_t>(1));
        CHECK(Json::array({shifted.year, shifted.month}) == c.result<Json>());
    });
    golden::for_each(module, "month_distance", [](const golden::Case& c) {
        const Json a = c.arg<Json>(0);
        const Json b = c.arg<Json>(1);
        CHECK(history::month_distance({a[0].get<int>(), a[1].get<int>()},
                                      {b[0].get<int>(), b[1].get<int>()}) ==
              c.result<std::int64_t>());
    });
    golden::for_each(module, "month_key", [](const golden::Case& c) {
        const auto key = history::month_key(day_from(c.arg<std::string>(0)));
        CHECK(Json::array({key.year, key.month}) == c.result<Json>());
    });
    golden::for_each(module, "month_weeks", [](const golden::Case& c) {
        const auto call = [&] {
            return history::month_weeks(c.arg<int>(0), c.arg<int>(1), c.arg<int>(2));
        };
        if (c.raises()) {
            CHECK(raised(call) == c.raises_type());
            return;
        }
        Json weeks = Json::array();
        for (const auto& week : call()) {
            Json days = Json::array();
            for (const auto& day : week) {
                days.push_back(iso(day));
            }
            weeks.push_back(days);
        }
        CHECK(weeks == c.result<Json>());
    });
    golden::for_each(module, "query_bounds", [](const golden::Case& c) {
        const auto call = [&] {
            return history::query_bounds(c.arg<int>(0), c.arg<int>(1));
        };
        if (c.raises()) {
            CHECK(raised(call) == c.raises_type());
            return;
        }
        const auto [start, end] = call();
        CHECK(Json::array({start, end}) == c.result<Json>());
    });
    golden::for_each(module, "entry_item", [](const golden::Case& c) {
        const auto when = fv::py::fromisoformat(c.arg<std::string>(1));
        const auto item =
            history::entry_item(entry_from(c.arg<Json>(0)), when, c.arg<std::string>(2));
        CHECK(item.day() == history::day_of(when));
        const Json actual =
            Json::array({item.kind, item.record_id, item.title, item.preview, item.seconds,
                         place_json(item.place), item.label(), location_json(item)});
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each(module, "calendar_item", [](const golden::Case& c) {
        history::CalendarItem item;
        item.kind = c.arg<std::string>(0);
        item.title = c.arg<std::string>(1);
        item.preview = c.arg<std::string>(2);
        item.place = *place_from(c.arg<Json>(3));
        item.also_on = c.arg<std::string>(4);
        CHECK(Json::array({item.label(), location_json(item)}) == c.result<Json>());
    });
    golden::for_each(module, "index", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), index_json(c)));
    });
}

TEST_CASE("golden services.dictation_stats load_summary", "[golden]") {
    golden::for_each("services.dictation_stats", "load_summary", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(), summary_json(c)));
    });
}
