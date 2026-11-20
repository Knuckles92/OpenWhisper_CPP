#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "archive/zip_writer.h"
#include "base/atomic_file.h"
#include "testkit/testkit.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/manifest.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;

namespace {
struct Release {
    fv::testkit::TempDataRoot temp;
    fs::path tree = temp.path() / "stage";
    fs::path zip = temp.path() / "release.zip";
    Release() {
        fs::create_directories(tree / "platforms");
        fv::write_file_atomic(tree / "openwhisper-cpp.exe", "app");
        fv::write_file_atomic(tree / "openwhisper-cpp-updater.exe", "helper");
        fv::write_file_atomic(tree / "platforms" / "qwindows.dll", "plugin");
    }
};
} // namespace

TEST_CASE("a OpenWhisper_CPP release round-trips through its native archive manifest",
          "[update_native]") {
    Release r;
    u::write_manifest(r.tree, "0.1.1");
    u::pack_archive(r.tree, "0.1.1", r.zip);
    const auto doc = u::verify_archive(r.zip, "0.1.1");
    CHECK(doc.at("member_count") == 4);
    CHECK(doc.at("unpacked_bytes") == 15);
    CHECK(doc.at("files").contains("platforms/qwindows.dll"));
    CHECK_NOTHROW(u::verify_tree(r.tree, doc));
    const auto candidate = r.temp.path() / "candidate";
    REQUIRE(u::extract_archive(r.zip, candidate, "0.1.1") == doc);
    CHECK_NOTHROW(u::verify_tree(candidate, doc));
    CHECK_THROWS_AS(u::extract_archive(r.zip, candidate, "0.1.1"), u::UpdateError);
    fv::CancelSource canceled;
    canceled.cancel();
    CHECK_THROWS(u::extract_archive(r.zip, r.temp.path() / "canceled", "0.1.1", canceled.token()));
    CHECK_FALSE(fs::exists(r.temp.path() / "canceled"));
    CHECK_THROWS(u::verify_archive(r.zip, "0.1.2"));
    CHECK_THROWS_AS(u::pack_archive(r.tree, "0.1.2", r.zip), u::UpdateError);
    CHECK_NOTHROW(u::verify_archive(r.zip, "0.1.1"));
}

TEST_CASE("manifest validation rejects incompatible releases or modified trees",
          "[update_native]") {
    Release r;
    auto doc = u::build_manifest(r.tree, "0.1.1");
    SECTION("newer updater required") {
        doc["minimum_updater_version"] = "0.2.0";
        CHECK_THROWS_AS(u::validate_manifest(doc, {}, "0.1.1"), u::RequiresSetup);
    }
    SECTION("missing executable") {
        fs::remove(r.tree / "openwhisper-cpp.exe");
        CHECK_THROWS_AS(u::build_manifest(r.tree, "0.1.1"), u::UpdateError);
    }
    SECTION("changed bytes of the same length") {
        fv::write_file_atomic(r.tree / "openwhisper-cpp.exe", "bad");
        CHECK_THROWS_AS(u::verify_tree(r.tree, doc), u::UpdateError);
    }
    SECTION("unmanaged root") {
        fv::write_file_atomic(r.tree / "user-notes.txt", "keep");
        CHECK_THROWS_AS(u::build_manifest(r.tree, "0.1.1"), u::RequiresSetup);
    }
    SECTION("case collision") {
        doc["files"]["OpenWhisper_CPP.dll"] = doc["files"]["openwhisper-cpp.exe"];
        doc["files"]["openwhisper-cpp.dll"] = doc["files"]["openwhisper-cpp.exe"];
        doc["member_count"] = 6;
        doc["unpacked_bytes"] = 21;
        CHECK_THROWS_AS(u::validate_manifest(doc), u::UpdateError);
    }
}

TEST_CASE("only literal relative Windows names can appear in a release", "[update_native]") {
    for (auto name : {"", "/root", "../up", "foo//bar", "foo/.", "foo/..", "a:b", "dir\\name",
                      "NUL.log", "dir/lpt1", "name.", "name ", "foo/<file", "a/**", "q|pipe"}) {
        CAPTURE(name);
        CHECK_THROWS_AS(u::member_name(name), u::UpdateError);
    }
    CHECK(u::member_name("platforms/qwindows.dll") == "platforms/qwindows.dll");
    CHECK(u::member_name("platforms/", true) == "platforms");
}

TEST_CASE("the archive is independently checked against every declared hash", "[update_native]") {
    Release r;
    const auto doc = u::build_manifest(r.tree, "0.1.1").dump();
    auto zip = fv::archive::ZipWriter::create(r.zip);
    const auto add = [&](std::string_view name, std::string_view data, unsigned attrs = 0) {
        zip.add_bytes(name, std::as_bytes(std::span(data)), fv::archive::Method::Deflated, attrs);
    };
    add(fv::update::manifest_name(), doc);
    SECTION("an altered payload has its original size") {
        add("openwhisper-cpp.exe", "bad");
        add("openwhisper-cpp-updater.exe", "helper");
        add("platforms/qwindows.dll", "plugin");
    }
    SECTION("the archive omits a declared file") {
        add("openwhisper-cpp.exe", "app");
        add("openwhisper-cpp-updater.exe", "helper");
    }
    SECTION("a declared file is a symbolic link") {
        add("openwhisper-cpp.exe", "app", 0120777U << 16);
    }
    SECTION("a second entry aliases the first") {
        add("helper.dll", "a");
        add("HELPER.dll", "b");
    }
    // from tests/test_app_update_apply.py::TestSafeExtract::test_rejects_parent_member
    SECTION("a member climbs out of the candidate") {
        add("openwhisper-cpp.exe", "app");
        add("openwhisper-cpp-updater.exe", "helper");
        add("platforms/qwindows.dll", "plugin");
        add("../escape.exe", "x");
    }
    zip.close();
    CHECK_THROWS_AS(u::verify_archive(r.zip), u::UpdateError);
    const auto candidate = r.temp.path() / "candidate";
    CHECK_THROWS_AS(u::extract_archive(r.zip, candidate, "0.1.1"), u::UpdateError);
    CHECK_FALSE(fs::exists(candidate));
    CHECK_FALSE(fs::exists(r.temp.path() / "escape.exe"));
    for (const auto& entry : fs::directory_iterator(r.temp.path())) {
        CHECK_FALSE(entry.path().filename().wstring().starts_with(L".openwhisper-cpp-candidate-"));
    }
}
