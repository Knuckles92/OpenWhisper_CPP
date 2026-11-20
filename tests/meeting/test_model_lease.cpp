// meeting/refinalize.py acquire_model_lease and release_model_lease, and the application's lease
// over the speech service (architecture 5.6) that replaces OpenWhisper's release_local_engine and
// restore_local_engine pair.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "meeting/model_lease.h"
#include "recovery_support.h"

using namespace fv::meeting;
using namespace fv::meeting::test;
using fv::speech::ReadyState;
using fv::speech::Slot;

namespace {

// Waits (bounded) until the slot has left Loading.
fv::speech::Readiness settled(SpeechRig& rig, Slot slot) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (rig.service.readiness(slot).state == ReadyState::Loading &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return rig.service.readiness(slot);
}

} // namespace

TEST_CASE("acquire_model_lease and release_model_lease never throw", "[meeting][refinalize]") {
    CHECK_FALSE(acquire_model_lease(nullptr));
    release_model_lease(nullptr);

    NullModelLease none;
    CHECK_FALSE(acquire_model_lease(&none));

    std::vector<std::string> calls;
    FunctionModelLease failing(
        [&calls]() -> bool {
            calls.emplace_back("acquire");
            throw std::runtime_error("controller gone");
        },
        [&calls] {
            calls.emplace_back("release");
            throw std::runtime_error("reload refused");
        });
    CHECK_FALSE(acquire_model_lease(&failing));
    release_model_lease(&failing);
    CHECK(calls == std::vector<std::string>{"acquire", "release"});

    FunctionModelLease taking([] { return true; }, [] {});
    CHECK(acquire_model_lease(&taking));
}

TEST_CASE("a shared meeting model is held by an engine lease, never unloaded",
          "[meeting][refinalize]") {
    SpeechRig rig;
    rig.select();
    REQUIRE(settled(rig, Slot::Dictation).state == ReadyState::Ready);
    SpeechModelLease lease(rig.service, [] { return false; });

    REQUIRE(acquire_model_lease(&lease));
    CHECK(rig.service.lease_holders() == std::vector<std::string>{"meeting-finalization"});
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Ready);

    // A second pass (another meeting) overlaps the first: the first one's hold must survive it.
    REQUIRE(acquire_model_lease(&lease));
    CHECK(rig.service.lease_holders().size() == 2);
    release_model_lease(&lease);
    CHECK(rig.service.lease_holders() == std::vector<std::string>{"meeting-finalization"});
    release_model_lease(&lease);
    CHECK(rig.service.lease_holders().empty());

    // A release without an acquire gives back nothing.
    release_model_lease(&lease);
    CHECK(rig.service.lease_holders().empty());
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("a separate meeting model releases dictation until the last pass ends",
          "[meeting][refinalize]") {
    SpeechRig rig;
    rig.select();
    REQUIRE(settled(rig, Slot::Dictation).state == ReadyState::Ready);
    SpeechModelLease lease(rig.service, [] { return true; });

    REQUIRE(acquire_model_lease(&lease));
    CHECK(rig.service.lease_holders().empty());
    const fv::speech::Readiness released = rig.service.readiness(Slot::Dictation);
    CHECK(released.state == ReadyState::Offline);
    CHECK(released.device_info == "Released while Meeting Mode runs");

    REQUIRE(acquire_model_lease(&lease));
    release_model_lease(&lease);
    // The other pass still runs: dictation stays released.
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Offline);

    release_model_lease(&lease);
    CHECK(settled(rig, Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("an override meeting slot is loaded once and released back to dictation",
          "[meeting][refinalize]") {
    SpeechRig rig;
    rig.select();
    REQUIRE(settled(rig, Slot::Dictation).state == ReadyState::Ready);
    int selections = 0;
    bool separate = true;
    SpeechModelLease lease(
        rig.service, [&] { return separate; }, "meeting-finalization",
        [&] {
            ++selections;
            fv::speech::BackendConfig config;
            config.backend = "parakeet";
            config.model_key = "separate-meeting-model";
            return config;
        });

    REQUIRE(lease.acquire());
    CHECK(settled(rig, Slot::Meeting).state == ReadyState::Ready);
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Offline);

    // Changing the settings during a pass only affects the next independent group of passes.
    separate = false;
    REQUIRE(lease.acquire());
    CHECK(selections == 1);
    lease.release();
    CHECK(rig.service.readiness(Slot::Dictation).state == ReadyState::Offline);
    lease.release();
    CHECK(settled(rig, Slot::Dictation).state == ReadyState::Ready);
    CHECK(rig.service.generation(Slot::Meeting) == rig.service.generation(Slot::Dictation));

    REQUIRE(lease.acquire());
    CHECK(selections == 1);
    CHECK(rig.service.lease_holders() == std::vector<std::string>{"meeting-finalization"});
    lease.release();
}
