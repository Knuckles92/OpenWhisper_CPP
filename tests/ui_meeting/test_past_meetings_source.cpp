// The port Past Meetings reads through, over a SqlMeetingRepository on a temporary database, and
// the panel listing what it holds. The database blocks, so every call runs off the GUI thread.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QLabel>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "meeting_state/sql_repository.h"
#include "past_meetings_support.h"
#include "testkit/testkit.h"
#include "ui_meeting/past_meetings_source.h"

using fv::Json;
using fvtest::offGui;

namespace {

void addMeeting(fv::meeting_state::SqlMeetingRepository& repository, const std::string& id,
                const std::string& title, const std::string& status, const std::string& started) {
    repository.create_meeting(
        {{"id", id},
         {"title", title},
         {"status", status},
         {"started_at", started},
         {"ended_at", status == "ended" ? Json("2026-03-14T10:12:00") : Json()},
         {"host_token", "host"},
         {"guest_token", "guest"},
         {"cloud_enabled", false},
         {"spool_dir", ""}});
}

// Declared first, so the database closes before the folder is removed.
struct Repository {
    Repository() {
        offGui([this] {
            database = std::make_unique<fv::Database>(
                fv::Database::open_or_create(root.path() / "meetings.db"));
            repository =
                std::make_shared<fv::meeting_state::SqlMeetingRepository>(*database, clock);
            addMeeting(*repository, "m_plan", "Planning review", "ended", "2026-03-14T09:30:00");
            addMeeting(*repository, "m_stand", "Standup", "ended", "2026-03-13T08:30:00");
            addMeeting(*repository, "m_live", "Running now", "active", "2026-03-15T09:00:00");
        });
    }
    ~Repository() {
        offGui([this] {
            repository.reset();
            database->close();
        });
    }
    Repository(const Repository&) = delete;
    Repository& operator=(const Repository&) = delete;

    fv::testkit::TempDataRoot root;
    fv::FakeClock clock;
    std::unique_ptr<fv::Database> database;
    std::shared_ptr<fv::meeting_state::SqlMeetingRepository> repository;
};

std::vector<std::string> idsOf(const std::vector<Json>& rows) {
    std::vector<std::string> ids;
    for (const Json& row : rows) {
        ids.push_back(row.at("id").get<std::string>());
    }
    return ids;
}

} // namespace

TEST_CASE("the SQL source lists past meetings newest first with their content summary",
          "[past_meetings_source]") {
    Repository db;
    fv::SqlPastMeetingsSource source(db.repository);

    const std::vector<Json> rows =
        offGui([&] { return source.list_past_meeting_summaries(101, ""); });

    CHECK(idsOf(rows) == std::vector<std::string>{"m_plan", "m_stand"});
    for (const Json& row : rows) {
        CHECK(row.contains("content_summary"));
    }
}

TEST_CASE("the SQL source matches the query by the source", "[past_meetings_source]") {
    Repository db;
    fv::SqlPastMeetingsSource source(db.repository);

    CHECK(idsOf(offGui([&] { return source.list_past_meeting_summaries(101, "STAND"); })) ==
          std::vector<std::string>{"m_stand"});
    CHECK(
        offGui([&] { return source.list_past_meeting_summaries(101, "nothing like it"); }).empty());
    CHECK(offGui([&] { return source.list_past_meeting_summaries(1, ""); }).size() == 1);
}

TEST_CASE("the SQL source answers the content summary of one meeting and every start time",
          "[past_meetings_source]") {
    Repository db;
    fv::SqlPastMeetingsSource source(db.repository);

    const Json summary = offGui([&] { return source.get_meeting_content_summary("m_plan"); });
    const auto times = offGui([&] { return source.list_past_meeting_times(); });

    CHECK(summary.at("has_audio") == false);
    CHECK(summary.at("is_empty") == true);
    REQUIRE(times.size() == 2);
    CHECK(times[0].id == "m_plan");
}

TEST_CASE("the panel lists what the repository holds, off the GUI thread",
          "[past_meetings_source][widget]") {
    Repository db;
    auto source = std::make_shared<fv::SqlPastMeetingsSource>(db.repository);
    fvtest::Io io;
    fv::PastMeetingsOptions options;
    options.summarize = [source](const std::string& id) {
        return source->get_meeting_content_summary(id);
    };
    fv::PastMeetingsPanel panel(fv::pastMeetingsProvider(source), io.executor(), options);

    panel.refresh();
    io.drain();

    std::vector<std::string> shown;
    for (fv::PastMeetingItem* card : panel.cards()) {
        shown.push_back(card->meetingId().toStdString());
    }
    CHECK(shown == std::vector<std::string>{"m_plan", "m_stand"});
    CHECK(panel.sectionHeader()->text() == QStringLiteral("2 meetings"));
}
