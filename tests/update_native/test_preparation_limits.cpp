// What refuses a native update before it starts (audit A16-17): the free-space rule of
// prepare_candidate (check_free_space, _SPACE_MARGIN) and the gate refusals of
// _exclusive_preparation, commit_prepared_update and recover_transaction
// (services/app_update_apply.py).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "base/atomic_file.h"
#include "testkit/testkit.h"
#include "update/release_identity.h"
#include "update/testing/fake_mutex_api.h"
#include "update_native/journal.h"
#include "update_native/manifest.h"
#include "update_native/native_platform.h"
#include "update_native/prepare.h"
#include "update_native/transaction_gates.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using Catch::Matchers::Equals;

namespace {

// int(required * 1.15), as Python computes it.
std::int64_t with_margin(std::int64_t required) {
    return static_cast<std::int64_t>(static_cast<double>(required) * 1.15);
}

std::string shortfall(std::int64_t needed, std::int64_t free) {
    return "Not enough disk space: " + std::to_string(needed) + " bytes needed, " +
           std::to_string(free) + " free.";
}

struct NoRegistry final : u::RegistryActions {
    void verify(const u::UpdateJournal&) override {}
    void install_recovery(const u::UpdateJournal&) override {}
    void clear_recovery(const u::UpdateJournal&) override {}
    void update(const u::UpdateJournal&) override {}
    void restore(const u::UpdateJournal&) override {}
};

} // namespace

TEST_CASE("a preparation needs room beside the install for both trees and room for the archive",
          "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const fs::path install = temp.path() / "programs" / "FioriVoce";
    const fs::path updates = temp.path() / "data" / "updates";
    fs::create_directories(install);
    fs::create_directories(updates);
    std::uint64_t beside_install = 0;
    std::uint64_t in_cache = 0;
    std::vector<fs::path> asked;
    const u::FreeBytes free_bytes = [&](const fs::path& path) {
        asked.push_back(path);
        return path == updates ? in_cache : beside_install;
    };
    // The current install (400) and the unpacked release (600) stay side by side until the
    // update is confirmed; the archive (200) lies in the cache.
    const auto needed_install = with_margin(1000);
    const auto needed_archive = with_margin(200);

    SECTION("enough on both drives") {
        beside_install = static_cast<std::uint64_t>(needed_install);
        in_cache = static_cast<std::uint64_t>(needed_archive);
        CHECK_NOTHROW(u::check_preparation_space(install, 400, 600, updates, 200, free_bytes));
        CHECK(asked == std::vector<fs::path>{install.parent_path(), updates});
    }
    SECTION("the unpacked release alone fits, the rollback tree beside it does not") {
        beside_install = static_cast<std::uint64_t>(needed_install - 1);
        in_cache = 1u << 30;
        CHECK_THROWS_WITH(u::check_preparation_space(install, 400, 600, updates, 200, free_bytes),
                          Equals(shortfall(needed_install, needed_install - 1)));
    }
    SECTION("the update cache is short") {
        beside_install = 1u << 30;
        in_cache = static_cast<std::uint64_t>(needed_archive - 1);
        CHECK_THROWS_AS(u::check_preparation_space(install, 400, 600, updates, 200, free_bytes),
                        u::UpdateError);
        CHECK_THROWS_WITH(u::check_preparation_space(install, 400, 600, updates, 200, free_bytes),
                          Equals(shortfall(needed_archive, needed_archive - 1)));
    }
}

TEST_CASE("the size of the installed tree counts every file below it", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    const fs::path install = temp.path() / "app";
    fs::create_directories(install / "platforms");
    fv::write_file_atomic(install / "fiorivoce.exe", std::string(300, 'a'));
    fv::write_file_atomic(install / "platforms" / "qwindows.dll", std::string(100, 'b'));
    CHECK(u::tree_size_bytes(install) == 400);
    CHECK(u::tree_size_bytes(temp.path() / "missing") == 0);
}

TEST_CASE("a held gate names what holds it", "[update_native]") {
    fv::update::testing::FakeMutexApi mutex;
    const auto& identity = fv::update::release_identity();
    u::GateHeld held = u::GateHeld::Update;

    SECTION("the setup") {
        mutex.held_elsewhere.insert(identity.setup_mutex());
        CHECK_FALSE(
            u::NativeTransactionGates::acquire(mutex, identity, u::SetupGate::Acquire, &held));
        CHECK(held == u::GateHeld::Setup);
        CHECK(u::preparation_refusal(held) == "Setup is already running.");
        CHECK(u::transaction_refusal(held) == "The setup program is already running.");
    }
    SECTION("another update") {
        held = u::GateHeld::Setup;
        mutex.held_elsewhere.insert(identity.update_mutexes().back());
        CHECK_FALSE(
            u::NativeTransactionGates::acquire(mutex, identity, u::SetupGate::Acquire, &held));
        CHECK(held == u::GateHeld::Update);
        CHECK(u::preparation_refusal(held) == "Another update is already in progress.");
        CHECK(u::transaction_refusal(held) == "Another update is already in progress.");
    }
    SECTION("an installer that is not running") {
        CHECK_FALSE(u::NativeTransactionGates::acquire(mutex, identity,
                                                       u::SetupGate::HeldByInstaller, &held));
        CHECK(held == u::GateHeld::Setup);
    }
    // Nothing stays held after a refusal.
    CHECK(mutex.owned().empty());
}

TEST_CASE("the helper's commit refuses with the gate that is held", "[update_native]") {
    fv::testkit::TempDataRoot temp;
    u::JournalContext context{temp.path() / "data", temp.path() / "programs" / "FioriVoce",
                              "{01234567-89AB-CDEF-0123-456789ABCDEF}"};
    constexpr auto id = "0123456789abcdef0123456789abcdef";
    fs::create_directories(context.install_dir);
    fs::create_directories(u::journal_paths(context, id).transaction_dir);
    NoRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    const auto& identity = fv::update::release_identity();
    u::NativePlatform platform(context, id, registry, mutex, identity);
    mutex.held_elsewhere.insert(identity.setup_mutex());
    CHECK_THROWS_WITH(platform.acquire_gates(), Equals("The setup program is already running."));
    mutex.held_elsewhere.clear();
    mutex.held_elsewhere.insert(identity.update_mutexes().front());
    CHECK_THROWS_WITH(platform.acquire_gates(), Equals("Another update is already in progress."));
    mutex.held_elsewhere.clear();
    CHECK(platform.acquire_gates() != nullptr);
}
