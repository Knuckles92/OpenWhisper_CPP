// tests/test_personalize_foundation_history.py: the per-entry context columns, the failed
// recording retry and the dictation_stats rows (the v16 migration test is tests/db's; the export
// test is M2's), plus HistoryManager's saving, last dictation and clears.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "history/dictation_stats.h"
#include "history/entry.h"
#include "history/manager.h"
#include "history_test_support.h"
#include "pyport/errors.h"
#include "sql/db.h"

using fv::ContextFields;
using fv::HistoryManager;
using fv::history_test::RecordingSync;
using fv::history_test::services_with;
using fv::history_test::TestDatabase;

namespace {

const ContextFields context_values = {
    {"app_id", "outlook.exe"},   {"app_name", "Outlook"},     {"app_category", "email"},
    {"cleanup_level", "medium"}, {"entry_kind", "dictation"}, {"cleaned_text", "Hello, world."},
    {"language", "en"},
};

ContextFields context_of(const fv::HistoryEntry& entry) {
    ContextFields values;
    for (const auto column : fv::HISTORY_CONTEXT_COLUMNS) {
        values.emplace(std::string(column), *entry.context_column(column));
    }
    return values;
}

ContextFields all_null() {
    ContextFields values;
    for (const auto column : fv::HISTORY_CONTEXT_COLUMNS) {
        values.emplace(std::string(column), std::nullopt);
    }
    return values;
}

} // namespace

// from
// tests/test_personalize_foundation_history.py::test_add_history_entry_stores_context_and_leaves_unset_ones_null
TEST_CASE("test_add_history_entry_stores_context_and_leaves_unset_ones_null",
          "[test_personalize_foundation_history]") {
    TestDatabase db;
    db.add("e1", "Hello, world.", "2026-10-06T10:00:00+00:00", "base", context_values);
    db.add("e2", "plain", "2026-10-06T10:01:00+00:00", "base", {{"app_name", std::nullopt}});

    const auto stored = db.repository().get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK(context_of(*stored) == context_values);
    const auto plain = db.repository().get_history_entry_by_id("e2");
    REQUIRE(plain);
    CHECK(context_of(*plain) == all_null());
}

// from
// tests/test_personalize_foundation_history.py::test_only_known_context_columns_reach_the_model
TEST_CASE("test_only_known_context_columns_reach_the_model",
          "[test_personalize_foundation_history]") {
    fv::NewEntry fields;
    fields.text = "hi";
    fields.model = "base";
    fields.context = {{"entry_kind", "command"}};
    CHECK(fv::create_entry(fields).entry_kind == "command");

    fields.context = {{"window_title", "Inbox"}};
    try {
        fv::create_entry(fields);
        FAIL("expected TypeError");
    } catch (const fv::py::TypeError& error) {
        CHECK(std::string(error.what()).find("window_title") != std::string::npos);
    }
}

// from
// tests/test_personalize_foundation_history.py::test_history_manager_passes_context_through_and_drops_the_rest
TEST_CASE("test_history_manager_passes_context_through_and_drops_the_rest",
          "[test_personalize_foundation_history]") {
    TestDatabase db;
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{});
    fv::AddEntry fields;
    fields.text = "Hello, world.";
    fields.model = "base";
    fields.columns = context_values;
    fields.columns["window_title"] = "Inbox — never stored";
    fields.columns["app_category"] = std::nullopt;

    const auto entry = manager.add_entry(fields).value();

    const auto stored = manager.get_entry(entry.id);
    REQUIRE(stored);
    CHECK(stored->app_name == "Outlook");
    CHECK(stored->entry_kind == "dictation");
    CHECK_FALSE(stored->app_category);
    // No column holds the window title.
    const auto rows = db.column("SELECT count(*) FROM transcription_history WHERE "
                                "app_id LIKE '%Inbox%' OR app_name LIKE '%Inbox%' OR "
                                "source_name LIKE '%Inbox%' OR title LIKE '%Inbox%'");
    CHECK(rows == std::vector<std::string>{"0"});
}

// from
// tests/test_personalize_foundation_history.py::test_failed_recording_retry_replaces_the_row_and_keeps_the_audio
TEST_CASE("test_failed_recording_retry_replaces_the_row_and_keeps_the_audio",
          "[test_personalize_foundation_history]") {
    TestDatabase db;
    RecordingSync sync;
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                           services_with(&sync));
    const auto source = db.root() / "capture.wav";
    fv::history_test::write_file(source, "RIFF");
    fv::AddEntry fields;
    fields.text = "";
    fields.model = "remote";
    fields.source_audio_path = source;
    fields.source_name = "Quick Record";
    fields.audio_duration = 5.0;
    fields.columns = {{"entry_kind", std::string(fv::FAILED_RECORDING_KIND)}};
    const auto failed = manager.add_entry(fields).value();
    CHECK(sync.saved.empty());
    REQUIRE(failed.audio_file);
    const auto audio_path = manager.get_recording_path(*failed.audio_file);
    REQUIRE(audio_path);
    const auto found = manager.failed_recording_for_path(*audio_path);
    REQUIRE(found);
    CHECK(found->id == failed.id);
    CHECK_FALSE(manager.failed_recording_for_path(db.root() / *failed.audio_file));

    fv::FailedRecordingRetry retry;
    retry.text = "Recovered.";
    retry.raw_text = "recovered";
    retry.model = "local_whisper";
    retry.transcription_time = 0.5;
    retry.cleanup_provider = "test";
    retry.cleanup_model = "test";
    retry.context = {{"entry_kind", "file"}};
    const auto completed = manager.complete_failed_recording(failed.id, retry);

    CHECK(completed.timestamp == failed.timestamp);
    CHECK(completed.audio_file == failed.audio_file);
    CHECK(completed.audio_duration == failed.audio_duration);
    CHECK(completed.source_name == failed.source_name);
    CHECK(completed.cleaned_text == "Recovered.");
    CHECK(manager.get_entry(failed.id)->text == "Recovered.");
    CHECK_FALSE(manager.failed_recording_for_path(*audio_path));
    CHECK(sync.saved == std::vector<std::pair<std::string, std::string>>{{"dictation", failed.id}});
    CHECK_FALSE(
        db.repository().complete_failed_recording(failed.id, {{"text", std::string("Stale")}}));
    CHECK_THROWS_WITH(manager.complete_failed_recording(failed.id, retry),
                      "The failed recording entry is no longer in History");
}

// from tests/test_personalize_foundation_history.py::test_dictation_stats_rows_round_trip
TEST_CASE("test_dictation_stats_rows_round_trip", "[test_personalize_foundation_history]") {
    TestDatabase db;
    db.database().write([](fv::sql::Connection& conn) {
        conn.exec("INSERT INTO dictation_stats (entry_id, timestamp, entry_kind, app_name, "
                  "app_category, cleanup_level, spoken_words, final_words, words_edited, "
                  "audio_seconds, language, target, snippets, snippet_words, corrections, shots) "
                  "VALUES ('e1', '2026-10-06T10:00:00+00:00', 'dictation', 'Outlook', 'email', "
                  "'medium', 12, 11, 2, 4.5, 'en', NULL, 0, 0, 0, 0)");
    });
    fv::dictation_stats::StatsRepository stats(db.database());
    const auto rows = stats.rows();
    REQUIRE(rows.size() == 1);
    const auto& row = rows[0];
    CHECK(row.id == 1);
    CHECK(row.spoken_words == 12);
    CHECK(row.final_words == 11);
    CHECK(row.words_edited == 2);
    CHECK(row.audio_seconds == 4.5);
}

TEST_CASE("add_entry copies the source audio and keeps cleaned_text beside raw_text",
          "[history_manager]") {
    TestDatabase db;
    RecordingSync sync;
    const fv::FakeClock clock(fv::history_test::utc(2026, 11, 14, 18, 5, 9), "America/Los_Angeles");
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                           services_with(&sync, &clock));
    const auto source = db.root() / "recorded_audio.wav";
    fv::history_test::write_file(source, "RIFF....WAVE");
    fv::AddEntry fields;
    fields.text = "Hello, world.";
    fields.raw_text = "hello world";
    fields.model = "parakeet";
    fields.source_audio_path = source;
    fields.source_name = "Quick Record";
    fields.columns = {{"entry_kind", "dictation"}, {"language", "en"}, {"app_name", "Notepad"}};

    const auto entry = manager.add_entry(fields).value();

    CHECK(entry.audio_file == "recording_20261114_100509.wav");
    CHECK(fv::history_test::read_file(db.recordings() / *entry.audio_file) == "RIFF....WAVE");
    CHECK(entry.timestamp == "2026-11-14T18:05:09+00:00");
    const auto stored = manager.get_entry(entry.id);
    REQUIRE(stored);
    CHECK(*stored == entry);
    CHECK(stored->cleaned_text == "Hello, world.");
    CHECK(sync.saved == std::vector<std::pair<std::string, std::string>>{{"dictation", entry.id}});
    CHECK(manager.last_dictation()->id == entry.id);

    fields.source_audio_path = db.root() / "missing.wav";
    fields.raw_text.reset();
    const auto without_audio = manager.add_entry(fields).value();
    CHECK_FALSE(without_audio.audio_file);
    CHECK_FALSE(manager.get_entry(without_audio.id)->cleaned_text);
}

TEST_CASE("the last dictation survives a move to the host and is forgotten on delete and clear",
          "[history_manager]") {
    TestDatabase db;
    RecordingSync sync;
    int forgotten = 0;
    fv::HistoryServices services = services_with(&sync);
    services.forget_replayable_events = [&] {
        ++forgotten;
    };
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{}, services);
    fv::AddEntry fields;
    fields.text = "spoken";
    fields.model = "base";
    fields.source_name = "Quick Record";
    const auto saved = manager.add_entry(fields).value();

    CHECK(manager.delete_entry(saved.id, false, true));
    CHECK(forgotten == 0);
    const auto moved = manager.last_dictation();
    REQUIRE(moved);
    CHECK(moved->id == saved.id);
    CHECK(moved->stored_on == "the host");
    CHECK_FALSE(fv::is_local_entry(*moved));
    CHECK(sync.deleted ==
          std::vector<std::pair<std::string, std::string>>{{"dictation", saved.id}});

    const auto second = manager.add_entry(fields).value();
    CHECK(manager.delete_entry(second.id));
    CHECK(forgotten == 1);
    CHECK_FALSE(manager.delete_entry(second.id));
    CHECK_FALSE(manager.last_dictation());

    const auto third = manager.add_entry(fields).value();
    manager.clear_history();
    CHECK(forgotten == 2);
    CHECK_FALSE(manager.last_dictation());
    CHECK(sync.cleared_kinds == std::vector<std::string>{"dictation"});
    CHECK(manager.get_history().empty());
    (void)third;
}

TEST_CASE("no last dictation is offered while dictations move to the host", "[history_manager]") {
    TestDatabase db;
    db.add("old", "old", "2026-01-01T09:00:00", "base", {}, "Quick Record");
    fv::HistoryServices services;
    bool to_host = true;
    services.dictations_move_to_host = [&] {
        return to_host;
    };
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{}, services);
    CHECK_FALSE(manager.last_dictation());
    to_host = false;
    CHECK(manager.last_dictation()->id == "old");
}

TEST_CASE("clear_history_and_recordings removes every saved recording", "[history_manager]") {
    TestDatabase db;
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{});
    fv::history_test::write_file(db.recordings() / "recording_20260101_120000.wav", "a");
    fv::history_test::write_file(db.recordings() / "notes.txt", "keep");
    db.add("a", "a", "2026-01-01T09:00:00+00:00");
    db.add("theirs", "b", "2026-01-01T09:00:00+00:00");
    db.set_origin("theirs", "laptop");

    manager.clear_history_and_recordings();

    CHECK(fv::history_test::list_names(db.recordings()) == std::vector<std::string>{"notes.txt"});
    REQUIRE(manager.get_history().size() == 1);
    CHECK(manager.get_history()[0].id == "theirs");
}

TEST_CASE("preserve_recording copies standalone audio and refuses missing files",
          "[history_manager]") {
    TestDatabase db;
    const fv::FakeClock clock(fv::history_test::utc(2026, 5, 6, 7, 8, 9));
    HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                           services_with(nullptr, &clock));
    fv::history_test::write_file(db.root() / "failed.wav", "audio");
    CHECK(manager.preserve_recording(db.root() / "failed.wav") == "recording_20260506_070809.wav");
    CHECK_FALSE(manager.preserve_recording(db.root() / "gone.wav"));
    CHECK_FALSE(manager.preserve_recording(db.root()));
    CHECK_FALSE(manager.preserve_recording({}));
}

TEST_CASE("a failed copy saves the entry without audio and leaves no partial file",
          "[history_manager]") {
    TestDatabase db;
    const auto recordings = db.recordings();
    HistoryManager manager(db.database(), recordings, fv::RecordingRetention{});
    std::filesystem::remove_all(recordings);
    fv::history_test::write_file(db.root() / "capture.wav", "RIFF");
    fv::AddEntry fields;
    fields.text = "kept";
    fields.model = "base";
    fields.source_audio_path = db.root() / "capture.wav";

    const auto entry = manager.add_entry(fields).value();

    CHECK_FALSE(entry.audio_file);
    CHECK(manager.get_entry(entry.id)->text == "kept");
    CHECK_FALSE(std::filesystem::exists(recordings));
}
