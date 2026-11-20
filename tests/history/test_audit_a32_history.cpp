// The a32 checklist's HI-01 search claim: History search matches the transcript, the raw text,
// the timestamp, the app, the source (file) name and the title, case-insensitively
// (database.py search_history_entries' seven ilike columns); and ST-04's: Stats are kept apart
// from History, so clearing History leaves them.
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "history/dictation_stats.h"
#include "history/entry.h"
#include "history/manager.h"
#include "history/repository.h"
#include "history_test_support.h"

using fv::HistoryEntry;
using fv::history_test::TestDatabase;

namespace {

HistoryEntry plain(std::string id, std::string timestamp) {
    HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = "nothing to see";
    entry.timestamp = std::move(timestamp);
    entry.model = "parakeet";
    return entry;
}

std::vector<std::string> found(TestDatabase& db, std::string_view query) {
    std::vector<std::string> ids;
    for (const HistoryEntry& entry : db.repository().search_history_entries(query, 50)) {
        ids.push_back(entry.id);
    }
    return ids;
}

} // namespace

TEST_CASE("a32 HI-01: History search reads text, raw text, time, app, file name and title",
          "[unit]") {
    TestDatabase db;
    HistoryEntry text = plain("by-text", "2026-01-01T09:00:00+00:00");
    text.text = "Quarterly Budget review";
    HistoryEntry raw = plain("by-raw", "2026-01-02T09:00:00+00:00");
    raw.raw_text = "um the zebrafish thing";
    HistoryEntry when = plain("by-time", "2026-07-14T09:00:00+00:00");
    HistoryEntry app = plain("by-app", "2026-01-03T09:00:00+00:00");
    app.app_name = "Thunderbird";
    HistoryEntry file = plain("by-file", "2026-01-04T09:00:00+00:00");
    file.source_name = "interview-take2.wav";
    HistoryEntry title = plain("by-title", "2026-01-05T09:00:00+00:00");
    title.title = "Launch plan notes";
    for (const HistoryEntry* entry : {&text, &raw, &when, &app, &file, &title}) {
        // put_history_entry stores every column (add_history_entry leaves the title NULL).
        db.repository().put_history_entry(*entry);
    }

    CHECK(found(db, "budget") == std::vector<std::string>{"by-text"});
    CHECK(found(db, "ZEBRAFISH") == std::vector<std::string>{"by-raw"});
    CHECK(found(db, "2026-07-14") == std::vector<std::string>{"by-time"});
    CHECK(found(db, "thunder") == std::vector<std::string>{"by-app"});
    CHECK(found(db, "take2") == std::vector<std::string>{"by-file"});
    CHECK(found(db, "launch plan") == std::vector<std::string>{"by-title"});
    CHECK(found(db, "nothing like it").empty());
}

TEST_CASE("a32 ST-04: clearing History leaves the Stats", "[unit]") {
    TestDatabase db;
    fv::dictation_stats::StatsRepository stats(db.database());
    fv::HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{});
    fv::AddEntry fields;
    fields.text = "four words said here";
    fields.model = "parakeet";
    REQUIRE(manager.add_entry(fields));
    fv::dictation_stats::EntryFields counted;
    counted.text = "four words said here";
    counted.entry_kind = "dictation";
    counted.timestamp = "2026-11-19T12:00:00+00:00";
    stats.record(counted, nullptr);

    manager.clear_history();
    manager.clear_history_and_recordings();

    CHECK(db.repository().search_history_entries("", 10).empty());
    std::int64_t words = 0;
    for (const auto& row : stats.rows()) {
        if (row.entry_kind == "dictation") {
            words += row.spoken_words;
        }
    }
    CHECK(words == 4);
}
