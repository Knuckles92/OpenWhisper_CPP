// The counting rules of services/dictation_stats.py:
//   tests/test_personalize_s9_stats.py (9 of 18; the summary tests are M2's),
//   tests/test_personalize_fix_f5_stats_spoken.py (2 of 5; the snippet tests are M4's),
//   tests/test_stats_features.py (4 of 20; meeting rows are inserted straight into the schema).
//
// load_summary (M2) first calls _ensure_backfilled and count_features, then reads the rows of
// the counted and feature kinds. Where a Python test reads a summary number, these tests run the
// same two steps (counted()) and check the number on those rows: words_all_time is the sum of
// their spoken words, entries their count, a feature's count, seconds and words the rows of its
// kind, words_cleaned_up the edited words of the counted kinds.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "history/dictation_stats.h"
#include "history/manager.h"
#include "history_test_support.h"
#include "pyport/datetime.h"
#include "pyport/errors.h"
#include "sql/db.h"

namespace stats = fv::dictation_stats;
using fv::history_test::TestDatabase;
using stats::StatRow;

namespace {

// timezone(timedelta(hours=-7)) and (hours=9); Etc/GMT names count the other way.
const std::chrono::time_zone& pacific() {
    return *std::chrono::locate_zone("Etc/GMT+7");
}
const std::chrono::time_zone& tokyo() {
    return *std::chrono::locate_zone("Etc/GMT-9");
}

constexpr fv::py::Date today{2026, 10, 6};

// _at: an aware UTC timestamp for `hour` o'clock Pacific time (UTC-7) on `day`.
std::string at(fv::py::Date day, int hour = 12) {
    const std::chrono::sys_seconds instant =
        std::chrono::sys_days{std::chrono::year{day.year} / static_cast<unsigned>(day.month) /
                              static_cast<unsigned>(day.day)} +
        std::chrono::hours{hour + 7};
    return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", instant);
}

struct Stats {
    TestDatabase db;
    stats::StatsRepository repository{db.database()};

    // _record: a dictation (by default) counted from its fields alone.
    void record(std::string text, std::optional<std::string> raw = std::nullopt,
                std::optional<double> seconds = std::nullopt, std::string kind = "dictation",
                std::optional<std::string> app = std::nullopt,
                std::optional<std::string> entry_id = std::nullopt, fv::py::Date day = today,
                int hour = 12) {
        stats::EntryFields fields;
        fields.text = std::move(text);
        fields.raw_text = std::move(raw);
        fields.audio_duration = seconds;
        fields.entry_kind = std::move(kind);
        fields.app_name = std::move(app);
        fields.id = std::move(entry_id);
        fields.timestamp = at(day, hour);
        repository.record(fields, nullptr);
    }
    // _rows: every row but the markers.
    std::vector<StatRow> rows() {
        std::vector<StatRow> found;
        for (auto& row : repository.rows()) {
            if (row.entry_kind != "marker") {
                found.push_back(std::move(row));
            }
        }
        return found;
    }
    // What load_summary counts first, then the rows it reads (_rows: counted and feature
    // kinds with a readable timestamp).
    std::vector<StatRow> counted() {
        repository.ensure_backfilled();
        repository.count_features();
        std::vector<StatRow> found;
        for (auto& row : repository.rows()) {
            const bool kind_read = std::ranges::find(stats::COUNTED_KINDS, row.entry_kind) !=
                                       stats::COUNTED_KINDS.end() ||
                                   std::ranges::find(stats::FEATURE_KINDS, row.entry_kind) !=
                                       stats::FEATURE_KINDS.end();
            if (kind_read && stats::local_time(row.timestamp, pacific())) {
                found.push_back(std::move(row));
            }
        }
        return found;
    }
    void history(std::string id, std::string text, std::string timestamp,
                 fv::ContextFields context = {}, std::optional<std::string> source = std::nullopt,
                 std::optional<std::string> raw = std::nullopt,
                 std::optional<double> audio_duration = std::nullopt) {
        db.add(std::move(id), std::move(text), std::move(timestamp), "base", std::move(context),
               std::move(source), std::move(raw), audio_duration);
    }
};

std::int64_t words_all_time(const std::vector<StatRow>& rows) {
    std::int64_t total = 0;
    for (const auto& row : rows) {
        total += row.spoken_words;
    }
    return total;
}

std::vector<StatRow> of_kind(const std::vector<StatRow>& rows, std::string_view kind) {
    std::vector<StatRow> found;
    for (const auto& row : rows) {
        if (row.entry_kind == kind) {
            found.push_back(row);
        }
    }
    return found;
}

// _meeting: a meeting with two segments ("hello there", "three more words": 5 words).
void meeting(TestDatabase& db, const std::string& meeting_id,
             std::chrono::system_clock::time_point started, int minutes = 30, double paused = 0.0,
             const std::string& status = "ended",
             std::optional<std::string> origin = std::nullopt) {
    const auto iso = [](std::chrono::system_clock::time_point moment) {
        const auto micros = std::chrono::floor<std::chrono::microseconds>(moment);
        return std::format("{:%Y-%m-%dT%H:%M:%S}+00:00", micros);
    };
    const std::string started_at = iso(started);
    const std::optional<std::string> ended =
        status == "active"
            ? std::nullopt
            : std::optional<std::string>(iso(started + std::chrono::minutes(minutes)));
    db.database().write([&](fv::sql::Connection& conn) {
        fv::sql::Statement session = conn.prepare(
            "INSERT INTO meeting_sessions (id, title, status, started_at, ended_at, "
            "paused_total_s, host_token, guest_token, cloud_enabled, spool_dir, state_seq, "
            "origin_device_id) VALUES (?, 'Standup', ?, ?, ?, ?, 'h', 'g', 0, '', 0, ?)");
        session.bind(1, meeting_id);
        session.bind(2, status);
        session.bind(3, started_at);
        session.bind(4, ended ? fv::sql::Value(*ended) : fv::sql::Value(nullptr));
        session.bind(5, paused);
        session.bind(6, origin ? fv::sql::Value(*origin) : fv::sql::Value(nullptr));
        session.step();
        int index = 0;
        for (const char* text : {"hello there", "three more words"}) {
            fv::sql::Statement segment = conn.prepare(
                "INSERT INTO meeting_segments (id, meeting_id, channel, start_s, end_s, text, "
                "speaker_source, speaker_pinned, created_at) VALUES (?, ?, 'mic', ?, ?, ?, "
                "'auto', 0, ?)");
            segment.bind(1, meeting_id + "-" + std::to_string(index));
            segment.bind(2, meeting_id);
            segment.bind(3, static_cast<double>(index));
            segment.bind(4, index + 1.0);
            segment.bind(5, std::string(text));
            segment.bind(6, started_at);
            segment.step();
            ++index;
        }
    });
}

// SqlMeetingRepository.delete_meeting: counts first (so a deleted meeting keeps its row), then
// deletes the meeting's rows.
void delete_meeting(Stats& s, const std::string& meeting_id) {
    s.repository.count_features();
    s.db.database().write([&](fv::sql::Connection& conn) {
        for (const char* table : {"meeting_segments", "meeting_sessions"}) {
            const std::string column =
                std::string(table) == "meeting_sessions" ? "id" : "meeting_id";
            fv::sql::Statement statement =
                conn.prepare("DELETE FROM " + std::string(table) + " WHERE " + column + " = ?");
            statement.bind(1, meeting_id);
            statement.step();
        }
    });
}

} // namespace

// from tests/test_personalize_s9_stats.py::test_words_count_spaces_or_cjk_characters
TEST_CASE("test_words_count_spaces_or_cjk_characters", "[test_personalize_s9_stats]") {
    const auto [text, expected] = GENERATE(table<std::optional<std::string>, std::size_t>({
        {"Hello, world.", 2},
        {"  so — anyway ...  ", 2},
        {"我喜欢你", 4},
        {"今日はgood day", 5},
        {"안녕 하세요", 2},
        {"", 0},
        {std::nullopt, 0},
    }));
    CHECK(stats::count_words(text) == expected);
}

// from tests/test_personalize_s9_stats.py::test_words_edited_counts_word_level_changes
TEST_CASE("test_words_edited_counts_word_level_changes", "[test_personalize_s9_stats]") {
    const auto [before, after, expected] = GENERATE(table<std::string, std::string, std::int64_t>({
        {"a b c", "a b c", 0},
        {"a b c", "a x c", 1},
        {"a b c", "a c", 1},
        {"a c", "a b c", 1},
        {"um so I think", "I think", 2},
        {"one two", "three four five", 3},
    }));
    CHECK(stats::words_edited(before, after) == expected);
}

// from tests/test_personalize_s9_stats.py::test_local_times_convert_aware_times_and_keep_naive_ones
TEST_CASE("test_local_times_convert_aware_times_and_keep_naive_ones",
          "[test_personalize_s9_stats]") {
    using fv::py::DateTime;
    CHECK(stats::local_time("2026-10-05T23:30:00+00:00", pacific()) ==
          DateTime{2026, 10, 5, 16, 30});
    CHECK(stats::local_time("2026-10-05T23:30:00+00:00", tokyo()) == DateTime{2026, 10, 6, 8, 30});
    CHECK(stats::local_time("2026-10-05T23:30:00Z", tokyo()) == DateTime{2026, 10, 6, 8, 30});
    CHECK(stats::local_time("2026-10-05T23:30:00", tokyo()) == DateTime{2026, 10, 5, 23, 30});
    CHECK_FALSE(stats::local_time("not a time", *std::chrono::current_zone()));
}

// The expected values are Python 3.12's datetime.fromisoformat(...).astimezone(tz).
TEST_CASE("local times of far-future and year-1 moments follow Python's datetime",
          "[dictation_stats]") {
    using fv::py::DateTime;
    const auto& utc = *std::chrono::locate_zone("Etc/UTC");
    CHECK(stats::local_time("9999-12-31T10:00:00-05:00", utc) == DateTime{9999, 12, 31, 15, 0});
    CHECK(stats::local_time("7000-06-01T12:00:00+00:00", tokyo()) == DateTime{7000, 6, 1, 21, 0});
    CHECK(stats::local_time("9999-12-31T14:59:59.999999+00:00", tokyo()) ==
          DateTime{9999, 12, 31, 23, 59, 59, 999999});
    // OverflowError: the moment in UTC, which astimezone() passes through, leaves years 1..9999.
    CHECK_THROWS_AS(stats::local_time("9999-12-31T23:00:00-05:00", utc), fv::py::OverflowError);
    CHECK_THROWS_AS(stats::local_time("9999-12-31T23:00:00-05:00", pacific()),
                    fv::py::OverflowError);
    CHECK_THROWS_AS(stats::local_time("0001-01-01T03:00:00+05:00", tokyo()), fv::py::OverflowError);
    CHECK_THROWS_AS(stats::local_time("0001-01-01T06:00:00+05:00", pacific()),
                    fv::py::OverflowError);
}

// from tests/test_personalize_s9_stats.py::test_a_saved_dictation_is_counted_from_its_entry
TEST_CASE("test_a_saved_dictation_is_counted_from_its_entry", "[test_personalize_s9_stats]") {
    Stats s;
    fv::HistoryManager manager(s.db.database(), s.db.recordings(), fv::RecordingRetention{});
    fv::AddEntry fields;
    fields.text = "Hello, world.";
    fields.raw_text = "um hello world";
    fields.model = "base";
    fields.audio_duration = 3.0;
    fields.source_name = "Quick Record";
    fields.columns = {{"entry_kind", "dictation"},
                      {"app_name", "Slack"},
                      {"app_category", "work"},
                      {"cleanup_level", "medium"},
                      {"language", "en"}};
    const auto entry = manager.add_entry(fields).value();
    // dictation_pipeline.record_stats, twice.
    stats::record(s.repository, stats::EntryFields::of(fields), &entry);
    stats::record(s.repository, stats::EntryFields::of(fields), &entry);

    const auto rows = s.rows();
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    CHECK(std::tie(row.entry_id, row.entry_kind, row.app_name, row.app_category) ==
          std::make_tuple(std::optional<std::string>(entry.id), std::string("dictation"),
                          std::optional<std::string>("Slack"), std::optional<std::string>("work")));
    CHECK(std::tie(row.spoken_words, row.final_words, row.words_edited) ==
          std::make_tuple(3, 2, 3));
    CHECK(row.audio_seconds == 3.0);
    CHECK(row.cleanup_level == "medium");
    CHECK(row.language == "en");
    CHECK(row.timestamp == entry.timestamp);
}

// from
// tests/test_personalize_s9_stats.py::test_files_add_no_dictated_words_and_other_computers_entries_are_not_counted
TEST_CASE("test_files_add_no_dictated_words_and_other_computers_entries_are_not_counted",
          "[test_personalize_s9_stats]") {
    Stats s;
    s.repository.reset();
    stats::EntryFields talk;
    talk.text = "a talk";
    talk.entry_kind = "file";
    talk.audio_duration = 90.0;
    talk.timestamp = at(today);
    s.repository.record(talk, nullptr);
    stats::EntryFields upload;
    upload.text = "upload";
    upload.source_name = "talk.mp3";
    upload.timestamp = at(today);
    s.repository.record(upload, nullptr);
    stats::EntryFields theirs;
    theirs.text = "theirs";
    theirs.entry_kind = "dictation";
    theirs.origin_device_id = "laptop";
    theirs.timestamp = at(today);
    s.repository.record(theirs, nullptr);
    stats::EntryFields again;
    again.text = "said before";
    again.entry_kind = "file";
    again.timestamp = at(today);
    again.stats_counts = {{"retranscription", true}};
    s.repository.record(again, nullptr);

    std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> seen;
    for (const auto& row : s.rows()) {
        seen.emplace_back(row.entry_kind, row.spoken_words, row.final_words);
    }
    CHECK(seen == std::vector<std::tuple<std::string, std::int64_t, std::int64_t>>{{"file", 0, 2},
                                                                                   {"file", 0, 1}});
    const auto rows = s.counted();
    // words_all_time 0, and no counted kind, so no day of the streak.
    CHECK(words_all_time(rows) == 0);
    CHECK(of_kind(rows, "dictation").empty());
    CHECK(rows.size() == 2); // entries
    const auto files = of_kind(rows, "file");
    double seconds = 0;
    std::int64_t file_words = 0;
    for (const auto& row : files) {
        seconds += row.audio_seconds.value_or(0.0);
        file_words += row.final_words;
        // Both fall on the last bar of the week: today, Pacific time.
        CHECK(stats::local_time(row.timestamp, pacific())->date() == today);
    }
    CHECK(files.size() == 2);
    CHECK(seconds == 90.0);
    CHECK(file_words == 3);
}

// from tests/test_personalize_s9_stats.py::test_a_rewrite_adds_its_edits_but_no_spoken_words
TEST_CASE("test_a_rewrite_adds_its_edits_but_no_spoken_words", "[test_personalize_s9_stats]") {
    Stats s;
    s.record("Make it formal, please.", "make it formal pls", 4.0, "command");
    const auto rows = s.rows();
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].spoken_words == 0);
    CHECK(rows[0].final_words == 4);
    CHECK_FALSE(rows[0].audio_seconds);
    CHECK(rows[0].words_edited == 3);
}

// from tests/test_personalize_s9_stats.py::test_listeners_hear_each_counted_dictation_once
TEST_CASE("test_listeners_hear_each_counted_dictation_once", "[test_personalize_s9_stats]") {
    Stats s;
    int heard = 0;
    {
        const fv::Subscription listener = stats::add_listener(s.repository, [&] { ++heard; });
        s.record("counted", std::nullopt, std::nullopt, "dictation", std::nullopt, "one");
        s.record("counted again", std::nullopt, std::nullopt, "dictation", std::nullopt, "one");
        stats::EntryFields theirs;
        theirs.text = "theirs";
        theirs.entry_kind = "dictation";
        theirs.origin_device_id = "laptop";
        theirs.timestamp = at(today);
        s.repository.record(theirs, nullptr);
    }
    s.record("not heard");
    CHECK(heard == 1);
}

// from tests/test_personalize_s9_stats.py::test_earlier_dictations_are_counted_once_on_first_load
TEST_CASE("test_earlier_dictations_are_counted_once_on_first_load", "[test_personalize_s9_stats]") {
    Stats s;
    s.history("legacy", "said this before", "2026-10-05T09:00:00", {}, "Quick Record", std::nullopt,
              2.0);
    s.history("profile", "dear team", "2026-10-04T09:00:00", {}, "Quick Record · Email",
              "dear team um");
    s.history("v17", "Kind dictation.", "2026-10-03T16:00:00+00:00", {{"entry_kind", "dictation"}});
    s.history("upload", "a talk", "2026-10-05T10:00:00", {}, "talk.mp3");
    s.history("retranscribe", "again", "2026-10-05T11:00:00", {{"entry_kind", "file"}},
              "recording_x.wav");
    s.history("theirs", "laptop words", "2026-10-05T12:00:00", {}, "Quick Record");
    s.db.set_origin("theirs", "laptop");
    // Saved live after the upgrade, before Stats was ever opened.
    fv::HistoryManager manager(s.db.database(), s.db.recordings(), fv::RecordingRetention{});
    fv::AddEntry fields;
    fields.text = "fresh words";
    fields.model = "base";
    fields.columns = {{"entry_kind", "dictation"}};
    const auto live = manager.add_entry(fields).value();
    s.repository.record(stats::EntryFields::of(fields), &live);

    const auto rows = s.counted();

    std::vector<std::string> ids;
    for (const auto& row : s.rows()) {
        ids.push_back(row.entry_id.value_or(""));
    }
    std::ranges::sort(ids);
    std::vector<std::string> expected = {"legacy", "profile", "v17", live.id, "upload"};
    std::ranges::sort(expected);
    CHECK(ids == expected);
    CHECK(words_all_time(rows) == 3 + 3 + 2 + 2);
    std::int64_t cleaned_up = 0;
    for (const auto& row : rows) {
        if (std::ranges::find(stats::COUNTED_KINDS, row.entry_kind) != stats::COUNTED_KINDS.end()) {
            cleaned_up += row.words_edited;
        }
    }
    CHECK(cleaned_up == 1);
    CHECK(of_kind(rows, "file").size() == 1);
    s.counted();
    CHECK(s.rows().size() == 5);
}

// from
// tests/test_personalize_s9_stats.py::test_reset_clears_stats_keeps_history_and_doesnt_count_it_again
TEST_CASE("test_reset_clears_stats_keeps_history_and_doesnt_count_it_again",
          "[test_personalize_s9_stats]") {
    Stats s;
    s.history("legacy", "said this before", "2026-10-05T09:00:00", {}, "Quick Record");
    CHECK(words_all_time(s.counted()) == 3);

    s.repository.reset();

    const auto empty = s.counted();
    CHECK(words_all_time(empty) == 0);
    CHECK(empty.empty()); // entries 0, and no app to rank
    CHECK(s.db.repository().get_history_entry_by_id("legacy"));
    s.record("after the reset");
    CHECK(words_all_time(s.counted()) == 3);
}

// from
// tests/test_personalize_fix_f5_stats_spoken.py::test_a_dictation_without_snippets_counts_as_before
TEST_CASE("test_a_dictation_without_snippets_counts_as_before",
          "[test_personalize_fix_f5_stats_spoken]") {
    // What the transcription runtime saves for "hello there friend" with AI cleanup on and no
    // snippet: the cleaned text, the raw text, and the raw text as the words said.
    Stats s;
    fv::HistoryManager manager(s.db.database(), s.db.recordings(), fv::RecordingRetention{});
    fv::AddEntry fields;
    fields.text = "Hello there friend.";
    fields.raw_text = "hello there friend";
    fields.model = "base";
    fields.audio_duration = 1.5;
    fields.source_name = "Quick Record";
    fields.columns = {{"entry_kind", "dictation"}};
    fields.spoken_text = "hello there friend";
    fields.stats_counts = {{"snippets", 0}, {"snippet_words", 0}, {"corrections", 0}};
    const auto entry = manager.add_entry(fields).value();
    s.repository.record(stats::EntryFields::of(fields), &entry);

    std::vector<std::int64_t> spoken;
    for (const auto& row : s.rows()) {
        spoken.push_back(row.spoken_words);
    }
    CHECK(spoken == std::vector<std::int64_t>{3});
}

// from tests/test_personalize_fix_f5_stats_spoken.py::test_spoken_text_wins_over_the_saved_text
TEST_CASE("test_spoken_text_wins_over_the_saved_text", "[test_personalize_fix_f5_stats_spoken]") {
    std::string intro;
    for (int i = 0; i < 120; ++i) {
        intro += (i == 0 ? "" : " ") + std::format("word{}", i);
    }
    stats::EntryFields fields;
    fields.entry_kind = "dictation";
    fields.text = intro;
    fields.spoken_text = "my intro";
    fields.audio_duration = 1.2;
    const auto row = stats::stat(fields);
    REQUIRE(row);
    CHECK(row->spoken_words == 2);
    CHECK(row->final_words == 120);
}

// from
// tests/test_stats_features.py::test_finished_meetings_are_counted_once_and_outlive_their_deletion
TEST_CASE("test_finished_meetings_are_counted_once_and_outlive_their_deletion",
          "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    // As on an install that has never counted meetings.
    s.db.database().write([](fv::sql::Connection& conn) {
        conn.exec("DELETE FROM dictation_stats WHERE target = 'features'");
    });
    const auto start = fv::history_test::utc(2026, 10, 6, 17);
    meeting(s.db, "done", start, 30, 300.0);
    meeting(s.db, "running", start, 30, 0.0, "active");
    meeting(s.db, "theirs", start, 30, 0.0, "ended", "laptop");

    auto meetings = of_kind(s.counted(), "meeting");
    REQUIRE(meetings.size() == 1);
    CHECK(meetings[0].audio_seconds == 1500.0);
    CHECK(meetings[0].final_words == 5);
    CHECK(meetings[0].entry_id == "done");
    // In the week ending 2026-10-06, Pacific time.
    CHECK(stats::local_time(meetings[0].timestamp, pacific())->date() == today);

    delete_meeting(s, "done");
    CHECK(of_kind(s.counted(), "meeting").size() == 1);
}

// from
// tests/test_stats_features.py::test_a_meeting_deleted_before_stats_were_opened_is_still_counted
TEST_CASE("test_a_meeting_deleted_before_stats_were_opened_is_still_counted",
          "[test_stats_features]") {
    Stats s;
    s.repository.reset();
    meeting(s.db, "quick", std::chrono::system_clock::now() + std::chrono::minutes(1), 10);
    delete_meeting(s, "quick");
    CHECK(of_kind(s.counted(), "meeting").size() == 1);
}

// from tests/test_stats_features.py::test_a_reset_forgets_past_meetings_and_counts_later_ones
TEST_CASE("test_a_reset_forgets_past_meetings_and_counts_later_ones", "[test_stats_features]") {
    Stats s;
    meeting(s.db, "before", std::chrono::system_clock::now() - std::chrono::hours(2));
    CHECK(of_kind(s.counted(), "meeting").size() == 1);

    s.repository.reset();
    CHECK(s.counted().empty());

    meeting(s.db, "after", std::chrono::system_clock::now() + std::chrono::minutes(1));
    CHECK(of_kind(s.counted(), "meeting").size() == 1);
}

// from
// tests/test_stats_features.py::test_history_from_before_is_counted_once_without_retranscribed_recordings
TEST_CASE("test_history_from_before_is_counted_once_without_retranscribed_recordings",
          "[test_stats_features]") {
    Stats s;
    s.history("upload", "a talk", "2026-10-05T10:00:00", {}, "talk.mp3", std::nullopt, 60.0);
    s.history("again", "said before", "2026-10-05T11:00:00", {{"entry_kind", "file"}},
              "recording_20261005.wav");
    s.history("walk", "step one", "2026-10-05T12:00:00", {{"entry_kind", "walkthrough"}});
    s.history("failed", "", "2026-10-05T13:00:00",
              {{"entry_kind", std::string(fv::FAILED_RECORDING_KIND)}});

    for (int load = 0; load < 2; ++load) {
        const auto rows = s.counted();
        CHECK(of_kind(rows, "file").size() == 1);
        CHECK(of_kind(rows, "walkthrough").size() == 1);
    }
}

TEST_CASE("count_features skips meetings that ended before the features marker",
          "[dictation_stats]") {
    Stats s;
    const auto now = std::chrono::system_clock::now();
    s.repository.count_features(); // writes the features marker now
    meeting(s.db, "old", now - std::chrono::hours(3));
    meeting(s.db, "paused", now + std::chrono::minutes(1), 30, 0.0, "paused");
    meeting(s.db, "long-pause", now + std::chrono::minutes(1), 1, 600.0);
    s.repository.count_features();
    const auto meetings = of_kind(s.rows(), "meeting");
    REQUIRE(meetings.size() == 1);
    CHECK(meetings[0].entry_id == "long-pause");
    CHECK(meetings[0].audio_seconds == 0.0); // floored at 0
}

TEST_CASE("counts too large for SQLite fail as Python's insert does", "[dictation_stats]") {
    stats::EntryFields fields;
    fields.entry_kind = "action";
    fields.text = "sent";
    fields.timestamp = at(today);
    fields.stats_counts = {{"snippet_words", 1e300}};
    CHECK_THROWS_AS(stats::stat(fields), fv::py::OverflowError);
    fields.stats_counts = {{"snippet_words", -1e300}, {"corrections", 2.9}};
    const auto row = stats::stat(fields);
    REQUIRE(row);
    CHECK(row->snippet_words == 0);
    CHECK(row->corrections == 2);
}
