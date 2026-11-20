// from tests/test_walkthrough_storage.py
#include <catch2/catch_test_macros.hpp>

#include "base/identity.h"
#include "base/paths.h"
#include "base/testing/map_env_reader.h"
#include "golden/golden.h"
#include "support.h"
#include "walkthrough/storage.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;
using namespace std::chrono_literals;

namespace {

std::chrono::local_seconds started() {
    return std::chrono::local_days{std::chrono::year{2026} / 10 / 21} + 14h + 32min + 5s;
}

// The data root is the test's folder, as OpenWhisper's conftest makes tmp_path the data root;
// every variable the per-user folder is read from stays inside it too.
fv::Paths paths_in(const TempFolder& tmp) {
    const auto inside = fv::path_to_utf8(tmp / "user");
    const fv::MapEnvReader env{
        {std::string(fv::identity().env_prefix) + "DATA_DIR", fv::path_to_utf8(tmp.path())},
        {"LOCALAPPDATA", inside},
        {"XDG_DATA_HOME", inside},
        {"HOME", inside},
        {"USERPROFILE", inside}};
    return fv::Paths::resolve(fv::identity(), env);
}

std::vector<std::string> entries(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(fv::path_to_utf8(entry.path().filename()));
    }
    return names;
}

} // namespace

TEST_CASE("captures live in the data root", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto paths = paths_in(tmp);

    CHECK(std::filesystem::equivalent(paths.data_root(), tmp.path()));
    CHECK(captures_root(paths) == paths.data_root() / "captures");
}

TEST_CASE("a walkthrough folder is named for its start and created", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));

    const auto folder = new_capture_folder(root, started());

    CHECK(folder == root / "2026-10-21_14-32-05");
    CHECK(std::filesystem::is_directory(folder));
    CHECK(std::filesystem::is_empty(folder));
}

TEST_CASE("a taken name gets the next number", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));

    std::vector<std::string> names;
    for (int index = 0; index < 3; ++index) {
        const auto folder = new_capture_folder(root, started());
        CHECK(std::filesystem::is_directory(folder));
        names.push_back(fv::path_to_utf8(folder.filename()));
    }

    CHECK(names == std::vector<std::string>{"2026-10-21_14-32-05", "2026-10-21_14-32-05-2",
                                            "2026-10-21_14-32-05-3"});
}

TEST_CASE("a file with the name is never written into", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));
    std::filesystem::create_directories(root);
    write_file(root / "2026-10-21_14-32-05", "someone else's");

    CHECK(new_capture_folder(root, started()).filename() == "2026-10-21_14-32-05-2");
    CHECK(read_file(root / "2026-10-21_14-32-05") == "someone else's");
}

TEST_CASE("a screenshot path is named for when it was taken", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));

    const auto path = new_screenshot_path(root, started());

    CHECK(path == root / "screenshots" / "2026-10-21_14-32-05.png");
    CHECK(std::filesystem::is_directory(path.parent_path()));
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("a saved screenshot moves the next one on", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));

    std::vector<std::string> names;
    for (int index = 0; index < 3; ++index) {
        const auto path = new_screenshot_path(root, started());
        write_file(path, "png");
        names.push_back(fv::path_to_utf8(path.filename()));
    }

    CHECK(names == std::vector<std::string>{"2026-10-21_14-32-05.png", "2026-10-21_14-32-05-2.png",
                                            "2026-10-21_14-32-05-3.png"});
}

TEST_CASE("an unsaved screenshot path is offered again", "[test_walkthrough_storage]") {
    const TempFolder tmp;
    const auto root = captures_root(paths_in(tmp));

    // Only the folder is created; the name is the caller's once it saves.
    CHECK(new_screenshot_path(root, started()) == new_screenshot_path(root, started()));
}

TEST_CASE("a captures root that cannot be created is an error", "[storage]") {
    const TempFolder tmp;
    write_file(tmp / "captures", "a file");

    CHECK_THROWS_AS(new_capture_folder(tmp / "captures", started()),
                    std::filesystem::filesystem_error);
    CHECK_THROWS_AS(new_screenshot_path(tmp / "captures", started()),
                    std::filesystem::filesystem_error);
}

// --- golden vectors ---------------------------------------------------------------------------

namespace {

void make_existing(const std::filesystem::path& root, const fv::Json& existing) {
    for (const auto& item : existing) {
        const auto path = root / fv::path_from_utf8(item.at(0).get<std::string>());
        std::filesystem::create_directories(path.parent_path());
        if (item.at(1).get<std::string>() == "dir") {
            std::filesystem::create_directory(path);
        } else {
            write_file(path, "someone else's");
        }
    }
}

} // namespace

TEST_CASE("golden: new_capture_folder", "[golden][storage]") {
    fv::golden::for_each(
        "services.walkthrough.storage", "new_capture_folder", [](const fv::golden::Case& c) {
            const TempFolder tmp;
            const auto root = tmp / "captures";
            make_existing(root, c.args()[1]);
            const auto when = local_from_iso(c.arg<std::string>(0));
            std::vector<std::string> names;
            for (int index = 0; index < c.arg<int>(2); ++index) {
                names.push_back(fv::path_to_utf8(new_capture_folder(root, when).filename()));
            }
            CHECK(names == c.result<std::vector<std::string>>());
        });
}

TEST_CASE("golden: new_screenshot_path", "[golden][storage]") {
    fv::golden::for_each("services.walkthrough.storage", "new_screenshot_path",
                         [](const fv::golden::Case& c) {
                             const TempFolder tmp;
                             const auto root = tmp / "captures";
                             make_existing(root, c.args()[1]);
                             const auto when = local_from_iso(c.arg<std::string>(0));
                             std::vector<std::string> names;
                             for (int index = 0; index < c.arg<int>(2); ++index) {
                                 const auto path = new_screenshot_path(root, when);
                                 if (c.arg<bool>(3)) {
                                     write_file(path, "png");
                                 }
                                 names.push_back(fv::path_to_utf8(path.filename()));
                             }
                             CHECK(names == c.result<std::vector<std::string>>());
                         });
}
