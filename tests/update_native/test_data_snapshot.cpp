#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string>
#include <variant>

#include "base/atomic_file.h"
#include "base/identity.h"
#include "sql/db.h"
#include "testkit/testkit.h"
#include "update_native/data_snapshot.h"
#include "update_native/manifest.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
namespace {
constexpr auto tx = "aa000000000000000000000000000001";
std::string content(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("native rollback restores settings and committed SQLite WAL pages", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto root = temp.path();
    const auto db = root / fv::identity().database_file;
    const auto settings = root / fv::identity().settings_file;
    fv::write_file_atomic(settings, "original");
    {
        auto live = fv::sql::Connection::open(db, fv::sql::OpenMode::Create);
        live.exec("PRAGMA journal_mode=WAL");
        live.exec("PRAGMA wal_autocheckpoint=0");
        live.exec("CREATE TABLE notes(value TEXT)");
        live.exec("INSERT INTO notes VALUES ('original')");
        u::snapshot_data(root, tx);
        live.exec("DROP TABLE notes");
    }
    fv::write_file_atomic(settings, "changed");
    u::restore_data(root, tx);
    u::restore_data(root, tx);
    auto restored = fv::sql::Connection::open(db, fv::sql::OpenMode::ReadOnly);
    auto note = restored.prepare("SELECT value FROM notes");
    REQUIRE(note.step());
    CHECK(std::get<std::string>(note.column(0)) == "original");
    CHECK(content(settings) == "original");
}

TEST_CASE("rollback preserves absence and verifies every snapshot before changing data",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto root = temp.path();
    const auto settings = root / fv::identity().settings_file;
    SECTION("an absent preference file stays absent after rollback") {
        u::snapshot_data(root, tx);
        fv::write_file_atomic(settings, "created during health launch");
        u::restore_data(root, tx);
        CHECK_FALSE(fs::exists(settings));
    }
    SECTION("tampered snapshot leaves the current data intact") {
        fv::write_file_atomic(settings, "original");
        u::snapshot_data(root, tx);
        fv::write_file_atomic(settings, "current");
        const auto saved = root / "updates" / "tx" / tx / "data" / fv::identity().settings_file;
        fv::write_file_atomic(saved, "tampered");
        CHECK_THROWS_AS(u::restore_data(root, tx), u::UpdateError);
        CHECK(content(settings) == "current");
    }
    SECTION("a health launch cannot replace the existing snapshot") {
        fv::write_file_atomic(settings, "original");
        u::snapshot_data(root, tx);
        fv::write_file_atomic(settings, "changed");
        CHECK_THROWS(u::snapshot_data(root, tx));
        u::restore_data(root, tx);
        CHECK(content(settings) == "original");
    }
    SECTION("a transaction identifier cannot select another directory") {
        CHECK_THROWS(u::snapshot_data(root, "../other"));
        CHECK_FALSE(fs::exists(root / "updates"));
    }
}
