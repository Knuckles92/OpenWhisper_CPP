#pragma once

// An installed 0.1.0 with its Inno uninstaller, a packed 0.1.1 release in the update cache and
// the trusted context of a per-user registration: what a native preparation starts from.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "base/atomic_file.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "testkit/testkit.h"
#include "update/apply.h"
#include "update/install_probe.h"
#include "update/release_identity.h"
#include "update_native/archive.h"
#include "update_native/journal.h"
#include "update_native/prepare.h"

namespace fv::update_native::test {
namespace fs = std::filesystem;
namespace u = fv::update_native;

struct PrepareFixture {
    fv::testkit::TempDataRoot folder;
    fs::path install = folder.path() / "installed app";
    fs::path data = folder.path() / "data";
    fs::path updates = data / "updates";
    fs::path archive = updates / "OpenWhisper_CPP-0.1.1-win64.zip";
    u::JournalContext context;
    std::string digest;
    // `new_app`: the bytes of the release's application, for a test that runs it.
    explicit PrepareFixture(const std::string& new_app = "new app") {
        const auto next = folder.path() / "new release";
        for (const auto& dir : {install, next, updates})
            fs::create_directories(dir);
        for (const auto& tree : {install, next}) {
            const auto prefix = tree == install ? std::string("old") : std::string("new");
            fv::write_file_atomic(tree / "openwhisper-cpp.exe",
                                  tree == install ? std::string("old app") : new_app);
            fv::write_file_atomic(tree / "openwhisper-cpp-updater.exe", prefix + " helper");
            fv::write_file_atomic(tree / "libcrypto-3-x64.dll", prefix + " library");
        }
        u::write_manifest(install, "0.1.0");
        fv::write_file_atomic(install / "unins000.exe", "Inno image");
        fv::write_file_atomic(install / "unins000.dat", "Inno state");
        u::pack_archive(next, "0.1.1", archive);
        digest = fv::download::sha256_file(archive, true);
        const auto& release = fv::update::release_identity();
        fv::update::InstallRegistration registration;
        registration.key_path = release.uninstall_key_path();
        registration.install_location = install;
        registration.uninstall_string = "\"" + fv::path_to_utf8(install / "unins000.exe") + "\"";
        registration.display_version = "0.1.0";
        context = {data, install,      release.app_id,
                   {},   registration, R"(Software\Microsoft\Windows\CurrentVersion\RunOnce)"};
    }
    // What a later attempt's download does after a discarded preparation removed the archive:
    // the same release is in the update cache again.
    void download_again() {
        fs::remove(archive);
        u::pack_archive(folder.path() / "new release", "0.1.1", archive);
        digest = fv::download::sha256_file(archive, true);
    }
    fv::update::Handoff prepare() {
        return u::prepare_update(context, archive, digest, "0.1.0", "0.1.1", 0, 0);
    }
};

inline std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace fv::update_native::test
