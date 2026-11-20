#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

#include "pyport/errors.h"
#include "pyport/json.h"
#include "support.h"
#include "walkthrough/files.h"

using namespace fv::walkthrough;
using namespace fv::walkthrough::test;

namespace {

std::vector<std::string> entries(const std::filesystem::path& folder) {
    std::vector<std::string> names;
    for (const auto& entry : std::filesystem::directory_iterator(folder)) {
        names.push_back(fv::path_to_utf8(entry.path().filename()));
    }
    std::ranges::sort(names);
    return names;
}

std::string value_error(const std::function<void()>& call) {
    try {
        call();
    } catch (const fv::py::ValueError& error) {
        return error.what();
    }
    return "";
}

} // namespace

TEST_CASE("atomic JSON is written as Python's json.dump writes it", "[files]") {
    const TempFolder tmp;
    const auto path = tmp / "draft.json";
    fv::Json value = fv::Json::object();
    value["version"] = 1;
    value["text"] = "Café";
    value["kept_at_s"] = 2.0;
    value["notes_s"] = fv::Json::array({fv::Json::array({2.5, nullptr})});
    value["empty"] = fv::Json::array();

    atomic_json(path, value);

    CHECK(read_file(path) == "{\n  \"version\": 1,\n  \"text\": \"Café\",\n"
                             "  \"kept_at_s\": 2.0,\n  \"notes_s\": [\n    [\n      2.5,\n"
                             "      null\n    ]\n  ],\n  \"empty\": []\n}");
    CHECK(entries(tmp.path()) == std::vector<std::string>{"draft.json"});
}

TEST_CASE("atomic JSON refuses NaN and infinity and leaves the old file", "[files]") {
    const TempFolder tmp;
    const auto path = tmp / "draft.json";
    write_file(path, "old");

    for (const double bad :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(),
          -std::numeric_limits<double>::infinity()}) {
        fv::Json value = fv::Json::object();
        value["notes_s"] = fv::Json::array({fv::Json::array({1.0, bad})});
        const auto message = value_error([&] { atomic_json(path, value); });
        CHECK(message.starts_with("Out of range float values are not JSON compliant: "));
    }
    CHECK(value_error([&] { atomic_json(path, fv::Json(std::nan(""))); }) ==
          "Out of range float values are not JSON compliant: nan");
    CHECK(read_file(path) == "old");
    CHECK(entries(tmp.path()) == std::vector<std::string>{"draft.json"});
}

TEST_CASE("atomic JSON leaves no temporary file on failure", "[files]") {
    const TempFolder tmp;
    // The target is a folder: the replace fails after the temporary file was written.
    std::filesystem::create_directory(tmp / "draft.json");
    std::filesystem::create_directory(tmp / "draft.json" / "inside");

    CHECK_THROWS_AS(atomic_json(tmp / "draft.json", fv::Json::object()),
                    std::filesystem::filesystem_error);
    CHECK(entries(tmp.path()) == std::vector<std::string>{"draft.json"});
    CHECK_THROWS_AS(atomic_json(tmp / "missing" / "draft.json", fv::Json::object()),
                    std::filesystem::filesystem_error);
}

TEST_CASE("publishing replaces every file and keeps the old ones aside", "[files]") {
    const TempFolder tmp;
    const auto stage = tmp / "stage";
    const auto folder = tmp / "capture";
    std::filesystem::create_directories(stage);
    std::filesystem::create_directories(folder);
    write_file(folder / "walkthrough.md", "old md");
    write_file(folder / "walkthrough.original.json", "stale");
    write_file(stage / "walkthrough.md", "new md");
    write_file(stage / "walkthrough.pdf", "new pdf");
    const std::vector<std::string> names{"walkthrough.md", "walkthrough.pdf"};
    const std::vector<std::string> remove{"walkthrough.original.json", "never-there.json"};

    publish_files(stage, folder, names, remove);

    CHECK(read_file(folder / "walkthrough.md") == "new md");
    CHECK(read_file(folder / "walkthrough.pdf") == "new pdf");
    CHECK_FALSE(std::filesystem::exists(folder / "walkthrough.original.json"));
    CHECK(read_file(stage / ".previous" / "walkthrough.md") == "old md");
    CHECK(read_file(stage / ".previous" / "walkthrough.original.json") == "stale");
    CHECK_FALSE(std::filesystem::exists(stage / ".previous" / "walkthrough.pdf"));
}

TEST_CASE("publish files rolls back each target from previous on a failed replace", "[files]") {
    const TempFolder tmp;
    const auto stage = tmp / "stage";
    const auto folder = tmp / "capture";
    std::filesystem::create_directories(stage);
    std::filesystem::create_directories(folder);
    write_file(folder / "walkthrough.md", "old md");
    write_file(folder / "walkthrough.pdf", "old pdf");
    write_file(folder / "walkthrough.original.json", "original");
    write_file(stage / "walkthrough.md", "new md");
    write_file(stage / "walkthrough.json", "new manifest");
    // walkthrough.pdf was never staged: its replacement fails after two others succeeded.
    const std::vector<std::string> names{"walkthrough.md", "walkthrough.json", "walkthrough.pdf"};
    const std::vector<std::string> remove{"walkthrough.original.json"};

    CHECK_THROWS_AS(publish_files(stage, folder, names, remove), std::filesystem::filesystem_error);

    CHECK(read_file(folder / "walkthrough.md") == "old md");
    CHECK(read_file(folder / "walkthrough.pdf") == "old pdf");
    CHECK(read_file(folder / "walkthrough.original.json") == "original");
    // A file that was not there before is not left behind.
    CHECK_FALSE(std::filesystem::exists(folder / "walkthrough.json"));
}

TEST_CASE("publishing needs a fresh previous folder", "[files]") {
    const TempFolder tmp;
    std::filesystem::create_directories(tmp / "stage" / ".previous");
    std::filesystem::create_directories(tmp / "capture");

    CHECK_THROWS_AS(publish_files(tmp / "stage", tmp / "capture", std::vector<std::string>{}),
                    std::filesystem::filesystem_error);
}

TEST_CASE("local file refuses separators and anything but a regular file", "[files]") {
    const TempFolder tmp;
    write_file(tmp / "shot-1.png", "png");
    std::filesystem::create_directory(tmp / "sub");

    CHECK(local_file(tmp.path(), "shot-1.png") == tmp / "shot-1.png");
    for (const auto* name : {"sub/shot-1.png", "sub\\shot-1.png", "../shot-1.png", "."}) {
        CHECK(value_error([&] { local_file(tmp.path(), name); }) == "Invalid walkthrough filename");
    }
#ifdef _WIN32
    CHECK(value_error([&] { local_file(tmp.path(), "c:shot-1.png"); }) ==
          "Invalid walkthrough filename");
#endif
    CHECK(value_error([&] { local_file(tmp.path(), "missing.png"); }) ==
          "Missing or unsafe walkthrough file: missing.png");
    CHECK(value_error([&] { local_file(tmp.path(), "sub"); }) ==
          "Missing or unsafe walkthrough file: sub");
    CHECK(value_error([&] { local_file(tmp.path(), ".."); }) ==
          "Missing or unsafe walkthrough file: ..");
}

TEST_CASE("local file refuses symlinked folders and files", "[files][platform]") {
    const TempFolder tmp;
    std::filesystem::create_directory(tmp / "real");
    write_file(tmp / "real" / "shot-1.png", "png");
    std::error_code error;
    std::filesystem::create_directory_symlink(tmp / "real", tmp / "linked", error);
    std::filesystem::create_symlink(tmp / "real" / "shot-1.png", tmp / "real" / "shot-2.png",
                                    error);
    if (error) {
        SKIP("This account cannot create symbolic links: " << error.message());
    }

    CHECK(value_error([&] { local_file(tmp / "linked", "shot-1.png"); }) ==
          "Missing or unsafe walkthrough file: shot-1.png");
    CHECK(value_error([&] { local_file(tmp / "real", "shot-2.png"); }) ==
          "Missing or unsafe walkthrough file: shot-2.png");
    CHECK(value_error([&] { read_json(tmp / "real" / "shot-2.png"); }) ==
          "Unsafe or oversized walkthrough metadata");
}

TEST_CASE("read json refuses 8 MiB and one byte, and anything but an object", "[files]") {
    const TempFolder tmp;
    const auto path = tmp / "walkthrough.json";

    std::string big = "{\"a\": \"";
    big += std::string(MAX_METADATA_BYTES - big.size() - 2, 'x');
    big += "\"}";
    REQUIRE(big.size() == MAX_METADATA_BYTES);
    write_file(path, big);
    CHECK(read_json(path).at("a").get<std::string>().size() == MAX_METADATA_BYTES - 9);

    write_file(path, big + " ");
    CHECK(value_error([&] { read_json(path); }) == "Unsafe or oversized walkthrough metadata");

    write_file(path, "[1, 2]");
    CHECK(value_error([&] { read_json(path); }) == "Walkthrough metadata must be an object");
    write_file(path, "{\"version\": 1}");
    CHECK(read_json(path) == fv::py::loads("{\"version\": 1}"));
    write_file(path, "{\"version\": \r\n1}");
    CHECK(read_json(path).at("version") == 1);
    write_file(path, "{\"text\": \"\xff\"}");
    CHECK_FALSE(value_error([&] { read_json(path); }).empty());
    write_file(path, "{not json");
    CHECK_THROWS_AS(read_json(path), fv::py::JsonError);
    CHECK_THROWS_AS(read_json(tmp / "missing.json"), std::filesystem::filesystem_error);
}
