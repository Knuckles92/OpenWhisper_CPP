// tests/test_history_export.py, and the export tests of test_personalize_foundation_history.py,
// test_personalize_s9_history_edit.py and test_personalize_coordinator.py.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "history/entry.h"
#include "history/export.h"
#include "history/manager.h"
#include "history_test_support.h"
#include "pyport/json.h"
#include "testkit/testkit.h"

#ifndef _WIN32
#include <sys/stat.h>
#endif

namespace history = fv::history;
using fv::HistoryEntry;
using fv::Json;
using fv::history_test::read_file;
using fv::history_test::write_file;

namespace {

// The machine's zone in OpenWhisper's tests does not matter to them; this pins one.
const fv::FakeClock& pacific_clock() {
    static const fv::FakeClock clock(fv::FakeClock::default_utc(), "America/Los_Angeles");
    return clock;
}

struct EntryArgs {
    std::string id = "h_1";
    std::string timestamp = "2026-03-15T14:30:00+00:00";
    std::string model = "local_whisper (turbo | cuda (float16))";
    std::string text = "Hello from history";
    std::optional<std::string> raw_text;
    std::optional<std::string> cleanup_provider;
    std::optional<std::string> cleanup_model;
    std::optional<std::string> audio_file;
    std::optional<double> transcription_time = 1.2;
    std::optional<double> audio_duration = 3.4;
    std::optional<std::int64_t> file_size = 2048;
    std::optional<std::string> source_name;
};

// test_history_export._entry
HistoryEntry entry(const EntryArgs& args = {}) {
    HistoryEntry row;
    row.id = args.id;
    row.timestamp = args.timestamp;
    row.model = args.model;
    row.text = args.text;
    row.raw_text = args.raw_text;
    row.cleanup_provider = args.cleanup_provider;
    row.cleanup_model = args.cleanup_model;
    row.audio_file = args.audio_file;
    row.transcription_time = args.transcription_time;
    row.audio_duration = args.audio_duration;
    row.file_size = args.file_size;
    row.source_name = args.source_name;
    return row;
}

Json serialize(const EntryArgs& args) {
    return history::serialize_history_entry(entry(args), pacific_clock());
}

std::vector<std::string> ids(const std::vector<Json>& rows) {
    std::vector<std::string> found;
    for (const Json& row : rows) {
        found.push_back(row.at("id").get<std::string>());
    }
    return found;
}

fv::py::DateTime naive(int year, int month, int day, int hour = 0, int minute = 0, int second = 0) {
    fv::py::DateTime moment;
    moment.year = year;
    moment.month = month;
    moment.day = day;
    moment.hour = hour;
    moment.minute = minute;
    moment.second = second;
    return moment;
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

} // namespace

// from tests/test_history_export.py::test_serialize_history_entry_adds_display_fields
TEST_CASE("serialize history entry adds display fields", "[test_history_export]") {
    const Json payload = serialize({.text = "", .audio_file = "recording_20260315_143000.wav"});
    CHECK(payload["id"] == "h_1");
    CHECK(payload["has_audio"] == true);
    CHECK(payload["preview_text"] == "Empty transcript");
    CHECK_FALSE(payload["formatted_timestamp"].get<std::string>().empty());
}

// from tests/test_history_export.py::test_list_export_entries_uses_manager_order
TEST_CASE("list export entries uses manager order", "[test_history_export]") {
    fv::history_test::TestDatabase db;
    db.add("h_old", "old", "2026-03-15T10:00:00+00:00");
    db.add("h_new", "new", "2026-03-16T10:00:00+00:00");
    fv::HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                               fv::history_test::services_with(nullptr, &pacific_clock()));

    CHECK(ids(history::list_export_entries(manager, pacific_clock())) ==
          std::vector<std::string>{"h_new", "h_old"});
    CHECK(ids(history::list_export_entries({entry({.id = "h_new"}), entry({.id = "h_old"})},
                                           pacific_clock())) ==
          std::vector<std::string>{"h_new", "h_old"});
}

// from tests/test_history_export.py::test_filter_export_entries_applies_date_and_audio
TEST_CASE("filter export entries applies date and audio", "[test_history_export]") {
    const std::vector<Json> rows = {
        serialize({.id = "h_old", .timestamp = "2026-01-01T10:00:00+00:00"}),
        serialize({.id = "h_mid", .timestamp = "2026-03-15T14:30:00+00:00"}),
        serialize({.id = "h_new", .timestamp = "2026-06-01T09:00:00+00:00"}),
        serialize({.id = "h_audio",
                   .timestamp = "2026-03-20T09:00:00+00:00",
                   .audio_file = "recording.wav"}),
        serialize({.id = "h_undated", .timestamp = ""}),
    };
    const auto& clock = pacific_clock();
    const auto filtered = history::filter_export_entries(
        rows, naive(2026, 3, 1), naive(2026, 3, 31, 23, 59, 59), false, clock);
    CHECK(ids(filtered) == std::vector<std::string>{"h_mid", "h_audio"});

    const auto audio_only = history::filter_export_entries(rows, {}, {}, true, clock);
    CHECK(ids(audio_only) == std::vector<std::string>{"h_audio"});

    const auto undated_dropped =
        history::filter_export_entries(rows, naive(2020, 1, 1), {}, false, clock);
    const auto kept = ids(undated_dropped);
    CHECK(std::ranges::find(kept, "h_undated") == kept.end());
}

// from tests/test_history_export.py::test_markdown_toggles_cleaned_and_raw
TEST_CASE("markdown toggles cleaned and raw", "[test_history_export]") {
    const Json row = serialize({.text = "Fixed hello",
                                .raw_text = "um hello",
                                .cleanup_provider = "openai",
                                .cleanup_model = "gpt-4o"});
    const std::string full = history::render_entry_document(row, history::FORMAT_MARKDOWN);
    CHECK(contains(full, "Fixed hello"));
    CHECK(contains(full, "### Raw"));
    CHECK(contains(full, "um hello"));
    CHECK(contains(full, "Cleanup: openai \xC2\xB7 gpt-4o"));

    const std::string cleaned_only = history::render_entry_document(
        row, history::FORMAT_MARKDOWN, {.include_cleaned = true, .include_raw = false});
    CHECK(contains(cleaned_only, "Fixed hello"));
    CHECK_FALSE(contains(cleaned_only, "### Raw"));

    const std::string raw_only = history::render_entry_document(
        row, history::FORMAT_MARKDOWN, {.include_cleaned = false, .include_raw = true});
    CHECK_FALSE(contains(raw_only, "Fixed hello"));
    CHECK(contains(raw_only, "um hello"));
}

// from tests/test_history_export.py::test_txt_and_json_ignore_markdown_toggles
TEST_CASE("txt and json ignore markdown toggles", "[test_history_export]") {
    const Json row = serialize({.text = "Fixed", .raw_text = "Raw", .cleanup_model = "gpt-4o"});
    const history::ExportOptions none{.include_cleaned = false, .include_raw = false};
    const std::string txt = history::render_entry_document(row, history::FORMAT_TXT, none);
    CHECK(contains(txt, "Fixed"));
    CHECK(contains(txt, "Raw:"));
    const Json payload =
        fv::py::loads(history::render_entry_document(row, history::FORMAT_JSON, none));
    CHECK(payload["text"] == "Fixed");
    CHECK(payload["raw_text"] == "Raw");
    CHECK_FALSE(payload.contains("preview_text"));
}

// from tests/test_history_export.py::test_combined_documents_wrap_entries
TEST_CASE("combined documents wrap entries", "[test_history_export]") {
    const std::vector<Json> rows = {serialize({.id = "h_a", .text = "Alpha"}),
                                    serialize({.id = "h_b", .text = "Beta"})};
    const auto& clock = pacific_clock();
    const std::string markdown =
        history::render_export_document(rows, history::FORMAT_MARKDOWN, {}, clock);
    CHECK(markdown.starts_with("# Transcription History Export"));
    CHECK(contains(markdown, "Alpha"));
    CHECK(contains(markdown, "Beta"));

    const std::string txt = history::render_export_document(rows, history::FORMAT_TXT, {}, clock);
    CHECK(contains(txt, "Alpha"));
    CHECK(contains(txt, std::string(60, '-')));

    const Json envelope =
        fv::py::loads(history::render_export_document(rows, history::FORMAT_JSON, {}, clock));
    CHECK(envelope["format_version"] == history::kFormatVersion);
    std::vector<std::string> found;
    for (const Json& row : envelope["entries"]) {
        found.push_back(row["id"].get<std::string>());
    }
    CHECK(found == std::vector<std::string>{"h_a", "h_b"});
}

// from tests/test_history_export.py::test_write_per_entry_files_disambiguates_stems
TEST_CASE("write per entry files disambiguates stems", "[test_history_export]") {
    fv::testkit::TempDataRoot tmp;
    const std::string stamp = "2026-03-15T14:30:00+00:00";
    const std::vector<Json> rows = {
        serialize({.id = "h_a", .timestamp = stamp, .text = "Same title"}),
        serialize({.id = "h_b", .timestamp = stamp, .text = "Same title"}),
    };
    const auto written =
        history::write_per_entry_files(rows, history::FORMAT_TXT, tmp.path(), {}, pacific_clock());
    std::set<std::string> names;
    for (const auto& path : written) {
        names.insert(fv::path_to_utf8(path.filename()));
    }
    CHECK(names.size() == 2);
    CHECK(
        std::ranges::all_of(names, [](const std::string& name) { return name.ends_with(".txt"); }));
    CHECK_FALSE(read_file(written.front()).empty());
}

// from tests/test_history_export.py::test_write_per_entry_files_never_overwrites_prior_export
TEST_CASE("write per entry files never overwrites prior export", "[test_history_export]") {
    fv::testkit::TempDataRoot tmp;
    const auto& clock = pacific_clock();
    const Json row = serialize({.text = "Existing title"});
    const std::string original_name = history::entry_file_stem(row, clock) + ".txt";
    const auto original = tmp.path() / fv::path_from_utf8(original_name);
    write_file(original, "keep me");

    const auto written =
        history::write_per_entry_files({row}, history::FORMAT_TXT, tmp.path(), {}, clock);

    CHECK(read_file(original) == "keep me");
    std::string expected_name = original_name;
    expected_name.replace(expected_name.find(".txt"), 4, "-2.txt");
    CHECK(written ==
          std::vector<std::filesystem::path>{tmp.path() / fv::path_from_utf8(expected_name)});
}

// from tests/test_history_export.py::test_entry_file_stem_uses_local_stamp_and_preview
TEST_CASE("entry file stem uses local stamp and preview", "[test_history_export]") {
    const Json row =
        serialize({.timestamp = "2026-03-15T14:30:00+00:00", .text = "Hello from history"});
    const std::string stem = history::entry_file_stem(row, pacific_clock());
    CHECK(contains(stem, "Hello-from-history"));
    CHECK(stem.ends_with("Hello-from-history"));
}

// from
// tests/test_personalize_foundation_history.py::test_history_export_carries_the_context_columns
TEST_CASE("history export carries the context columns", "[test_personalize_foundation_history]") {
    const fv::ContextFields context = {
        {"app_id", "outlook.exe"},   {"app_name", "Outlook"},     {"app_category", "email"},
        {"cleanup_level", "medium"}, {"entry_kind", "dictation"}, {"cleaned_text", "Hello, world."},
        {"language", "en"},
    };
    const HistoryEntry row = fv::create_entry(
        {.text = "Hello, world.", .model = "base", .context = context}, pacific_clock());
    const Json payload = history::serialize_history_entry(row, pacific_clock());

    for (const std::string_view column : fv::HISTORY_CONTEXT_COLUMNS) {
        INFO(column);
        CHECK(payload[std::string(column)] == *context.at(std::string(column)));
    }
}

// from tests/test_personalize_s9_history_edit.py::test_export_and_agents_read_the_chosen_text
// (the export half; the agent API half is M5's)
TEST_CASE("export and agents read the chosen text", "[test_personalize_s9_history_edit]") {
    fv::history_test::TestDatabase db;
    fv::history_test::RecordingSync sync;
    fv::HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                               fv::history_test::services_with(&sync, &pacific_clock()));
    // _cleaned(): a dictation whose AI version replaced what was said.
    fv::AddEntry fields;
    fields.text = "Hello, world.";
    fields.model = "base";
    fields.raw_text = "hello world um";
    const auto saved = manager.add_entry(fields);
    REQUIRE(saved);
    manager.use_version(saved->id, fv::ORIGINAL_VERSION);
    const auto stored = manager.get_entry(saved->id);
    REQUIRE(stored);

    const Json payload = history::serialize_history_entry(*stored, pacific_clock());
    CHECK(payload["text"] == "hello world um");
    CHECK(payload["raw_text"] == "hello world um");
    CHECK(payload["cleaned_text"] == "Hello, world.");
}

// from tests/test_personalize_coordinator.py::test_export_says_when_the_original_was_kept
TEST_CASE("export says when the original was kept", "[test_personalize_coordinator]") {
    Json row = {{"text", "um hi"},
                {"raw_text", "um hi"},
                {"cleaned_text", "Hi."},
                {"cleanup_provider", "openrouter"},
                {"cleanup_model", "m"}};
    CHECK(history::cleanup_label(row) == "openrouter \xC2\xB7 m (original kept)");
    row["text"] = "Hi.";
    CHECK(history::cleanup_label(row) == "openrouter \xC2\xB7 m");
}

#ifndef _WIN32
TEST_CASE("per-entry files get the permissions of Python's open", "[history_export]") {
    fv::testkit::TempDataRoot tmp;
    const mode_t previous = ::umask(022);
    const auto written =
        history::write_per_entry_files({serialize({.text = "Shared notes"})}, history::FORMAT_TXT,
                                       tmp.path(), {}, pacific_clock());
    ::umask(previous);
    REQUIRE(written.size() == 1);
    namespace fs = std::filesystem;
    CHECK((fs::status(written.front()).permissions() & fs::perms::all) ==
          (fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read |
           fs::perms::others_read));
}
#endif

TEST_CASE("the default export file is named after the product", "[history_export]") {
    CHECK(history::default_export_file_stem() == "fiorivoce_history");
}
