// write_file_atomic: the temporary-and-replace write of OpenWhisper's settings save.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <fstream>
#include <iterator>

#include "base/atomic_file.h"
#include "base/utf8.h"
#include "test_support.h"

using fv::test::TempDir;

namespace {

std::string read_all(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

std::vector<std::string> names_in(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(fv::path_to_utf8(entry.path().filename()));
    }
    std::sort(names.begin(), names.end());
    return names;
}

} // namespace

TEST_CASE("an atomic write creates the file and leaves no temporary") {
    TempDir temp;
    const auto target = temp.path() / "fiorivoce_settings.json";
    fv::write_file_atomic(target, "{\n  \"a\": 1\n}\n");
    CHECK(read_all(target) == "{\n  \"a\": 1\n}\n");
    CHECK(names_in(temp.path()) == std::vector<std::string>{"fiorivoce_settings.json"});
}

TEST_CASE("an atomic write replaces the whole file, binary bytes included") {
    TempDir temp;
    const auto target = temp.path() / "data.bin";
    fv::write_file_atomic(target, "a much longer first version of the file");
    const std::string bytes("\0\r\n\xff", 4);
    fv::write_file_atomic(target, bytes, {.fsync = false});
    CHECK(read_all(target) == bytes);
    fv::write_file_atomic(target, "");
    CHECK(std::filesystem::file_size(target) == 0);
    CHECK(names_in(temp.path()) == std::vector<std::string>{"data.bin"});
}

TEST_CASE("a failed replace leaves the original intact and no stray temporary") {
    TempDir temp;
    const auto target = temp.path() / "fiorivoce_settings.json";
    fv::write_file_atomic(target, "original");
#if defined(_WIN32)
    // An open handle without delete sharing (as std::ifstream opens it) makes MoveFileExW fail.
    std::ifstream holder(target, std::ios::binary);
    REQUIRE(holder);
    CHECK_THROWS_AS(fv::write_file_atomic(target, "replacement"),
                    std::filesystem::filesystem_error);
    holder.close();
    CHECK(read_all(target) == "original");
    CHECK(names_in(temp.path()) == std::vector<std::string>{"fiorivoce_settings.json"});
#else
    // rename cannot put a file over a folder that holds something.
    const auto folder = temp.path() / "settings";
    std::filesystem::create_directories(folder / "inside");
    CHECK_THROWS_AS(fv::write_file_atomic(folder, "replacement"),
                    std::filesystem::filesystem_error);
    CHECK(std::filesystem::is_directory(folder / "inside"));
    CHECK(read_all(target) == "original");
    CHECK(names_in(temp.path()) == std::vector<std::string>{"fiorivoce_settings.json", "settings"});
#endif
}

TEST_CASE("an atomic write into a missing folder fails without creating it") {
    TempDir temp;
    CHECK_THROWS_AS(fv::write_file_atomic(temp.path() / "missing" / "file.json", "{}"),
                    std::filesystem::filesystem_error);
    CHECK_FALSE(std::filesystem::exists(temp.path() / "missing"));
}

TEST_CASE("a relative target is written beside the working directory's file") {
    TempDir temp;
    fv::test::ScopedCwd cwd(temp.path());
    fv::write_file_atomic("relative.json", "[]");
    CHECK(read_all(temp.path() / "relative.json") == "[]");
}
