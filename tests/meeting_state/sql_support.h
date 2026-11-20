#pragma once

// What the SQL repository and store tests share: OpenWhisper's `repo` and `db` fixtures (a fresh
// database in a temporary data folder) and tests/helpers.py make_meeting and make_segment.

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/testing/fake_clock.h"
#include "db/database.h"
#include "meeting_state/schema.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/testing/fixed_ids.h"
#include "meeting_state/types.h"
#include "testkit/testkit.h"

namespace fv::meeting_state::test_support {

struct SqlFixture {
    SqlFixture()
        : database(
              std::make_unique<Database>(Database::open_or_create(root.path() / "openwhisper.db"))),
          repo(std::make_shared<SqlMeetingRepository>(*database, clock)) {}

    std::filesystem::path db_path() const { return root.path() / "openwhisper.db"; }

    // Declared first, so the database closes before the folder is removed.
    fv::testkit::TempDataRoot root;
    fv::FakeClock clock;
    testing::FixedIds ids;
    std::unique_ptr<Database> database;
    std::shared_ptr<SqlMeetingRepository> repo;
};

// A bare active meeting row (tests/helpers.py make_meeting).
inline std::string make_meeting(SqlMeetingRepository& repo,
                                const std::string& meeting_id = "m_test1") {
    repo.create_meeting({{"id", meeting_id},
                         {"title", "Test meeting"},
                         {"status", "active"},
                         {"started_at", "2026-01-01T09:00:00.000001"},
                         {"host_token", "host-token"},
                         {"guest_token", "guest-token"},
                         {"cloud_enabled", false},
                         {"spool_dir", "/tmp/spool"}});
    return meeting_id;
}

// A transcript segment, mic channel by default (tests/helpers.py make_segment).
inline TranscriptSegment make_segment(const std::string& meeting_id,
                                      const std::string& segment_id = "sg_aaa111",
                                      double start = 1.0, double end = 3.0,
                                      const std::string& text = "hello world",
                                      const std::string& channel = "mic") {
    TranscriptSegment segment;
    segment.segment_id = segment_id;
    segment.meeting_id = meeting_id;
    segment.channel = channel;
    segment.start_s = start;
    segment.end_s = end;
    segment.text = text;
    return segment;
}

inline MeetingState state_for(const std::string& meeting_id) {
    MeetingState state;
    state.meeting_id = meeting_id;
    return state;
}

inline std::vector<std::string> ids_of(const std::vector<Json>& rows, const char* key = "id") {
    std::vector<std::string> ids;
    for (const auto& row : rows) {
        ids.push_back(row.at(key).get<std::string>());
    }
    return ids;
}

// One column of every row a query returns, as JSON.
inline std::vector<Json> column_values(Database& database, const std::string& sql) {
    return database.read([&](sql::Connection& connection) {
        sql::Statement statement = connection.prepare(sql);
        std::vector<Json> values;
        while (statement.step()) {
            const sql::Value value = statement.column(0);
            if (const auto* text = std::get_if<std::string>(&value)) {
                values.emplace_back(*text);
            } else if (const auto* integer = std::get_if<std::int64_t>(&value)) {
                values.emplace_back(*integer);
            } else if (const auto* real = std::get_if<double>(&value)) {
                values.emplace_back(*real);
            } else {
                values.emplace_back(nullptr);
            }
        }
        return values;
    });
}

} // namespace fv::meeting_state::test_support
