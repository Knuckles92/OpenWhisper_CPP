// Durable insertion-order delivery to the meeting agent (tests/test_meeting_delivery_cursor.py).
// OpenWhisper drives these through its CheckpointScheduler (the meeting agent's); here a pass is
// what the scheduler asks of the repository: the page after the consumer's cursor, and on
// success the cursor moved to the page's last delivery_seq.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers.hpp>

#include <algorithm>
#include <latch>
#include <string>
#include <thread>
#include <vector>

#include "sql_support.h"

using fv::Json;
using namespace fv::meeting_state;
using namespace fv::meeting_state::test_support;

namespace {

// One delivery pass of `consumer`: the ids it would hand the agent. A failed pass leaves the
// cursor where it was, so the same page is replayed.
std::vector<std::string> deliver(SqlMeetingRepository& repo, const std::string& meeting_id,
                                 const std::string& consumer, bool succeeds = true) {
    const auto page = repo.get_segments_delivery_page(
        meeting_id, repo.get_delivery_cursor(meeting_id, consumer), 300);
    if (succeeds && !page.empty()) {
        repo.advance_delivery_cursor(meeting_id, consumer,
                                     page.back()["delivery_seq"].get<std::int64_t>());
    }
    return ids_of(page);
}

void exec(fv::Database& database, const std::string& sql) {
    database.write([&](fv::sql::Connection& connection) { connection.exec(sql); });
}

} // namespace

// from
// tests/test_meeting_delivery_cursor.py::test_delayed_old_start_segment_arrives_after_cursor_and_survives_reopen
TEST_CASE("test_delayed_old_start_segment_arrives_after_cursor_and_survives_reopen",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "newer", 600, 602)});
    CHECK(deliver(*f.repo, meeting_id, "cards") == std::vector<std::string>{"newer"});
    f.repo->advance_delivery_cursor(meeting_id, "notes", 1);

    // Its speech time is over 180 seconds behind the latest sent segment, but it was committed
    // later, so it comes next.
    f.repo->add_segments({make_segment(meeting_id, "late", 1, 3)});
    CHECK(deliver(*f.repo, meeting_id, "cards") == std::vector<std::string>{"late"});
    CHECK(deliver(*f.repo, meeting_id, "notes") == std::vector<std::string>{"late"});

    {
        fv::Database reopened = fv::Database::open_or_create(f.db_path());
        SqlMeetingRepository durable(reopened);
        CHECK(durable.get_delivery_cursor(meeting_id, "cards") == 2);
        CHECK(durable.get_delivery_cursor(meeting_id, "notes") == 2);
    }

    // A speaker update does not create a new delivery event; a new consumer resumes from the
    // durable acknowledgement.
    f.repo->update_segment_speaker(meeting_id, "late", std::nullopt, "human", true);
    CHECK(deliver(*f.repo, meeting_id, "cards").empty());
    CHECK(f.repo->get_segments_delivery_page(meeting_id, 2, 300).empty());
}

// from
// tests/test_meeting_delivery_cursor.py::test_delivery_pages_are_bounded_and_failed_pass_is_replayed
TEST_CASE("test_delivery_pages_are_bounded_and_failed_pass_is_replayed",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    std::vector<TranscriptSegment> segments;
    for (int index = 0; index < 625; ++index) {
        segments.push_back(make_segment(meeting_id, "sg_" + std::to_string(index),
                                        static_cast<double>(index), index + 2.0));
    }
    f.repo->add_segments(segments);

    std::vector<std::size_t> passes;
    passes.push_back(deliver(*f.repo, meeting_id, "cards", false).size());
    CHECK(f.repo->get_delivery_cursor(meeting_id, "cards") == 0);
    passes.push_back(deliver(*f.repo, meeting_id, "cards").size());
    CHECK(passes == std::vector<std::size_t>{300, 300});
    CHECK(f.repo->get_delivery_cursor(meeting_id, "cards") == 300);
    passes.push_back(deliver(*f.repo, meeting_id, "cards").size());
    passes.push_back(deliver(*f.repo, meeting_id, "cards").size());
    CHECK(passes == std::vector<std::size_t>{300, 300, 300, 25});
    CHECK(f.repo->get_delivery_cursor(meeting_id, "cards") == 625);
    CHECK(f.repo->get_segments_delivery_page(meeting_id, 625, 300).empty());
    // The page size is clamped to 1..301.
    CHECK(f.repo->get_segments_delivery_page(meeting_id, 0, 5000).size() == 301);
    CHECK(f.repo->get_segments_delivery_page(meeting_id, 0, 0).size() == 1);
}

// from
// tests/test_meeting_delivery_cursor.py::test_delivery_sequence_does_not_reuse_deleted_highest_row
TEST_CASE("test_delivery_sequence_does_not_reuse_deleted_highest_row",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "first", 500, 502)});
    const auto first = f.repo->get_segments_delivery_page(meeting_id, 0, 300)[0]["delivery_seq"]
                           .get<std::int64_t>();
    exec(*f.database, "DELETE FROM meeting_segments WHERE id='first'");
    f.repo->add_segments({make_segment(meeting_id, "late", 1, 3)});
    const auto page = f.repo->get_segments_delivery_page(meeting_id, first, 300);
    CHECK(ids_of(page) == std::vector<std::string>{"late"});
    CHECK(page[0]["delivery_seq"].get<std::int64_t>() > first);
}

// from
// tests/test_meeting_delivery_cursor.py::test_deleted_segment_redecoded_with_same_id_gets_new_delivery
TEST_CASE("test_deleted_segment_redecoded_with_same_id_gets_new_delivery",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments({make_segment(meeting_id, "redecoded", 500, 502)});
    const auto first = f.repo->get_segments_delivery_page(meeting_id, 0, 300)[0]["delivery_seq"]
                           .get<std::int64_t>();
    f.repo->advance_delivery_cursor(meeting_id, "cards", first);
    exec(*f.database, "DELETE FROM meeting_segments WHERE id='redecoded'");
    f.repo->add_segments({make_segment(meeting_id, "redecoded", 1, 3)});
    const auto page = f.repo->get_segments_delivery_page(
        meeting_id, f.repo->get_delivery_cursor(meeting_id, "cards"), 300);
    CHECK(ids_of(page) == std::vector<std::string>{"redecoded"});
    CHECK(page[0]["delivery_seq"].get<std::int64_t>() > first);
}

// from
// tests/test_meeting_delivery_cursor.py::test_concurrent_cursor_advancement_is_atomic_and_monotonic
TEST_CASE("test_concurrent_cursor_advancement_is_atomic_and_monotonic",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    const std::vector<std::int64_t> sequence = {8, 2, 6, 1, 7, 3, 5, 4};
    std::latch start(static_cast<std::ptrdiff_t>(sequence.size()));
    std::vector<std::thread> workers;
    for (const std::int64_t value : sequence) {
        workers.emplace_back([&, value] {
            start.arrive_and_wait();
            f.repo->advance_delivery_cursor(meeting_id, "cards", value);
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }
    CHECK(f.repo->get_delivery_cursor(meeting_id, "cards") == 8);
    f.repo->advance_delivery_cursor(meeting_id, "cards", 1);
    CHECK(f.repo->get_delivery_cursor(meeting_id, "cards") == 8);
}

// from
// tests/test_meeting_delivery_cursor.py::test_restart_drains_notes_backlog_when_cards_are_caught_up
TEST_CASE("test_restart_drains_notes_backlog_when_cards_are_caught_up",
          "[test_meeting_delivery_cursor]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    f.repo->add_segments(
        {make_segment(meeting_id, "older", 600, 602), make_segment(meeting_id, "late", 1, 3)});
    const std::int64_t high_water = f.repo->get_delivery_high_water(meeting_id);
    CHECK(high_water == 2);
    f.repo->advance_delivery_cursor(meeting_id, "cards", high_water);

    CHECK(deliver(*f.repo, meeting_id, "cards").empty());
    // Commit order; the notes pass orders its segments by speech time itself.
    CHECK(deliver(*f.repo, meeting_id, "notes") == std::vector<std::string>{"older", "late"});
    CHECK(f.repo->get_delivery_cursor(meeting_id, "notes") == high_water);
}

// from
// tests/test_meeting_delivery_cursor.py::test_upgrade_backfills_existing_segments_once_in_row_order
TEST_CASE("test_upgrade_backfills_existing_segments_once_in_row_order",
          "[test_meeting_delivery_cursor]") {
    const fv::testkit::TempDataRoot root;
    const auto path = root.path() / "openwhisper.db";
    const std::string meeting_id = "m_test1";
    {
        fv::Database database = fv::Database::open_or_create(path);
        SqlMeetingRepository repo(database);
        make_meeting(repo, meeting_id);
        repo.add_segments({make_segment(meeting_id, "original", 700, 702),
                           make_segment(meeting_id, "older", 2, 4)});
        exec(database, "DROP TRIGGER meeting_segments_delivery_ai");
        exec(database, "DROP TABLE meeting_segment_delivery");
        exec(database, "UPDATE schema_version SET version=15");
    }
    std::vector<std::int64_t> seqs;
    {
        fv::Database upgraded = fv::Database::open_or_create(path);
        const auto page =
            SqlMeetingRepository(upgraded).get_segments_delivery_page(meeting_id, 0, 300);
        CHECK(ids_of(page) == std::vector<std::string>{"original", "older"});
        for (const auto& row : page) {
            seqs.push_back(row["delivery_seq"].get<std::int64_t>());
        }
        CHECK(std::is_sorted(seqs.begin(), seqs.end()));
    }
    fv::Database reopened = fv::Database::open_or_create(path);
    std::vector<std::int64_t> again;
    for (const auto& row :
         SqlMeetingRepository(reopened).get_segments_delivery_page(meeting_id, 0, 300)) {
        again.push_back(row["delivery_seq"].get<std::int64_t>());
    }
    CHECK(again == seqs);
}

TEST_CASE("delivery cursors refuse unknown consumers and negative sequences",
          "[meeting_state_store]") {
    SqlFixture f;
    const std::string meeting_id = make_meeting(*f.repo);
    CHECK_THROWS_WITH(f.repo->get_delivery_cursor(meeting_id, "other"),
                      "unknown transcript delivery consumer");
    CHECK_THROWS_WITH(f.repo->advance_delivery_cursor(meeting_id, "notes", -1),
                      "invalid transcript delivery cursor");
    CHECK_THROWS_WITH(f.repo->advance_delivery_cursor(meeting_id, "other", 1),
                      "invalid transcript delivery cursor");
    CHECK(f.repo->get_delivery_high_water(meeting_id) == 0);
}
