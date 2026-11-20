// The launch gate an installed copy shares with its setup and its native updater: OpenWhisper's
// tests/test_update_startup_gate.py, over a fake mutex API and a fake clock. The real kernel
// mutexes are tried by an [interactive] test.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <deque>
#include <string>
#include <vector>

#include "base/testing/fake_clock.h"
#include "download_support.h"
#include "update/gates.h"
#include "update/release_identity.h"
#include "update/testing/fake_mutex_api.h"

namespace update = fv::update;
using update::testing::FakeMutexApi;
using namespace fv::update::test;

namespace {

const update::ReleaseIdentity& identity() {
    return update::release_identity();
}

update::InstallProbe development_build() {
    auto probe = installed_probe("C:/Users/u/Programs/FioriVoce");
    probe.running_dir = "D:/src/fiorivoce/out/build/bin";
    return probe;
}

} // namespace

// from tests/test_update_startup_gate.py::test_non_frozen_launch_is_not_gated
TEST_CASE("non frozen launch is not gated", "[test_update_startup_gate]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    // Even a running setup does not hold a development build back.
    mutexes.held_elsewhere.insert(identity().setup_mutex());
    auto gate = update::LaunchGate::enter(identity(), development_build(), mutexes, clock);
    REQUIRE(gate.has_value());
    CHECK_FALSE(gate->installed());
    CHECK_FALSE(gate->holds_app_mutexes());
    CHECK(mutexes.created_calls.empty());
    CHECK(mutexes.exists_calls.empty());
    CHECK(mutexes.waits.empty());
    CHECK_FALSE(gate->reacquire());
    CHECK(mutexes.created_calls.empty());
}

// from
// tests/test_update_startup_gate.py::test_the_launch_that_follows_setup_waits_instead_of_vanishing
TEST_CASE("the launch that follows setup waits instead of vanishing",
          "[test_update_startup_gate]") {
    // Inno's postinstall launch always runs while setup still holds its mutex.
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    std::deque<bool> held{true, true, false};
    const auto setup = identity().setup_mutex();
    mutexes.scripted_exists = [&](std::string_view name) {
        if (name != setup || held.empty()) {
            return false;
        }
        const bool value = held.front();
        held.pop_front();
        return value;
    };
    auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE(gate.has_value());
    CHECK(mutexes.waits.size() == 2);
    CHECK(mutexes.waits[0] == std::chrono::milliseconds(250));
    CHECK(gate->installed());
    CHECK(gate->holds_app_mutexes());
    // The setup mutex was taken for a moment and let go; the app mutexes stay.
    auto owned = mutexes.owned();
    std::ranges::sort(owned);
    auto expected = identity().app_mutexes();
    std::ranges::sort(expected);
    CHECK(owned == expected);
}

// from tests/test_update_startup_gate.py::test_a_setup_that_never_ends_refuses_the_launch
TEST_CASE("a setup that never ends refuses the launch", "[test_update_startup_gate]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    mutexes.held_elsewhere.insert(identity().setup_mutex());
    const auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE_FALSE(gate.has_value());
    CHECK(gate.error().find("Setup is still running") != std::string::npos);
    CHECK(gate.error() == "FioriVoce Setup is still running, so FioriVoce did not start. Finish "
                          "the installer, then start FioriVoce again.");
    // 120 s at a 0.25 s poll.
    CHECK(mutexes.waits.size() == 480);
    CHECK(mutexes.owned().empty());
}

TEST_CASE("a native update admits only the verified health launch", "[test_update_startup_gate]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    mutexes.held_elsewhere.insert(identity().update_mutexes().front());
    mutexes.held_elsewhere.insert(identity().setup_mutex());
    // from tests/test_update_startup_gate.py::test_update_mutex_without_token_exits: the launch
    // is refused at once, with the sentence main() shows in the native box before leaving with 0.
    const auto refused = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "FioriVoce is finishing an update. Try again in a moment.");
    CHECK(mutexes.waits.empty());
    CHECK(mutexes.owned().empty());
    // from tests/test_update_startup_gate.py::test_health_token_may_launch_during_update: only
    // the helper's own health launch, its token already verified, gets through.
    auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock, true);
    REQUIRE(gate);
    CHECK(gate->holds_app_mutexes());
    CHECK(mutexes.waits.empty());
    CHECK_FALSE(update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock, true));
}

TEST_CASE("a health launch whose helper is gone does not start", "[update]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    // No update mutex: nobody is waiting for this acknowledgement any more.
    const auto refused =
        update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock, true);
    REQUIRE_FALSE(refused);
    CHECK(refused.error() == "The update helper is no longer running.");
    CHECK(mutexes.owned().empty());
}

TEST_CASE("a second instance does not leave because the app mutex is taken", "[update]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    mutexes.held_elsewhere.insert(identity().app_mutex_global());
    auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE(gate.has_value());
    // All or none: the local mutex it did get is given back.
    CHECK_FALSE(gate->holds_app_mutexes());
    CHECK(mutexes.owned().empty());
}

TEST_CASE("a setup that starts after the wait ends the launch without a word", "[update]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    const auto setup = identity().setup_mutex();
    int polls = 0;
    // Free while the gate looks, taken when it reaches for it.
    mutexes.scripted_exists = [&](std::string_view name) {
        return name == setup && polls++ > 0;
    };
    const auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE_FALSE(gate.has_value());
    CHECK(gate.error().empty());
}

TEST_CASE("the app mutexes are let go for the setup and taken back after a failed start",
          "[update]") {
    fv::FakeClock clock;
    FakeMutexApi mutexes(clock);
    auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE(gate.has_value());
    REQUIRE(gate->holds_app_mutexes());
    gate->release_for_setup();
    CHECK_FALSE(gate->holds_app_mutexes());
    CHECK(mutexes.owned().empty());
    CHECK(gate->reacquire());
    CHECK(gate->holds_app_mutexes());
    CHECK(mutexes.owned().size() == identity().app_mutexes().size());
    {
        const auto moved = std::move(*gate);
        CHECK(moved.holds_app_mutexes());
    }
    CHECK(mutexes.owned().empty());
}

TEST_CASE("the first argument decides whether the gate runs", "[update]") {
    using Args = std::vector<std::string>;
    for (const auto* skipped :
         {"--self-test-gui", "--workflow-smoke", "--version", "--help", "--diagnostics"}) {
        CHECK(update::skips_launch_gate(Args{skipped}));
    }
    CHECK_FALSE(update::skips_launch_gate(Args{}));
    CHECK_FALSE(update::skips_launch_gate(Args{"--minimized", "--version"}));
    CHECK_FALSE(update::skips_launch_gate(Args{"--self-test"}));
}

#ifdef _WIN32
// The kernel's named mutexes: creates the identity's real app and setup mutex names, which a
// running FioriVoce or its setup would see. Written for the owner; never run by the gate.
TEST_CASE("the kernel's mutexes gate a launch", "[update][interactive]") {
    auto& mutexes = update::system_mutex_api();
    fv::FakeClock clock;
    auto gate = update::LaunchGate::enter(identity(), installed_probe(), mutexes, clock);
    REQUIRE(gate.has_value());
    REQUIRE(gate->holds_app_mutexes());
    for (const auto& name : identity().app_mutexes()) {
        CHECK(mutexes.exists(name));
        CHECK_FALSE(mutexes.create(name).has_value());
    }
    CHECK_FALSE(mutexes.exists(identity().setup_mutex()));
    gate->release_for_setup();
    for (const auto& name : identity().app_mutexes()) {
        CHECK_FALSE(mutexes.exists(name));
    }
}
#endif
