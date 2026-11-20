// Which installations may replace themselves (desktop.h: native_update_eligible), from
// OpenWhisper's TestPreserveAndEligibility (tests/test_app_update_apply.py).

#include <catch2/catch_test_macros.hpp>

#include "base/env.h"
#include "base/identity.h"
#include "base/paths.h"
#include "update_native/desktop.h"

#include <filesystem>
#include <vector>

namespace {
namespace u = fv::update_native;

// The registered per-user installation, run by its own unelevated user.
u::NativeEligibility eligible() {
    u::NativeEligibility facts;
    facts.standard_user = true;
    facts.probe.registered = true;
    facts.probe.install_dir = "C:/Users/u/AppData/Local/Programs/OpenWhisper_CPP";
    facts.probe.running_dir = facts.probe.install_dir;
    facts.default_data_root = true;
    facts.registered_program = true;
    facts.inventory_present = true;
    return facts;
}
} // namespace

TEST_CASE("the registered per-user installation may update itself", "[update_native]") {
    CHECK(u::native_update_eligible(eligible()));
    CHECK_FALSE(u::native_update_eligible({}));
}

// from
// tests/test_app_update_apply.py::TestPreserveAndEligibility::test_elevated_process_is_not_native_eligible
TEST_CASE("an elevated process is not native eligible", "[update_native]") {
    auto facts = eligible();
    // Also the answer when the token could not be read at all.
    facts.standard_user = false;
    CHECK_FALSE(u::native_update_eligible(facts));
}

// from tests/test_app_update_apply.py::TestPreserveAndEligibility::test_hklm_is_not_native_eligible
TEST_CASE("an all-users registration is not native eligible", "[update_native]") {
    auto facts = eligible();
    facts.probe.all_users = true;
    CHECK_FALSE(u::native_update_eligible(facts));
}

TEST_CASE("only the registered program with its inventory and default data is eligible",
          "[update_native]") {
    SECTION("no registration") {
        auto facts = eligible();
        facts.probe.registered = false;
        CHECK_FALSE(u::native_update_eligible(facts));
    }
    SECTION("a copy run from another folder") {
        auto facts = eligible();
        facts.probe.running_dir = "D:/portable/OpenWhisper_CPP";
        CHECK_FALSE(u::native_update_eligible(facts));
    }
    SECTION("another program of the folder") {
        auto facts = eligible();
        facts.registered_program = false;
        CHECK_FALSE(u::native_update_eligible(facts));
    }
    SECTION("an installation without its release inventory") {
        auto facts = eligible();
        facts.inventory_present = false;
        CHECK_FALSE(u::native_update_eligible(facts));
    }
    SECTION("an installation under Program Files") {
        auto facts = eligible();
        facts.under_program_files = true;
        CHECK_FALSE(u::native_update_eligible(facts));
    }
    SECTION("a data root the helper would not find") {
        auto facts = eligible();
        facts.default_data_root = false;
        CHECK_FALSE(u::native_update_eligible(facts));
    }
}

// from app_update_apply.py:_is_under_program_files: the folder itself or anything inside one of
// the three roots, never a sibling whose name only starts like one.
TEST_CASE("a folder under a Program Files root is found", "[update_native]") {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() / "fv-program-files";
    const std::vector<fs::path> roots{"", root, root / "x86"};
    CHECK(u::under_program_files(root, roots));
    CHECK(u::under_program_files(root / "OpenWhisper_CPP", roots));
    CHECK(u::under_program_files(root / "x86" / "OpenWhisper_CPP" / ".", roots));
    fs::path sibling = root;
    sibling += " Old";
    CHECK_FALSE(u::under_program_files(sibling / "OpenWhisper_CPP", roots));
    CHECK_FALSE(u::under_program_files(root.parent_path() / "Programs" / "OpenWhisper_CPP", roots));
    CHECK_FALSE(u::under_program_files(root / "OpenWhisper_CPP", {}));
}

#ifndef _WIN32
TEST_CASE("there is no native update off Windows", "[update_native]") {
    const fv::Paths paths = fv::Paths::resolve(fv::identity(), fv::system_env());
    const auto actions = u::native_update_actions(paths, eligible().probe);
    CHECK_FALSE(actions.prepare);
    CHECK_FALSE(actions.discard);
}
#endif
