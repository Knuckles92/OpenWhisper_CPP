// The rollback demo's switch on the helper's side (M10.12, packaging/windows/smoke-update.ps1):
// FIORIVOCE_TEST_FAIL_HEALTH set where the updater runs reaches the health launch it starts, and a
// health launch that exits 1 is rolled back. The application's half (bootstrap exits 1) is
// tests/ui_shell/test_bootstrap.cpp. Off Windows the new application is a shell script that
// behaves as the switch makes fiorivoce.exe behave.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <optional>
#include <string>

#include "prepare_support.h"
#include "testkit/testkit.h"
#include "update/release_identity.h"
#include "update/testing/fake_mutex_api.h"
#include "update_native/journal.h"
#include "update_native/manifest.h"
#include "update_native/native_platform.h"
#include "update_native/transaction.h"

namespace fs = std::filesystem;
namespace u = fv::update_native;
using u::test::PrepareFixture;
using u::test::read_text;

#ifndef _WIN32
namespace {
struct NoRegistry final : u::RegistryActions {
    void verify(const u::UpdateJournal&) override {}
    void install_recovery(const u::UpdateJournal&) override {}
    void clear_recovery(const u::UpdateJournal&) override {}
    void update(const u::UpdateJournal&) override {}
    void restore(const u::UpdateJournal&) override {}
};

// Restores the variable a test set.
struct ScopedVariable {
    std::string name;
    std::optional<std::string> saved;
    ScopedVariable(std::string variable, const std::optional<std::string>& value)
        : name(std::move(variable)), saved(fv::testkit::get_env(name)) {
        fv::testkit::set_env(name, value);
    }
    ~ScopedVariable() { fv::testkit::set_env(name, saved); }
};
} // namespace

TEST_CASE("a health launch made to fail by FIORIVOCE_TEST_FAIL_HEALTH is rolled back",
          "[update_native]") {
    const bool fail = GENERATE(false, true);
    const ScopedVariable variable("FIORIVOCE_TEST_FAIL_HEALTH",
                                  fail ? std::optional<std::string>("1") : std::nullopt);
    // As fiorivoce.exe --update-health: exit 1 under the switch, else acknowledge ($2 is the
    // token, $4 the transaction) and stay alive like a started application.
    PrepareFixture f("#!/bin/sh\n"
                     "[ \"$FIORIVOCE_TEST_FAIL_HEALTH\" = 1 ] && exit 1\n"
                     "tx=\"$(dirname \"$0\")/../data/updates/tx/$4\"\n"
                     "printf %s \"$2\" > \"$tx/healthy.part\" && "
                     "mv \"$tx/healthy.part\" \"$tx/healthy\"\n"
                     "exec sleep 2\n");
    const auto id = f.prepare().transaction_id;
    const auto paths = u::journal_paths(f.context, id);
    fs::permissions(paths.candidate_dir / "fiorivoce.exe", fs::perms::owner_exec,
                    fs::perm_options::add);
    auto journal = u::load_journal(f.context, id);
    NoRegistry registry;
    fv::update::testing::FakeMutexApi mutex;
    u::NativePlatform platform(f.context, id, registry, mutex, fv::update::release_identity());
    if (fail) {
        CHECK_THROWS_AS(u::apply_prepared(f.context, journal, platform), u::UpdateError);
        CHECK(journal.state == u::TransactionState::RolledBack);
        CHECK(read_text(f.install / "fiorivoce.exe") == "old app");
    } else {
        CHECK(u::apply_prepared(f.context, journal, platform) == u::TransactionState::Healthy);
        CHECK(read_text(f.install / "fiorivoce.exe") != "old app");
    }
}
#endif
