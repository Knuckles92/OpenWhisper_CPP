// tests/test_meeting_data_lifecycle.py: recoverable deletion and bulk clear. The two playback
// tests are M7.12's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/utf8.h"
#include "meeting_state/data_lifecycle.h"
#include "pyport/errors.h"
#include "stub_repository.h"
#include "testkit/testkit.h"

using fv::Json;
using namespace fv::meeting_state;
using fv::meeting_state::test_support::StubRepository;
namespace fs = std::filesystem;

namespace {

void write_file(const fs::path& path, const std::string& bytes) {
    std::ofstream out(path, std::ios::binary);
    out << bytes;
}

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string utf8(const fs::path& path) {
    return fv::path_to_utf8(path);
}

// os.utime(path, (old, old)) with old two hours ago.
void age_two_hours(const fs::path& path) {
    fs::last_write_time(path, fs::file_time_type::clock::now() - std::chrono::hours{2});
}

std::vector<fs::path> entries(const fs::path& folder) {
    std::vector<fs::path> out;
    for (const auto& entry : fs::directory_iterator(folder)) {
        out.push_back(entry.path());
    }
    return out;
}

// LifecycleRepository: one meeting, a delete that may fail.
class LifecycleRepository : public StubRepository {
public:
    LifecycleRepository(std::optional<Json> meeting, bool fail_delete = false)
        : meeting(std::move(meeting)), fail_delete(fail_delete) {}

    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        if (meeting && (*meeting)["id"] == std::string(meeting_id)) {
            return *meeting;
        }
        return std::nullopt;
    }
    void delete_meeting(std::string_view) override {
        if (fail_delete) {
            throw std::runtime_error("database unavailable");
        }
        meeting.reset();
    }

    std::optional<Json> meeting;
    bool fail_delete;
};

// MultiLifecycleRepository: meetings by id, in insertion order.
class MultiLifecycleRepository final : public StubRepository {
public:
    explicit MultiLifecycleRepository(std::vector<Json> rows) : rows(std::move(rows)) {}

    std::optional<Json> get_meeting(std::string_view meeting_id) override {
        for (const auto& row : rows) {
            if (row["id"] == std::string(meeting_id)) {
                return Json(row);
            }
        }
        return std::nullopt;
    }
    std::vector<Json> list_meetings() override { return rows; }
    void delete_meeting(std::string_view meeting_id) override {
        std::erase_if(rows, [&](const Json& row) { return row["id"] == std::string(meeting_id); });
    }

    std::vector<std::string> ids() const {
        std::vector<std::string> out;
        for (const auto& row : rows) {
            out.push_back(row["id"].get<std::string>());
        }
        return out;
    }

    std::vector<Json> rows;
};

Json meeting_row(const std::string& id, const fs::path& spool, const std::string& status = "") {
    Json row = {{"id", id}, {"spool_dir", utf8(spool)}};
    if (!status.empty()) {
        row["status"] = status;
    }
    return row;
}

} // namespace

// from tests/test_meeting_data_lifecycle.py::test_delete_removes_database_and_spool
TEST_CASE("test_delete_removes_database_and_spool", "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_delete";
    fs::create_directories(spool);
    write_file(spool / "audio.wav", "audio");
    LifecycleRepository repo(meeting_row("m_delete", spool));
    std::vector<std::pair<std::string, bool>> told;
    CHECK(delete_meeting_data(repo, "m_delete", root, true, [&](std::string id, bool origin) {
        told.emplace_back(std::move(id), origin);
    }));
    CHECK_FALSE(repo.meeting.has_value());
    CHECK_FALSE(fs::exists(spool));
    CHECK(entries(root).empty());
    CHECK(told == std::vector<std::pair<std::string, bool>>{{"m_delete", false}});
}

// from tests/test_meeting_data_lifecycle.py::test_delete_restores_spool_when_database_delete_fails
TEST_CASE("test_delete_restores_spool_when_database_delete_fails",
          "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_delete";
    fs::create_directories(spool);
    write_file(spool / "audio.wav", "audio");
    LifecycleRepository repo(meeting_row("m_delete", spool), true);
    bool told = false;
    CHECK_THROWS_WITH(
        delete_meeting_data(repo, "m_delete", root, true, [&](std::string, bool) { told = true; }),
        "database unavailable");
    CHECK(fs::is_directory(spool));
    CHECK(read_file(spool / "audio.wav") == "audio");
    CHECK_FALSE(told);
}

// from
// tests/test_meeting_data_lifecycle.py::test_delete_refreshes_old_spool_timestamp_before_database_commit
TEST_CASE("test_delete_refreshes_old_spool_timestamp_before_database_commit",
          "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_delete";
    fs::create_directories(spool);
    write_file(spool / "audio.wav", "audio");
    age_two_hours(spool);
    class Inspecting final : public LifecycleRepository {
    public:
        Inspecting(Json meeting, fs::path root)
            : LifecycleRepository(std::move(meeting)), root(std::move(root)) {}
        void delete_meeting(std::string_view meeting_id) override {
            for (const auto& entry : fs::directory_iterator(root)) {
                if (utf8(entry.path().filename()).starts_with(".deleting-")) {
                    age_s = std::chrono::duration<double>(fs::file_time_type::clock::now() -
                                                          fs::last_write_time(entry.path()))
                                .count();
                }
            }
            LifecycleRepository::delete_meeting(meeting_id);
        }
        fs::path root;
        std::optional<double> age_s;
    } repo(meeting_row("m_delete", spool), root);
    CHECK(delete_meeting_data(repo, "m_delete", root));
    REQUIRE(repo.age_s.has_value());
    CHECK(*repo.age_s < 10);
}

// from tests/test_meeting_data_lifecycle.py::test_delete_rejects_paths_outside_configured_root
TEST_CASE("test_delete_rejects_paths_outside_configured_root", "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    fs::create_directories(root);
    const fs::path outside = tmp.path() / "outside";
    fs::create_directories(outside);
    LifecycleRepository repo(meeting_row("m_delete", outside));
    CHECK_THROWS_AS(delete_meeting_data(repo, "m_delete", root), fv::py::ValueError);
    CHECK_THROWS_WITH(delete_meeting_data(repo, "m_delete", root),
                      "meeting spool is outside the configured meetings root");
    CHECK(fs::is_directory(outside));
    CHECK(repo.meeting.has_value());
}

// from tests/test_meeting_data_lifecycle.py::test_delete_can_keep_the_spool
TEST_CASE("test_delete_can_keep_the_spool", "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_keep";
    fs::create_directories(spool);
    write_file(spool / "audio.wav", "audio");
    LifecycleRepository repo(meeting_row("m_keep", spool));
    CHECK(delete_meeting_data(repo, "m_keep", root, false));
    CHECK_FALSE(repo.meeting.has_value());
    CHECK(fs::is_directory(spool));
    CHECK(read_file(spool / "audio.wav") == "audio");
}

// from tests/test_meeting_data_lifecycle.py::test_clear_meetings_skips_live_rows_and_ids
TEST_CASE("test_clear_meetings_skips_live_rows_and_ids", "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path live = root / "m_live";
    const fs::path paused = root / "m_paused";
    const fs::path done = root / "m_done";
    const fs::path skipped = root / "m_skip";
    for (const auto& folder : {live, paused, done, skipped}) {
        fs::create_directories(folder);
        write_file(folder / "audio.wav", "audio");
    }
    MultiLifecycleRepository repo(
        {meeting_row("m_live", live, "active"), meeting_row("m_paused", paused, "paused"),
         meeting_row("m_done", done, "ended"), meeting_row("m_skip", skipped, "ended")});
    const ClearResult result = clear_meetings(repo, root, true, {"m_skip"});
    CHECK(result.removed == 1);
    CHECK(repo.ids() == std::vector<std::string>{"m_live", "m_paused", "m_skip"});
    CHECK(fs::is_directory(live));
    CHECK(fs::is_directory(paused));
    CHECK(fs::is_directory(skipped));
    CHECK_FALSE(fs::exists(done));
}

// from tests/test_meeting_data_lifecycle.py::test_clear_meetings_keeps_spools_when_requested
TEST_CASE("test_clear_meetings_keeps_spools_when_requested", "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path spool = root / "m_done";
    fs::create_directories(spool);
    write_file(spool / "audio.wav", "audio");
    MultiLifecycleRepository repo({meeting_row("m_done", spool, "ended")});
    CHECK(clear_meetings(repo, root, false).removed == 1);
    CHECK(repo.rows.empty());
    CHECK(fs::is_directory(spool));
    CHECK(read_file(spool / "audio.wav") == "audio");
}

// from
// tests/test_meeting_data_lifecycle.py::test_clear_meetings_purges_orphan_spools_but_not_tombstones
TEST_CASE("test_clear_meetings_purges_orphan_spools_but_not_tombstones",
          "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path orphan = root / "m_orphan";
    const fs::path tombstone = root / ".deleting-m_old-abc";
    const fs::path leftover = root / "notes.txt";
    fs::create_directories(orphan);
    fs::create_directories(tombstone);
    write_file(orphan / "audio.wav", "audio");
    write_file(leftover, "keep files");
    MultiLifecycleRepository repo({});
    CHECK(clear_meetings(repo, root, true).removed == 0);
    CHECK_FALSE(fs::exists(orphan));
    CHECK(fs::is_directory(tombstone));
    CHECK(fs::is_regular_file(leftover));
}

// from
// tests/test_meeting_data_lifecycle.py::test_clear_meetings_purges_abandoned_deletion_tombstones
TEST_CASE("test_clear_meetings_purges_abandoned_deletion_tombstones",
          "[test_meeting_data_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path tombstone = root / ".deleting-m_old-abc";
    fs::create_directories(tombstone);
    write_file(tombstone / "audio.wav", "audio");
    age_two_hours(tombstone);
    MultiLifecycleRepository repo({});
    CHECK(clear_meetings(repo, root, true).removed == 0);
    CHECK_FALSE(fs::exists(tombstone));
}

TEST_CASE("lifecycle: paired computers' meetings stay and the callback says whose a delete was",
          "[meeting_state_lifecycle]") {
    fv::testkit::TempDataRoot tmp;
    const fs::path root = tmp.path() / "meetings";
    const fs::path mine = root / "m_mine";
    const fs::path theirs = root / "m_theirs";
    fs::create_directories(mine);
    fs::create_directories(theirs);
    Json paired = meeting_row("m_theirs", theirs, "ended");
    paired["origin_device_id"] = "dev-1";
    MultiLifecycleRepository repo({meeting_row("m_mine", mine, "ended"), paired});
    std::vector<std::pair<std::string, bool>> told;
    const auto result = clear_meetings(repo, root, true, {}, [&](std::string id, bool origin) {
        told.emplace_back(std::move(id), origin);
    });
    CHECK(result.removed == 1);
    CHECK(repo.ids() == std::vector<std::string>{"m_theirs"});
    CHECK(fs::is_directory(theirs));
    CHECK_FALSE(fs::exists(mine));
    CHECK(told == std::vector<std::pair<std::string, bool>>{{"m_mine", false}});
    // A paired computer's meeting deleted on its own still says so.
    told.clear();
    CHECK(delete_meeting_data(repo, "m_theirs", root, false, [&](std::string id, bool origin) {
        told.emplace_back(std::move(id), origin);
    }));
    CHECK(told == std::vector<std::pair<std::string, bool>>{{"m_theirs", true}});
    CHECK_FALSE(delete_meeting_data(repo, "m_gone", root));
    // A spool at the root itself is refused.
    MultiLifecycleRepository broad({meeting_row("m_root", root, "ended")});
    CHECK_THROWS_AS(delete_meeting_data(broad, "m_root", root), fv::py::ValueError);
}
