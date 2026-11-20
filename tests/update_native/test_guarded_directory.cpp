#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>

#include "testkit/testkit.h"
#include "update_native/guarded_directory.h"

namespace fs = std::filesystem;

TEST_CASE("guarded directory renames children without replacing a destination", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto parent = temp.path() / "programs";
    fs::create_directories(parent / "old");
    fs::create_directories(parent / "occupied");
    std::ofstream(parent / "old" / "marker") << "old";
    fv::update_native::GuardedDirectory guard(parent);
    CHECK_THROWS(guard.rename_child_exclusive("old", "occupied"));
    CHECK(fs::exists(parent / "old" / "marker"));
    guard.rename_child_exclusive("old", "rollback");
    CHECK(fs::exists(parent / "rollback" / "marker"));
    CHECK_FALSE(fs::exists(parent / "old"));
    CHECK_THROWS(guard.rename_child_exclusive("../rollback", "other"));
    CHECK_THROWS(guard.rename_child_exclusive("rollback", "../other"));
}

#ifdef _WIN32
// Sharing modes are Windows': elsewhere an open directory can still be renamed.
TEST_CASE("guarded directory holds ancestors against replacement", "[update_native][platform]") {
    fv::testkit::TempDataRoot temp;
    const auto parent = temp.path() / "outer" / "inner";
    fs::create_directories(parent);
    {
        fv::update_native::GuardedDirectory guard(parent);
        std::error_code error;
        fs::rename(parent.parent_path(), temp.path() / "moved", error);
        CHECK(error);
    }
    std::error_code error;
    fs::rename(parent.parent_path(), temp.path() / "moved", error);
    CHECK_FALSE(error);
}
#endif

TEST_CASE("guarded directory publishes and reads bounded files", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    fv::update_native::GuardedDirectory guard(temp.path());
    guard.write_file_atomic("journal.json", "first");
    CHECK(guard.read_file("journal.json", 5) == "first");
    CHECK_THROWS(guard.read_file("journal.json", 4));
    guard.write_file_atomic("journal.json", "second");
    CHECK(guard.read_file("journal.json", 6) == "second");
    CHECK_THROWS(guard.write_file_atomic("../outside", "no"));
#ifdef _WIN32
    {
        // A pin denies replacement and deletion through the handle's sharing mode.
        auto pin = guard.pin_file("journal.json");
        REQUIRE(pin);
        CHECK_THROWS(guard.write_file_atomic("journal.json", "replacement"));
        std::error_code error;
        fs::remove(temp.path() / "journal.json", error);
        CHECK(error);
    }
#endif
    guard.write_file_atomic("journal.json", "replacement");
    auto source = guard.pin_file("journal.json");
    REQUIRE(source);
    guard.copy_file_atomic_from(*source, "copy.json");
    CHECK(guard.read_file("copy.json", 11) == "replacement");
    guard.remove_file("copy.json");
    CHECK_FALSE(fs::exists(temp.path() / "copy.json"));
}

TEST_CASE("guarded directory refuses links, linked files and occupied directories",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto root = temp.path() / "root";
    fs::create_directories(root / "real" / "child");
    std::ofstream(root / "real" / "file.txt") << "content";
    std::error_code error;
    fs::create_directory_symlink(root / "real", root / "link", error);
    if (!error) {
        // A linked ancestor or leaf directory is never opened.
        CHECK_THROWS(fv::update_native::GuardedDirectory(root / "link"));
        CHECK_THROWS(fv::update_native::GuardedDirectory(root / "link" / "child"));
        fv::update_native::GuardedDirectory guard(root);
        CHECK_THROWS(guard.rename_child_exclusive("link", "moved"));
        CHECK_THROWS(guard.remove_empty_directory("link"));
    }
    fv::update_native::GuardedDirectory guard(root / "real");
    fs::create_symlink(root / "real" / "file.txt", root / "real" / "alias.txt", error);
    if (!error) {
        CHECK_THROWS(guard.read_file("alias.txt", 64));
        CHECK_THROWS(guard.pin_file("alias.txt"));
        CHECK_THROWS(guard.remove_file("alias.txt"));
        fs::remove(root / "real" / "alias.txt");
    }
    fs::create_hard_link(root / "real" / "file.txt", root / "real" / "second.txt", error);
    if (!error) {
        CHECK_THROWS(guard.read_file("file.txt", 64));
        CHECK_THROWS(guard.pin_file("second.txt"));
        fs::remove(root / "real" / "second.txt");
    }
    CHECK(guard.read_file("file.txt", 64) == "content");
    CHECK_FALSE(guard.pin_file("missing.txt"));
    CHECK_NOTHROW(guard.remove_file("missing.txt"));
    // A file is removed only while it still has the content its owner recorded.
    CHECK_THROWS(guard.remove_file("file.txt", std::string(64, '0')));
    CHECK(fs::exists(root / "real" / "file.txt"));
    CHECK_THROWS(guard.remove_empty_directory("file.txt"));
    fv::update_native::GuardedDirectory parent(root);
    CHECK_THROWS(parent.remove_empty_directory("real"));
    guard.remove_empty_directory("child");
    CHECK_FALSE(fs::exists(root / "real" / "child"));
    // Relative and unnormalized paths never name a guarded directory.
    CHECK_THROWS(fv::update_native::GuardedDirectory("relative"));
    CHECK_THROWS(fv::update_native::GuardedDirectory(root / "real" / ".." / "real"));
}
