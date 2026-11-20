// ZipWriter::create never makes a folder (sweep I2, open item B25): zipfile's open() raises
// FileNotFoundError for a missing folder, where minizip-ng made it, from a copy of the path cut
// to 319 bytes.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include "archive/zip_writer.h"
#include "testkit/testkit.h"

using fv::archive::ZipWriter;
namespace fs = std::filesystem;

namespace {

std::vector<fs::path> entries_below(const fs::path& root) {
    std::vector<fs::path> entries;
    for (const auto& entry : fs::recursive_directory_iterator(root)) {
        entries.push_back(entry.path());
    }
    return entries;
}

// A folder path below `base` well past minizip-ng's 320-byte buffer, of short components.
fs::path long_folder(const fs::path& base) {
    fs::path folder = base;
    while (folder.native().size() < 420) {
        folder /= std::string(40, 'd');
    }
    return folder;
}

// Windows refuses a path longer than MAX_PATH unless the program declares itself long-path aware
// or the path takes the \\?\ form; the test programs declare nothing, so there it takes the form.
fs::path long_path_form(const fs::path& path) {
#if defined(_WIN32)
    return fs::path(L"\\\\?\\" + fs::absolute(path).native());
#else
    return path;
#endif
}

} // namespace

TEST_CASE("a zip in a missing folder is refused and makes no folder", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path base = root.path() / "base";
    fs::create_directories(base);
    const fs::path folder = GENERATE_COPY(base / "missing", long_folder(base / "missing"));

    try {
        (void)ZipWriter::create(folder / "archive.zip");
        FAIL("the zip was created");
    } catch (const fs::filesystem_error& error) {
        CHECK(error.code() == std::make_error_code(std::errc::no_such_file_or_directory));
    }
    CHECK(entries_below(base).empty());
}

TEST_CASE("a zip under a long path whose folder exists is written there alone", "[zip_writer]") {
    fv::testkit::TempDataRoot root;
    const fs::path base = long_path_form(root.path() / "base");
    const fs::path folder = long_folder(base);
    fs::create_directories(folder);
    const auto before = entries_below(base);

    ZipWriter writer = ZipWriter::create(folder / "archive.zip");
    writer.close();

    CHECK(fs::is_regular_file(folder / "archive.zip"));
    CHECK(entries_below(base).size() == before.size() + 1);
}
