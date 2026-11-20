// Golden vectors of services.format_utils, services.titles, services.models,
// services.history_manager and services.dictation_stats (tools/parity/cases/<module>.py).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "history/dictation_stats.h"
#include "history/entry.h"
#include "history/format_utils.h"
#include "history/manager.h"
#include "history/repository.h"
#include "history/titles.h"
#include "history_test_support.h"
#include "pyport/errors.h"

namespace golden = fv::golden;
using fv::Json;

namespace {

// The vectors were made on a machine in Pacific time (pins.local_timezone).
const fv::FakeClock& pacific_clock() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/Los_Angeles");
    return clock;
}

// The Python exception's class name of a pyport error, "" for any other exception.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        fn();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

// An argument as the Python value it stands for: a tagged float ({"$float": "nan"}) as a number.
Json untagged(const Json& value) {
    if (value.is_object() && value.contains("$float")) {
        return golden::decode<double>(value);
    }
    return value;
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

// An entry from a dict of TranscriptionHistory columns (missing ones are None).
fv::HistoryEntry entry_from(const Json& fields) {
    fv::HistoryEntry entry;
    entry.id = text_field(fields, "id").value_or("");
    entry.text = text_field(fields, "text").value_or("");
    entry.raw_text = text_field(fields, "raw_text");
    entry.timestamp = text_field(fields, "timestamp").value_or("");
    entry.model = text_field(fields, "model").value_or("");
    entry.audio_file = text_field(fields, "audio_file");
    entry.audio_duration = real_field(fields, "audio_duration");
    entry.cleanup_provider = text_field(fields, "cleanup_provider");
    entry.cleanup_model = text_field(fields, "cleanup_model");
    entry.source_name = text_field(fields, "source_name");
    entry.title = text_field(fields, "title");
    entry.origin_device_id = text_field(fields, "origin_device_id");
    entry.origin_device_name = text_field(fields, "origin_device_name");
    entry.app_name = text_field(fields, "app_name");
    entry.entry_kind = text_field(fields, "entry_kind");
    entry.cleaned_text = text_field(fields, "cleaned_text");
    entry.stored_on = text_field(fields, "stored_on");
    return entry;
}

Json optional_json(const std::optional<std::string>& value) {
    return value ? Json(*value) : Json(nullptr);
}

Json stored_json(const std::optional<fv::HistoryEntry>& entry) {
    if (!entry) {
        return nullptr;
    }
    return Json::array(
        {entry->text, optional_json(entry->raw_text), optional_json(entry->cleaned_text)});
}

// The chrono zone of timezone(timedelta(hours=offset)): Etc/GMT names count the other way.
const std::chrono::time_zone& fixed_zone(int offset_hours) {
    if (offset_hours == 0) {
        return *std::chrono::locate_zone("Etc/UTC");
    }
    const std::string name = offset_hours > 0 ? "Etc/GMT-" + std::to_string(offset_hours)
                                              : "Etc/GMT+" + std::to_string(-offset_hours);
    return *std::chrono::locate_zone(name);
}

} // namespace

TEST_CASE("golden services.format_utils", "[golden]") {
    const auto& clock = pacific_clock();
    const auto audio_duration = [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(raised([&] { fv::format_audio_duration(untagged(c.arg<Json>(0))); }) ==
                  c.raises_type());
        } else {
            CHECK(fv::format_audio_duration(untagged(c.arg<Json>(0))) == c.result<std::string>());
        }
    };
    golden::for_each("services.format_utils", "format_audio_duration", audio_duration);
    // format_audio_duration again, on the inputs kept out of the plain list (the case file says
    // why).
    golden::for_each("services.format_utils", "format_audio_duration_edge_cases", audio_duration);
    golden::for_each("services.format_utils", "format_short_duration", [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(raised([&] { fv::format_short_duration(untagged(c.arg<Json>(0))); }) ==
                  c.raises_type());
        } else {
            CHECK(fv::format_short_duration(untagged(c.arg<Json>(0))) == c.result<std::string>());
        }
    });
    golden::for_each("services.format_utils", "format_size_bytes", [](const golden::Case& c) {
        CHECK(fv::format_size_bytes(c.arg<std::int64_t>(0)) == c.result<std::string>());
    });
    golden::for_each("services.format_utils", "format_sample_rate", [](const golden::Case& c) {
        CHECK(fv::format_sample_rate(c.arg<Json>(0)) == c.result<std::string>());
    });
    golden::for_each("services.format_utils", "format_file_size", [](const golden::Case& c) {
        CHECK(fv::format_file_size(c.arg<double>(0)) == c.result<std::string>());
    });
    golden::for_each("services.format_utils", "format_timestamp", [&](const golden::Case& c) {
        CHECK(fv::format_timestamp(c.arg<std::optional<std::string>>(0), clock) ==
              c.result<std::optional<std::string>>());
    });
}

TEST_CASE("golden services.titles", "[golden]") {
    golden::for_each("services.titles", "title_error", [](const golden::Case& c) {
        CHECK(fv::title_error(c.arg<Json>(0)) == c.result<std::optional<std::string>>());
    });
    golden::for_each("services.titles", "normalize_title", [](const golden::Case& c) {
        if (c.raises()) {
            CHECK(raised([&] { fv::normalize_title(c.arg<Json>(0)); }) == c.raises_type());
        } else {
            CHECK(fv::normalize_title(c.arg<Json>(0)) == c.result<std::string>());
        }
    });
}

TEST_CASE("golden services.models", "[golden]") {
    golden::for_each("services.models", "preview_text", [](const golden::Case& c) {
        const auto text = c.arg<std::optional<std::string>>(0);
        CHECK(fv::preview_text(text.value_or("")) == c.result<std::string>());
    });
    golden::for_each("services.models", "history_context", [](const golden::Case& c) {
        fv::ContextFields values;
        const auto context = c.arg<Json>(0);
        for (const auto& [key, value] : context.items()) {
            if (value.is_null()) {
                values.emplace(key, std::nullopt);
            } else {
                values.emplace(key, value.is_string() ? value.get<std::string>() : value.dump());
            }
        }
        if (c.raises()) {
            CHECK(raised([&] { fv::history_context(values); }) == c.raises_type());
            return;
        }
        Json kept = Json::object();
        for (const auto& [key, value] : fv::history_context(values)) {
            kept[key] = *value;
        }
        CHECK(golden::equal(c.result<Json>(), kept));
    });
}

TEST_CASE("golden services.history_manager", "[golden]") {
    golden::for_each("services.history_manager", "kind_of", [](const golden::Case& c) {
        CHECK(fv::kind_of(entry_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each("services.history_manager", "entry_version", [](const golden::Case& c) {
        CHECK(fv::entry_version(entry_from(c.arg<Json>(0))) == c.result<std::string>());
    });
    golden::for_each("services.history_manager", "ai_text", [](const golden::Case& c) {
        CHECK(fv::ai_text(entry_from(c.arg<Json>(0))) == c.result<std::optional<std::string>>());
    });
    golden::for_each("services.history_manager", "is_local_entry", [](const golden::Case& c) {
        CHECK(fv::is_local_entry(entry_from(c.arg<Json>(0))) == c.result<bool>());
    });
    golden::for_each("services.history_manager", "_describe_retention", [](const golden::Case& c) {
        CHECK(fv::describe_retention(c.arg<std::optional<std::int64_t>>(0),
                                     c.arg<std::optional<std::int64_t>>(1)) ==
              c.result<std::string>());
    });
    golden::for_each(
        "services.history_manager", "recordings_over_limit", [](const golden::Case& c) {
            std::vector<fv::RecordingInfo> recordings;
            for (const auto& item : c.arg<Json>(0)) {
                recordings.push_back({item[0].get<std::string>(), "", item[0].get<std::string>(),
                                      item[1].get<std::int64_t>()});
            }
            std::vector<std::string> names;
            for (const auto& rec :
                 fv::recordings_over_limit(recordings, c.arg<std::optional<std::int64_t>>(1),
                                           c.arg<std::optional<std::int64_t>>(2))) {
                names.push_back(rec.filename);
            }
            CHECK(names == c.result<std::vector<std::string>>());
        });
    golden::for_each("services.history_manager", "use_version", [](const golden::Case& c) {
        fv::history_test::TestDatabase db;
        fv::history_test::RecordingSync sync;
        fv::HistoryEntry entry = entry_from(c.arg<Json>(0));
        entry.timestamp = "2026-01-01T09:00:00+00:00";
        entry.model = "base";
        db.repository().put_history_entry(entry);
        fv::HistoryServices services;
        services.record_sync = &sync;
        fv::HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                                   services);
        const auto version = c.arg<std::string>(1);
        if (c.raises()) {
            CHECK(raised([&] { manager.use_version(entry.id, version); }) == c.raises_type());
            return;
        }
        const auto returned = manager.use_version(entry.id, version);
        const Json actual = {{"returned", stored_json(returned)},
                             {"stored", stored_json(manager.get_entry(entry.id))},
                             {"edited", !sync.edited.empty()}};
        CHECK(golden::equal(c.result<Json>(), actual));
    });
    golden::for_each("services.history_manager", "search_history_entries",
                     [](const golden::Case& c) {
                         fv::history_test::TestDatabase db;
                         for (const Json& row : c.arg<Json>(0)) {
                             fv::HistoryEntry entry = entry_from(row);
                             entry.model = "base";
                             db.repository().put_history_entry(entry);
                         }
                         std::vector<std::string> ids;
                         for (const auto& found : db.repository().search_history_entries(
                                  c.arg<std::string>(1), c.arg<std::optional<std::int64_t>>(2))) {
                             ids.push_back(found.id);
                         }
                         CHECK(ids == c.result<std::vector<std::string>>());
                     });
}

TEST_CASE("golden services.dictation_stats", "[golden]") {
    namespace stats = fv::dictation_stats;
    golden::for_each("services.dictation_stats", "words", [](const golden::Case& c) {
        CHECK(stats::words(c.arg<std::optional<std::string>>(0)) ==
              c.result<std::vector<std::string>>());
    });
    golden::for_each("services.dictation_stats", "count_words", [](const golden::Case& c) {
        CHECK(stats::count_words(c.arg<std::optional<std::string>>(0)) == c.result<std::size_t>());
    });
    golden::for_each("services.dictation_stats", "words_edited", [](const golden::Case& c) {
        CHECK(stats::words_edited(c.arg<std::optional<std::string>>(0),
                                  c.arg<std::optional<std::string>>(1)) ==
              c.result<std::int64_t>());
    });
    golden::for_each("services.dictation_stats", "next_milestone", [](const golden::Case& c) {
        CHECK(stats::next_milestone(c.arg<std::int64_t>(0)) == c.result<std::int64_t>());
    });
    golden::for_each("services.dictation_stats", "local_time", [](const golden::Case& c) {
        const auto moment =
            stats::local_time(c.arg<std::optional<std::string>>(0), fixed_zone(c.arg<int>(1)));
        const std::optional<std::string> text =
            moment ? std::optional<std::string>(moment->isoformat()) : std::nullopt;
        CHECK(text == c.result<std::optional<std::string>>());
    });
    golden::for_each("services.dictation_stats", "_stat", [](const golden::Case& c) {
        const Json& given = c.arg<Json>(0);
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
        const auto row = stats::stat(fields);
        if (!row) {
            CHECK(c.result<Json>().is_null());
            return;
        }
        const Json actual = {
            {"entry_id", optional_json(row->entry_id)},
            {"timestamp", row->timestamp},
            {"entry_kind", row->entry_kind},
            {"app_name", optional_json(row->app_name)},
            {"app_category", optional_json(row->app_category)},
            {"cleanup_level", optional_json(row->cleanup_level)},
            {"spoken_words", row->spoken_words},
            {"final_words", row->final_words},
            {"words_edited", row->words_edited},
            {"audio_seconds", row->audio_seconds ? Json(*row->audio_seconds) : Json(nullptr)},
            {"language", optional_json(row->language)},
            {"target", optional_json(row->target)},
            {"snippets", row->snippets},
            {"snippet_words", row->snippet_words},
            {"corrections", row->corrections},
            {"shots", row->shots},
        };
        CHECK(golden::equal(c.result<Json>(), actual));
    });
}
