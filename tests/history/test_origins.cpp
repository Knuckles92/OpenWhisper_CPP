// The entries paired computers store on this one (services/database.py's origin queries):
// tests/test_remote_records.py's host-side clear test, and the origin counts, renames and
// lookups the record kinds build on.

#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "history/entry.h"
#include "history/manager.h"
#include "history/repository.h"
#include "history_test_support.h"

using fv::HistoryEntry;
using fv::HistoryOrigin;
using fv::OriginCount;
using fv::history_test::RecordingSync;
using fv::history_test::services_with;
using fv::history_test::TestDatabase;

namespace {

// What the host stores for a paired computer's entry (HistoryRecords.import_record):
// every column, the device's id and name included.
HistoryEntry paired_entry(std::string id, std::optional<std::string> device,
                          std::optional<std::string> name, std::string text = "from afar") {
    HistoryEntry entry;
    entry.id = std::move(id);
    entry.text = std::move(text);
    entry.timestamp = "2026-01-02T09:00:00+00:00";
    entry.model = "parakeet-tdt-0.6b-v3";
    entry.source_name = "Quick Record";
    entry.origin_device_id = std::move(device);
    entry.origin_device_name = std::move(name);
    return entry;
}

std::vector<std::string> ids(const std::vector<HistoryEntry>& entries) {
    std::vector<std::string> result;
    for (const auto& entry : entries) {
        result.push_back(entry.id);
    }
    return result;
}

} // namespace

// from tests/test_remote_records.py::test_the_hosts_clear_keeps_paired_computers_entries
TEST_CASE("test_the_hosts_clear_keeps_paired_computers_entries", "[test_remote_records]") {
    // The paired client's move to the host (client.run_once) is M9.14's: here the host already
    // holds the entry as that move stores it.
    TestDatabase host;
    host.add("own", "the host's own", "2026-01-01T09:00:00+00:00");
    host.repository().put_history_entry(paired_entry("moved", "dev-laptop", "Laptop"));

    host.repository().clear_history();

    REQUIRE(host.repository().get_history_entry_by_id("moved"));
    CHECK_FALSE(host.repository().get_history_entry_by_id("own"));

    // History's own clear goes through the same query.
    RecordingSync sync;
    fv::HistoryManager manager(host.database(), host.recordings(), fv::RecordingRetention{},
                               services_with(&sync));
    host.add("own2", "again", "2026-01-03T09:00:00+00:00");
    manager.clear_history();
    CHECK(ids(manager.get_history()) == std::vector<std::string>{"moved"});
    CHECK(sync.cleared_kinds == std::vector<std::string>{"dictation"});
}

TEST_CASE("origin counts and renames cover each paired computer's entries", "[history_origins]") {
    TestDatabase db;
    auto& repository = db.repository();
    db.add("own1", "mine", "2026-01-01T09:00:00+00:00");
    db.add("own2", "mine too", "2026-01-01T10:00:00+00:00");
    repository.put_history_entry(paired_entry("l1", "dev-laptop", "Laptop"));
    repository.put_history_entry(paired_entry("l2", "dev-laptop", "Old laptop name"));
    repository.put_history_entry(paired_entry("l3", "dev-laptop", std::nullopt));
    repository.put_history_entry(paired_entry("d1", "dev-desk", std::nullopt));

    // Grouped by device in SQLite's group order; this computer's own entries are not counted.
    // name is the greatest stored name ("" when there is none).
    CHECK(repository.history_origin_counts() ==
          std::vector<OriginCount>{{"dev-desk", "", 1}, {"dev-laptop", "Old laptop name", 3}});

    CHECK(repository.rename_history_origin("dev-laptop", "Kitchen laptop") == 3);
    CHECK(repository.rename_history_origin("dev-unknown", "Nobody") == 0);
    CHECK(repository.history_origin_counts() ==
          std::vector<OriginCount>{{"dev-desk", "", 1}, {"dev-laptop", "Kitchen laptop", 3}});
    // Only the name changes: ids, origins and this computer's entries stay.
    const auto renamed = repository.get_history_entry_by_id("l3");
    REQUIRE(renamed);
    CHECK(renamed->origin_device_id == "dev-laptop");
    CHECK(renamed->origin_device_name == "Kitchen laptop");
    CHECK(renamed->text == "from afar");
    CHECK_FALSE(repository.get_history_entry_by_id("own1")->origin_device_name);
    CHECK(ids(repository.get_history_entries(std::nullopt, HistoryOrigin::device("dev-laptop")))
              .size() == 3);
    // Search finds a paired computer's entries by its new name.
    CHECK(repository.search_history_entries("kitchen").size() == 3);

    TestDatabase empty;
    CHECK(empty.repository().history_origin_counts().empty());
}

TEST_CASE("an entry looked up by id and origin belongs to that origin", "[history_origins]") {
    TestDatabase db;
    auto& repository = db.repository();
    db.add("own", "mine", "2026-01-01T09:00:00+00:00");
    repository.put_history_entry(paired_entry("theirs", "dev-laptop", "Laptop"));

    CHECK(repository.get_history_entry_by_id("own", HistoryOrigin::any())->id == "own");
    CHECK(repository.get_history_entry_by_id("own", HistoryOrigin::own())->id == "own");
    CHECK_FALSE(repository.get_history_entry_by_id("own", HistoryOrigin::device("dev-laptop")));

    CHECK(repository.get_history_entry_by_id("theirs", HistoryOrigin::any())->id == "theirs");
    CHECK_FALSE(repository.get_history_entry_by_id("theirs", HistoryOrigin::own()));
    const auto theirs =
        repository.get_history_entry_by_id("theirs", HistoryOrigin::device("dev-laptop"));
    REQUIRE(theirs);
    CHECK(*theirs == *repository.get_history_entry_by_id("theirs"));
    CHECK_FALSE(repository.get_history_entry_by_id("theirs", HistoryOrigin::device("dev-desk")));

    CHECK_FALSE(repository.get_history_entry_by_id("missing", HistoryOrigin::any()));
}

TEST_CASE("a paired computer's entries never count as this computer's dictations",
          "[history_origins]") {
    // The host side of record sync (A09-11, A09-14, A09-16): saving here reports the dictation,
    // History keeps paired computers' entries out of its own lists, and a copy kept on the host
    // is offered as the last dictation without being changed.
    TestDatabase db;
    RecordingSync sync;
    fv::HistoryManager manager(db.database(), db.recordings(), fv::RecordingRetention{},
                               services_with(&sync));
    db.repository().put_history_entry(paired_entry("theirs", "dev-laptop", "Laptop"));
    CHECK_FALSE(manager.last_dictation());

    fv::AddEntry fields;
    fields.text = "Cleaned.";
    fields.raw_text = "cleaned";
    fields.model = "base";
    fields.source_name = "Quick Record";
    const auto saved = manager.add_entry(fields).value();
    CHECK(sync.saved == std::vector<std::pair<std::string, std::string>>{{"dictation", saved.id}});

    // The move to the host finished: the row here goes, the host keeps it.
    CHECK(manager.delete_entry(saved.id, false, true));
    const auto last = manager.last_dictation();
    REQUIRE(last);
    CHECK(last->stored_on == "the host");
    CHECK_FALSE(manager.use_version(saved.id, fv::ORIGINAL_VERSION));
    CHECK(sync.edited.empty());

    fields.columns = {{"entry_kind", std::string(fv::FAILED_RECORDING_KIND)}};
    manager.add_entry(fields);
    // A failed recording keeps its audio here: it is never synced.
    CHECK(sync.saved.size() == 1);
}
