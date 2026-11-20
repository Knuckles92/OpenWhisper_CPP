// The scheduler on the durable delivery ledger of the SQL repository
// (tests/test_meeting_delivery_cursor.py, the tests that drive CheckpointScheduler; the repository
// halves are tests/meeting_state/test_delivery_cursor.cpp).

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/utf8.h"
#include "db/database.h"
#include "meeting_agent/scheduler.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/types.h"
#include "scheduler_support.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting_agent;
using namespace fv::meeting_agent::scheduler_test;
using namespace std::chrono_literals;
using fv::meeting_state::SqlMeetingRepository;
using fv::meeting_state::TranscriptSegment;
using P = SchedulerProbe;

namespace {

// The `repo` and `db` fixtures: a fresh database in a temporary data folder.
struct Sql {
    Sql()
        : database(std::make_unique<fv::Database>(fv::Database::open_or_create(db_path()))),
          repo(std::make_shared<SqlMeetingRepository>(*database, clock)) {}
    std::filesystem::path db_path() const {
        return root.path() / fv::path_from_utf8(fv::identity().database_file);
    }

    // Declared first, so the database closes before the folder is removed.
    fv::testkit::TempDataRoot root;
    fv::FakeClock clock;
    std::unique_ptr<fv::Database> database;
    std::shared_ptr<SqlMeetingRepository> repo;
};

std::string make_meeting(SqlMeetingRepository& repo) {
    repo.create_meeting({{"id", "m_test1"},
                         {"title", "Test meeting"},
                         {"status", "active"},
                         {"started_at", "2026-01-01T09:00:00.000001"},
                         {"host_token", "host-token"},
                         {"guest_token", "guest-token"},
                         {"cloud_enabled", false},
                         {"spool_dir", "/tmp/spool"}});
    return "m_test1";
}

TranscriptSegment make_segment(const std::string& meeting_id, const std::string& id, double start) {
    TranscriptSegment segment;
    segment.segment_id = id;
    segment.meeting_id = meeting_id;
    segment.channel = "mic";
    segment.start_s = start;
    segment.end_s = start + 2.0;
    segment.text = "hello world";
    return segment;
}

// _RepositoryEngine: the transcript and the ledger from the repository, a store that is not the
// subject, and a meeting clock 700 s in.
class RepositoryHost final : public SchedulerHost {
public:
    RepositoryHost(std::shared_ptr<SqlMeetingRepository> repo, std::string meeting_id)
        : repo_(std::move(repo)), meeting_id_(std::move(meeting_id)) {}

    std::shared_ptr<fv::meeting_state::MeetingStateStore> store() override {
        return store_.store();
    }
    std::vector<Json> get_transcript(double after_start_s) override {
        return repo_->get_segments(meeting_id_, after_start_s, std::nullopt);
    }
    double meeting_clock_s() const override { return 700.0; }
    std::vector<Json> delivery_page(std::int64_t after_seq, int limit) override {
        return repo_->get_segments_delivery_page(meeting_id_, after_seq, limit);
    }
    std::int64_t delivery_cursor(std::string_view consumer) override {
        return repo_->get_delivery_cursor(meeting_id_, consumer);
    }
    void advance_delivery_cursor(std::string_view consumer, std::int64_t seq) override {
        repo_->advance_delivery_cursor(meeting_id_, consumer, seq);
    }
    std::int64_t delivery_high_water() override {
        if (fail_high_water) {
            throw std::runtime_error("database unavailable");
        }
        return repo_->get_delivery_high_water(meeting_id_);
    }
    void revoke_agent_writes() override {}

    bool fail_high_water = false;

private:
    std::shared_ptr<SqlMeetingRepository> repo_;
    std::string meeting_id_;
    FakeSchedulerHost store_; // only for its store
};

std::vector<CheckpointPayload> passes(const FakeNotesAgent& agent, bool notes) {
    std::vector<CheckpointPayload> out;
    for (const auto& payload : agent.calls()) {
        if (payload.is_notes == notes && !payload.is_polish) {
            out.push_back(payload);
        }
    }
    return out;
}

} // namespace

// from
// tests/test_meeting_delivery_cursor.py::test_delayed_old_start_segment_arrives_after_cursor_and_survives_reopen
TEST_CASE("test_delayed_old_start_segment_arrives_after_cursor_and_survives_reopen",
          "[test_meeting_delivery_cursor]") {
    Sql sql;
    const auto meeting_id = make_meeting(*sql.repo);
    sql.repo->add_segments({make_segment(meeting_id, "newer", 600)});
    RepositoryHost host(sql.repo, meeting_id);
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent;
    {
        CheckpointScheduler scheduler(host, agent.core, clock);
        P::fire(scheduler);
        REQUIRE_FALSE(passes(agent, false).empty());
        CHECK(ids_of(passes(agent, false).back().new_segments) ==
              std::vector<std::string>{"newer"});

        // Its speech time is over 180 seconds behind the latest sent segment.
        sql.repo->add_segments({make_segment(meeting_id, "late", 1)});
        P::fire(scheduler);
        CHECK(ids_of(passes(agent, false).back().new_segments) == std::vector<std::string>{"late"});
        P::set_successful_checkpoints(scheduler, P::successful_checkpoints(scheduler) + 2);
        P::maybe_fire_notes(scheduler);
        REQUIRE_FALSE(passes(agent, true).empty());
        CHECK(ids_of(passes(agent, true).back().new_segments) == std::vector<std::string>{"late"});
    }
    {
        fv::Database reopened = fv::Database::open_or_create(sql.db_path());
        SqlMeetingRepository durable(reopened);
        CHECK(durable.get_delivery_cursor(meeting_id, "cards") == 2);
        CHECK(durable.get_delivery_cursor(meeting_id, "notes") == 2);
    }

    // A speaker update does not create a new delivery event, and a new scheduler resumes from
    // the durable acknowledgement after reconnect.
    sql.repo->update_segment_speaker(meeting_id, "late", std::nullopt, "human", true);
    FakeNotesAgent fresh_agent;
    CheckpointScheduler fresh(host, fresh_agent.core, clock);
    P::fire(fresh);
    CHECK(passes(fresh_agent, false).empty());
    CHECK(sql.repo->get_segments_delivery_page(meeting_id, 2, 300).empty());
}

// from
// tests/test_meeting_delivery_cursor.py::test_delivery_pages_are_bounded_and_failed_pass_is_replayed
TEST_CASE("test_delivery_pages_are_bounded_and_failed_pass_is_replayed",
          "[test_meeting_delivery_cursor]") {
    Sql sql;
    const auto meeting_id = make_meeting(*sql.repo);
    std::vector<TranscriptSegment> segments;
    for (int index = 0; index < 625; ++index) {
        segments.push_back(make_segment(meeting_id, "sg_" + std::to_string(index), index));
    }
    sql.repo->add_segments(segments);
    RepositoryHost host(sql.repo, meeting_id);
    fv::FakeClock clock;
    start_late(clock);
    FakeNotesAgent agent(1);
    CheckpointScheduler scheduler(host, agent.core, clock);

    const auto sizes = [&] {
        std::vector<std::size_t> out;
        for (const auto& payload : passes(agent, false)) {
            out.push_back(payload.new_segments.size());
        }
        return out;
    };
    P::fire(scheduler);
    CHECK(sql.repo->get_delivery_cursor(meeting_id, "cards") == 0);
    P::fire(scheduler);
    CHECK(sizes() == std::vector<std::size_t>{300, 300});
    CHECK(sql.repo->get_delivery_cursor(meeting_id, "cards") == 300);
    P::fire(scheduler);
    P::fire(scheduler);
    CHECK(sizes() == std::vector<std::size_t>{300, 300, 300, 25});
    CHECK(sql.repo->get_delivery_cursor(meeting_id, "cards") == 625);
    CHECK(sql.repo->get_segments_delivery_page(meeting_id, 625, 300).empty());
}

// from tests/test_meeting_delivery_cursor.py::test_failed_backlog_read_does_not_start_scheduler
TEST_CASE("test_failed_backlog_read_does_not_start_scheduler", "[test_meeting_delivery_cursor]") {
    Sql sql;
    const auto meeting_id = make_meeting(*sql.repo);
    RepositoryHost host(sql.repo, meeting_id);
    host.fail_high_water = true;
    FakeNotesAgent agent;
    CheckpointScheduler scheduler(host, agent.core, fv::system_clock());
    try {
        scheduler.start();
        FAIL("start() did not throw");
    } catch (const std::runtime_error& error) {
        CHECK(std::string(error.what()) == "database unavailable");
    }
    CHECK_FALSE(P::has_worker(scheduler));
}

// from
// tests/test_meeting_delivery_cursor.py::test_restart_drains_notes_backlog_when_cards_are_caught_up
TEST_CASE("test_restart_drains_notes_backlog_when_cards_are_caught_up",
          "[test_meeting_delivery_cursor]") {
    // Python replaces the worker loop and runs one tick by hand; here the worker's own first tick
    // (start() wakes it for the notes backlog) is that tick.
    Sql sql;
    const auto meeting_id = make_meeting(*sql.repo);
    sql.repo->add_segments(
        {make_segment(meeting_id, "older", 600), make_segment(meeting_id, "late", 1)});
    const auto high_water = sql.repo->get_delivery_high_water(meeting_id);
    sql.repo->advance_delivery_cursor(meeting_id, "cards", high_water);
    RepositoryHost host(sql.repo, meeting_id);
    FakeNotesAgent agent;
    CheckpointScheduler scheduler(host, agent.core, fv::system_clock());
    scheduler.start();
    CHECK(agent.core->wait_for_passes(1, 3000ms));
    scheduler.stop();
    CHECK(passes(agent, false).empty());
    REQUIRE_FALSE(passes(agent, true).empty());
    CHECK(ids_of(passes(agent, true)[0].new_segments) == std::vector<std::string>{"late", "older"});
    CHECK(sql.repo->get_delivery_cursor(meeting_id, "notes") == high_water);
}
