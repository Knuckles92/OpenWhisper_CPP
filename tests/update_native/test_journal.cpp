#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "base/hash.h"
#include "testkit/testkit.h"
#include "update_native/health.h"
#include "update_native/journal.h"
#include "update_native/manifest.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
namespace {
constexpr std::string_view kId = "0123456789abcdef0123456789abcdef";
constexpr std::string_view kAppId = "{01234567-89AB-CDEF-0123-456789ABCDEF}";
constexpr std::string_view kOtherId = "{FEDCBA98-7654-3210-FEDC-BA9876543210}";
constexpr std::string_view kRunOnce = "Software\\Microsoft\\Windows\\CurrentVersion\\RunOnce";

struct Fixture {
    fv::testkit::TempDataRoot temp;
    u::JournalContext context{temp.path() / "data", temp.path() / "programs" / "FioriVoce",
                              std::string(kAppId)};
    u::UpdateJournal journal = u::make_journal(context, kId);
    Fixture() {
        journal.old_version = "0.1.0";
        journal.new_version = "0.1.1";
        journal.old_display_version = "0.1.0";
        journal.registration.key_path =
            "Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\FioriVoce";
        journal.runonce_key = kRunOnce;
        context.expected_registration = journal.registration;
        context.expected_runonce_key = journal.runonce_key;
        journal.uninstaller_files = {"unins000.exe", "unins000.dat"};
        journal.compatibility_files = {
            {"unins000.exe", {{"size", 10}, {"sha256", std::string(64, 'a')}}},
            {"unins000.dat", {{"sha256", std::string(64, 'b')}, {"size", 3}}}};
        fs::create_directories(u::journal_paths(context, kId).transaction_dir);
    }
};
std::string read(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
} // namespace

TEST_CASE("journal parent identity is bound to an independently trusted executable",
          "[update_native]") {
    Fixture f;
    f.context.parent_exe = f.context.install_dir / "fiorivoce.exe";
    f.journal.parent_pid = 42;
    f.journal.parent_creation_time = 1234;
    f.journal.parent_exe = f.context.parent_exe;
    const auto paths = u::validate_journal_binding(f.context, f.journal, kId);
    CHECK(paths.parent_exe == f.context.parent_exe);
    f.journal.parent_exe = f.temp.path() / "other.exe";
    CHECK_THROWS_AS(u::validate_journal_binding(f.context, f.journal, kId), u::UpdateError);
}

TEST_CASE("native journal round trips and serializes sorted keys", "[update_native]") {
    Fixture f;
    f.journal.state = u::TransactionState::Prepared;
    f.journal.old_estimated_size_kb = 42;
    f.journal.archive_path = f.context.data_root / "updates" / "release.zip";
    u::save_journal(f.context, f.journal);
    const auto path = u::journal_paths(f.context, kId).journal_file;
    const auto raw = read(path);
    const auto json = fv::Json::parse(raw);
    std::vector<std::string> keys;
    for (auto it = json.begin(); it != json.end(); ++it)
        keys.push_back(it.key());
    CHECK(std::ranges::is_sorted(keys));
    CHECK(raw.find("\n  \"app_dir\"") != std::string::npos);
    CHECK_FALSE(json.contains("nvidia_preserved"));
    const auto loaded = u::load_journal(f.context, kId);
    CHECK(u::to_json(loaded) == u::to_json(f.journal));
    CHECK(loaded.health_token.size() == 32);
    CHECK(loaded.state == u::TransactionState::Prepared);
}

TEST_CASE("native health command needs a single complete transaction and token",
          "[update_native]") {
    std::vector<std::string> args;
    CHECK_FALSE(u::health_request(args));
    args = {"--update-health", std::string(32, 'a'), "--transaction-id", std::string(kId)};
    CHECK(u::health_request(args)->transaction_id == kId);
    args[1] = "bad";
    CHECK_THROWS(u::health_request(args));
    args.pop_back();
    CHECK_THROWS(u::health_request(args));
}
TEST_CASE("health acknowledgement binds the active journal, running version and complete image",
          "[update_native]") {
    Fixture f;
    fs::create_directories(f.context.install_dir);
    const auto executable = f.context.install_dir / fv::update::release_identity().app_exe;
    std::ofstream(executable, std::ios::binary) << "new image";
    f.journal.old_exe_sha256 = fv::sha256_hex("old image");
    f.journal.new_exe_sha256 = fv::sha256_hex("new image");
    f.journal.state = u::TransactionState::NewActive;
    u::save_journal(f.context, f.journal);
    const u::HealthRequest good{std::string(kId), f.journal.health_token};
    const auto marker = u::journal_paths(f.context, kId).transaction_dir / "healthy";
    CHECK(u::check_health(f.context, good, executable, "0.1.1", false));
    CHECK_FALSE(fs::exists(marker));
    CHECK_FALSE(u::check_health(f.context, {std::string(kId), std::string(32, '0')}, executable,
                                "0.1.1", true));
    CHECK_FALSE(u::check_health(f.context, good, executable, "0.1.0", true));
    CHECK_FALSE(fs::exists(marker));
    REQUIRE(u::check_health(f.context, good, executable, "0.1.1", true));
    CHECK(read(marker) == good.token);
    std::ofstream(executable, std::ios::binary) << "other";
    CHECK_FALSE(u::check_health(f.context, good, executable, "0.1.1", true));
}

TEST_CASE("journal paths come only from the trusted roots and transaction id", "[update_native]") {
    Fixture f;
    const auto trusted = u::validate_journal_binding(f.context, f.journal, kId);
    CHECK(trusted.install_dir == f.context.install_dir);
    CHECK(trusted.candidate_dir.filename() == "FioriVoce.new-01234567");
    CHECK(trusted.rollback_dir.filename() == "FioriVoce.old-01234567");
    SECTION("a journal cannot choose a different installation") {
        f.journal.app_dir = f.temp.path() / "other";
    }
    SECTION("a journal cannot redirect the candidate") {
        f.journal.candidate_dir = f.temp.path() / "candidate";
    }
    SECTION("a journal cannot redirect rollback") {
        f.journal.rollback_dir = f.temp.path() / "rollback";
    }
    SECTION("a journal cannot redirect the data root") {
        f.journal.appdata = f.temp.path() / "other";
    }
    SECTION("a journal cannot point at an outside archive") {
        f.journal.archive_path = f.temp.path() / "outside.zip";
    }
    SECTION("a journal cannot choose another id") {
        f.journal.transaction_id = "fedcba9876543210fedcba9876543210";
    }
    CHECK_THROWS_AS(u::validate_journal_binding(f.context, f.journal, kId), u::UpdateError);
    CHECK_THROWS_AS(u::journal_paths(f.context, "../escape"), u::UpdateError);
}

TEST_CASE("journal registry binding and identity cannot escape their allowed scope",
          "[update_native]") {
    Fixture f;
    SECTION("machine registry") {
        f.journal.registration.hive = fv::update::RegistryHive::LocalMachine;
    }
    SECTION("registration outside software") {
        f.journal.registration.key_path = "System\\Other";
    }
    SECTION("registration traversal") {
        f.journal.registration.key_path = "Software\\..\\Other";
    }
    SECTION("different but allowed registration") {
        f.journal.registration.key_path = "Software\\Other\\Uninstall";
    }
    SECTION("registration invalid view") {
        f.journal.registration.view = 7;
    }
    SECTION("runonce outside the allowed keys") {
        f.journal.runonce_key = "Software\\Other\\RunOnce";
    }
    SECTION("different but allowed runonce key") {
        f.journal.runonce_key = "Software\\FioriVoceTest\\case\\RunOnce";
    }
    SECTION("no independently trusted registration") {
        f.context.expected_registration.reset();
    }
    SECTION("production registration with foreign app id") {
        f.journal.app_id = kOtherId;
    }
    CHECK_THROWS_AS(u::validate_journal_binding(f.context, f.journal, kId), u::UpdateError);
}

TEST_CASE("a scoped test journal may use its own application id and RunOnce key",
          "[update_native]") {
    Fixture f;
    f.journal.registration.key_path = "Software\\FioriVoceTest\\case-1\\Uninstall";
    f.journal.runonce_key = "Software\\FioriVoceTest\\case-1\\RunOnce";
    f.journal.app_id = kOtherId;
    f.context.expected_registration = f.journal.registration;
    f.context.expected_runonce_key = f.journal.runonce_key;
    CHECK(u::is_test_journal(f.journal));
    CHECK_NOTHROW(u::validate_journal_binding(f.context, f.journal, kId));
    f.journal.registration.key_path = "Software\\FioriVoceTestOther\\case-1";
    CHECK_FALSE(u::is_test_journal(f.journal));
    CHECK_THROWS_AS(u::validate_journal_binding(f.context, f.journal, kId), u::UpdateError);
}

TEST_CASE("journal validates version, health token, digest and compatibility metadata",
          "[update_native]") {
    Fixture f;
    SECTION("non-upgrade") {
        f.journal.new_version = "0.1.0";
    }
    SECTION("pre-release version") {
        f.journal.new_version = "0.1.1-rc1";
    }
    SECTION("invalid display version") {
        f.journal.old_display_version = "unknown";
    }
    SECTION("negative installed size") {
        f.journal.old_estimated_size_kb = -1;
    }
    SECTION("invalid health token") {
        f.journal.health_token = "../escape";
    }
    SECTION("uppercase digest") {
        f.journal.old_exe_sha256 = std::string(64, 'A');
    }
    SECTION("unauthorized compatibility path") {
        f.journal.compatibility_files["other.dll"] = {{"size", 1},
                                                      {"sha256", std::string(64, 'a')}};
    }
    SECTION("invalid uninstaller name") {
        f.journal.uninstaller_files.push_back("../unins001.exe");
    }
    SECTION("negative compatibility size") {
        f.journal.compatibility_files["unins000.exe"]["size"] = -1;
    }
    SECTION("parent identity is not trusted") {
        f.journal.parent_pid = 42;
        f.journal.parent_exe = f.temp.path() / "elsewhere.exe";
        f.journal.parent_creation_time = 1;
    }
    CHECK_THROWS_AS(u::validate_journal_binding(f.context, f.journal, kId), u::UpdateError);
}

TEST_CASE("journal load rejects malformed and unsafe files", "[update_native]") {
    Fixture f;
    const auto path = u::journal_paths(f.context, kId).journal_file;
    SECTION("missing") {
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
    }
    SECTION("malformed") {
        fv::write_file_atomic(path, "{");
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
    }
    SECTION("incomplete") {
        fv::write_file_atomic(path, "{}");
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
    }
    SECTION("invalid state") {
        auto json = u::to_json(f.journal);
        json["state"] = "unknown";
        fv::write_file_atomic(path, json.dump());
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
    }
    SECTION("hard link") {
        const auto original = f.temp.path() / "original.json";
        fv::write_file_atomic(original, u::to_json(f.journal).dump());
        std::error_code error;
        fs::create_hard_link(original, path, error);
        if (error)
            SKIP("This filesystem cannot create a hard link.");
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
        CHECK_THROWS_AS(u::save_journal(f.context, f.journal), u::UpdateError);
    }
    SECTION("symbolic link") {
        const auto original = f.temp.path() / "original.json";
        fv::write_file_atomic(original, u::to_json(f.journal).dump());
        std::error_code error;
        fs::create_symlink(original, path, error);
        if (error)
            SKIP("This account cannot create a symbolic link.");
        CHECK_THROWS_AS(u::load_journal(f.context, kId), u::UpdateError);
        CHECK_THROWS_AS(u::save_journal(f.context, f.journal), u::UpdateError);
    }
}
