// tests/test_database.py: the history CRUD and search tests (the schema and migration tests are
// tests/db's), plus the other history queries of services/database.py.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include "history/entry.h"
#include "history/repository.h"
#include "history_test_support.h"
#include "pyport/errors.h"

using fv::HistoryEntry;
using fv::history_test::TestDatabase;

namespace {

HistoryEntry make(std::string id, std::string text, std::string timestamp,
                  std::string model = "local_whisper") {
    HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = std::move(text);
    entry.timestamp = std::move(timestamp);
    entry.model = std::move(model);
    return entry;
}

std::vector<std::string> ids(const std::vector<HistoryEntry>& entries) {
    std::vector<std::string> found;
    for (const auto& entry : entries) {
        found.push_back(entry.id);
    }
    return found;
}

} // namespace

// from tests/test_database.py::TestDatabaseManager::test_history_crud
TEST_CASE("test_history_crud", "[test_database]") {
    TestDatabase db;
    auto& repository = db.repository();
    const std::string entry_id = fv::uuid4_string();
    HistoryEntry entry = make(entry_id, "Test transcription", "2026-11-14T10:00:00.000001");
    entry.audio_file = "test.wav";
    entry.transcription_time = 1.5;
    entry.audio_duration = 3.0;
    entry.file_size = 48000;
    repository.add_history_entry(entry);

    const auto entries = repository.get_history_entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].id == entry_id);
    CHECK(entries[0].text == "Test transcription");

    const auto stored = repository.get_history_entry_by_id(entry_id);
    REQUIRE(stored);
    CHECK(stored->model == "local_whisper");
    CHECK(*stored == entry);

    CHECK(repository.delete_history_entry(entry_id));

    CHECK_FALSE(repository.get_history_entry_by_id(entry_id));
}

// from
// tests/test_database.py::TestDatabaseManager::test_history_search_is_bounded_and_treats_wildcards_literally
TEST_CASE("test_history_search_is_bounded_and_treats_wildcards_literally", "[test_database]") {
    TestDatabase db;
    auto& repository = db.repository();
    repository.add_history_entry(make("entry-old", "ordinary notes", "2026-01-01T00:00:00+00:00"));
    repository.add_history_entry(
        make("entry-mid", "literal 100% result", "2026-02-01T00:00:00+00:00"));
    repository.add_history_entry(
        make("entry-new", "another 100% result", "2026-03-01T00:00:00+00:00"));

    const auto matches = repository.search_history_entries("100%", 1);

    CHECK(ids(matches) == std::vector<std::string>{"entry-new"});
    CHECK(repository.search_history_entries("100_", 10).empty());
}

// from tests/test_database.py::TestDatabaseManager::test_history_raw_text_crud
TEST_CASE("test_history_raw_text_crud", "[test_database]") {
    TestDatabase db;
    HistoryEntry entry = make(fv::uuid4_string(), "Fixed transcription.", "2026-11-14T10:00:00");
    entry.raw_text = "um fixed transcription";
    db.repository().add_history_entry(entry);

    const auto stored = db.repository().get_history_entry_by_id(entry.id);
    REQUIRE(stored);
    CHECK(stored->text == "Fixed transcription.");
    CHECK(stored->raw_text == "um fixed transcription");
}

// from tests/test_database.py::TestDatabaseManager::test_history_cleanup_metadata_crud
TEST_CASE("test_history_cleanup_metadata_crud", "[test_database]") {
    TestDatabase db;
    HistoryEntry entry = make(fv::uuid4_string(), "Fixed transcription.", "2026-11-14T10:00:00");
    entry.raw_text = "um fixed transcription";
    entry.cleanup_provider = "openai";
    entry.cleanup_model = "gpt-4.1-mini";
    db.repository().add_history_entry(entry);

    const auto stored = db.repository().get_history_entry_by_id(entry.id);
    REQUIRE(stored);
    CHECK(stored->cleanup_provider == "openai");
    CHECK(stored->cleanup_model == "gpt-4.1-mini");
}

// from tests/test_database.py::TestDatabaseManager::test_history_source_name_crud
TEST_CASE("test_history_source_name_crud", "[test_database]") {
    TestDatabase db;
    HistoryEntry entry = make(fv::uuid4_string(), "Hello", "2026-11-14T10:00:00");
    entry.source_name = "sample.wav";
    db.repository().add_history_entry(entry);

    const auto stored = db.repository().get_history_entry_by_id(entry.id);
    REQUIRE(stored);
    CHECK(stored->source_name == "sample.wav");
}

TEST_CASE("add_history_entry leaves title and origin NULL; put_history_entry keeps them",
          "[history_repository]") {
    TestDatabase db;
    auto& repository = db.repository();
    HistoryEntry entry = make("e1", "text", "2026-01-01T00:00:00+00:00");
    entry.title = "A title";
    entry.origin_device_id = "device-1";
    entry.origin_device_name = "Laptop";
    repository.add_history_entry(entry);
    auto stored = repository.get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK_FALSE(stored->title);
    CHECK_FALSE(stored->origin_device_id);

    repository.put_history_entry(entry);
    stored = repository.get_history_entry_by_id("e1");
    REQUIRE(stored);
    CHECK(*stored == entry);
    entry.text = "replaced";
    entry.title.reset();
    repository.put_history_entry(entry);
    CHECK(repository.get_history_entry_by_id("e1") == entry);
    CHECK(repository.get_history_entries().size() == 1);
}

TEST_CASE("listing filters by origin and limits; 0 means no limit", "[history_repository]") {
    TestDatabase db;
    auto& repository = db.repository();
    repository.add_history_entry(make("a", "a", "2026-01-01T00:00:00+00:00"));
    repository.add_history_entry(make("b", "b", "2026-01-02T00:00:00+00:00"));
    repository.add_history_entry(make("c", "c", "2026-01-03T00:00:00+00:00"));
    db.set_origin("b", "device-1");

    CHECK(ids(repository.get_history_entries()) == std::vector<std::string>{"c", "b", "a"});
    CHECK(ids(repository.get_history_entries(0)) == std::vector<std::string>{"c", "b", "a"});
    CHECK(ids(repository.get_history_entries(2)) == std::vector<std::string>{"c", "b"});
    CHECK(ids(repository.get_history_entries(std::nullopt, fv::HistoryOrigin::own())) ==
          std::vector<std::string>{"c", "a"});
    CHECK(
        ids(repository.get_history_entries(std::nullopt, fv::HistoryOrigin::device("device-1"))) ==
        std::vector<std::string>{"b"});
    CHECK(ids(repository.search_history_entries("  ", 2)) == std::vector<std::string>{"c", "b"});

    repository.clear_history();
    CHECK(ids(repository.get_history_entries()) == std::vector<std::string>{"b"});
}

TEST_CASE("range reads are oldest first in [start, end) and calendar rows carry no text",
          "[history_repository]") {
    TestDatabase db;
    auto& repository = db.repository();
    HistoryEntry first = make("a", "a", "2026-01-01T10:00:00+00:00");
    first.source_name = "Quick Record";
    first.audio_duration = 2.5;
    first.entry_kind = "dictation";
    repository.add_history_entry(first);
    repository.add_history_entry(make("b", "b", "2026-01-02T10:00:00"));
    repository.add_history_entry(make("c", "c", "2026-01-03T00:00:00+00:00"));

    CHECK(ids(repository.get_history_entries_between("2026-01-01", "2026-01-03")) ==
          std::vector<std::string>{"a", "b"});
    CHECK(ids(repository.get_history_entries_between("2026-01-02", "2026-01-03T00:00:00+00:00")) ==
          std::vector<std::string>{"b"});

    const auto rows = repository.history_calendar_rows();
    REQUIRE(rows.size() == 3);
    const fv::HistoryCalendarRow expected{
        "a", "2026-01-01T10:00:00+00:00", "Quick Record", std::nullopt, 2.5, "dictation"};
    CHECK(std::ranges::find(rows, expected) != rows.end());
}

TEST_CASE("failed recordings complete only while still local and failed", "[history_repository]") {
    TestDatabase db;
    auto& repository = db.repository();
    HistoryEntry failed = make("f", "", "2026-01-01T00:00:00+00:00");
    failed.audio_file = "recording_20260101_000000.wav";
    failed.entry_kind = std::string(fv::FAILED_RECORDING_KIND);
    repository.add_history_entry(failed);

    CHECK(repository.get_failed_recording("recording_20260101_000000.wav")->id == "f");
    CHECK_FALSE(repository.get_failed_recording("other.wav"));
    CHECK_THROWS_AS(repository.complete_failed_recording("f", {{"bogus", nullptr}}),
                    std::invalid_argument);

    const auto done = repository.complete_failed_recording(
        "f", {{"text", std::string("Done.")}, {"entry_kind", std::string("file")}});
    REQUIRE(done);
    CHECK(done->text == "Done.");
    CHECK(done->entry_kind == "file");
    CHECK_FALSE(repository.complete_failed_recording("f", {{"text", std::string("Again")}}));
    CHECK_FALSE(repository.get_failed_recording("recording_20260101_000000.wav"));
}

TEST_CASE("clear_history_audio_file clears every entry naming the file", "[history_repository]") {
    TestDatabase db;
    auto& repository = db.repository();
    for (const char* id : {"a", "b"}) {
        HistoryEntry entry = make(id, id, "2026-01-01T00:00:00+00:00");
        entry.audio_file = "x.wav";
        repository.add_history_entry(entry);
    }
    HistoryEntry other = make("c", "c", "2026-01-01T00:00:00+00:00");
    other.audio_file = "y.wav";
    repository.add_history_entry(other);

    repository.clear_history_audio_file("x.wav");

    CHECK_FALSE(repository.get_history_entry_by_id("a")->audio_file);
    CHECK_FALSE(repository.get_history_entry_by_id("b")->audio_file);
    CHECK(repository.get_history_entry_by_id("c")->audio_file == "y.wav");
}

TEST_CASE("add_history_entry refuses unknown context keys before writing", "[history_repository]") {
    TestDatabase db;
    try {
        db.repository().add_history_entry(make("e", "t", "2026-01-01T00:00:00+00:00"),
                                          {{"window_title", "Inbox"}, {"app_name", "x"}});
        FAIL("expected TypeError");
    } catch (const fv::py::TypeError& error) {
        CHECK(std::string(error.what()) == "Not history context columns: window_title");
    }
    CHECK(db.repository().get_history_entries().empty());
}
