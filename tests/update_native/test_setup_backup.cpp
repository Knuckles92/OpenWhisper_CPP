#include "base/atomic_file.h"
#include "download/verified_files.h"
#include "testkit/testkit.h"
#include "update/update_contract.h"
#include "update_native/archive.h"
#include "update_native/manifest.h"
#include "update_native/setup_backup.h"
#include <catch2/catch_test_macros.hpp>

namespace {
namespace fs = std::filesystem;
namespace u = fv::update_native;

TEST_CASE("installer backup survives partial install, retry and failed verification",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto app = temp.path() / "install";
    const auto next = temp.path() / "release";
    fs::create_directory(app);
    fs::create_directory(next);
    fv::write_file_atomic(app / "fiorivoce.exe", "old app");
    fv::write_file_atomic(app / "retired.dll", "previous component");
    fv::write_file_atomic(app / "unins000.dat", "Inno registration");
    fv::write_file_atomic(app / "my-notes.txt", "unmanaged");
    for (const auto& name : {"fiorivoce.exe", "fiorivoce-updater.exe", "new.dll"})
        fv::write_file_atomic(next / name, name);
    const auto manifest = u::build_manifest(next, "0.1.1");
    const auto old_hash = fv::download::sha256_file(app / "fiorivoce.exe", true);
    REQUIRE_THROWS(u::prepare_setup(app, manifest, false));
    u::prepare_setup(app, manifest, true);
    fs::remove(app / "retired.dll");
    for (const auto& entry : fs::directory_iterator(next))
        fs::copy_file(entry.path(), app / entry.path().filename(),
                      fs::copy_options::overwrite_existing);
    u::write_manifest(next, "0.1.1");
    fs::copy_file(next / fv::update::manifest_name(), app / fv::update::manifest_name());
    SECTION("healthy install commits") {
        u::finish_setup(app, manifest);
        CHECK_FALSE(fs::exists(app / "retired.dll"));
        CHECK_FALSE(fs::exists(temp.path() / "install.setup-backup"));
    }
    SECTION("setup refusal restores every original after an incomplete installation") {
        fv::write_file_atomic(app / "new.dll", "damaged");
        REQUIRE_THROWS(u::finish_setup(app, manifest));
        u::rollback_setup(app, manifest, true);
        CHECK(fs::is_regular_file(app / "retired.dll"));
        CHECK_FALSE(fs::exists(app / "new.dll"));
        CHECK(fv::download::sha256_file(app / "fiorivoce.exe", true) == old_hash);
    }
    SECTION("starting setup again restores an interrupted preparation before taking a fresh copy") {
        u::prepare_setup(app, manifest, true);
        CHECK(fv::download::sha256_file(app / "fiorivoce.exe", true) == old_hash);
        u::rollback_setup(app, manifest, true);
    }
    CHECK(fs::is_regular_file(app / "my-notes.txt"));
    CHECK(fs::is_regular_file(app / "unins000.dat"));
}
TEST_CASE("first setup rollback only removes identical installer payloads", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const auto app = temp.path() / "install";
    const auto next = temp.path() / "release";
    fs::create_directory(next);
    for (const auto& name : {"fiorivoce.exe", "fiorivoce-updater.exe", "new.dll"})
        fv::write_file_atomic(next / name, name);
    const auto manifest = u::build_manifest(next, "0.1.1");
    u::prepare_setup(app, manifest, false);
    fv::write_file_atomic(app / "new.dll", "belongs to the user");
    CHECK_THROWS(u::rollback_setup(app, manifest, false));
    REQUIRE(fs::exists(app / "new.dll"));
    fv::write_file_atomic(app / "new.dll", "new.dll");
    u::rollback_setup(app, manifest, false);
    CHECK_FALSE(fs::exists(app / "new.dll"));
    CHECK_FALSE(fs::exists(temp.path() / "install.setup-backup"));
}

} // namespace
