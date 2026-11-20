// The meeting model lease of a start (services/runtime/meeting.py _start_worker,
// _on_asr_released_event, _on_background_ready_event, _start_failed) over the speech service:
// a meeting on the dictation model borrows the engine through an EngineLease; with a model of its
// own the dictation slot is released and restored on asr_released, background_ready or failure.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "meeting/model_lease.h"
#include "recovery_support.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;
using fv::speech::ReadyState;
using fv::speech::Slot;

namespace {

fv::speech::Readiness settled(fv::meeting::test::SpeechRig& speech, Slot slot) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (speech.service.readiness(slot).state == ReadyState::Loading &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return speech.service.readiness(slot);
}

struct LeaseRig {
    explicit LeaseRig(bool separate) {
        speech.select();
        REQUIRE(settled(speech, Slot::Dictation).state == ReadyState::Ready);
        lease = std::make_shared<SpeechModelLease>(
            speech.service, [separate] { return separate; }, "meeting");
        rig = std::make_unique<RuntimeRig>(
            [this](MeetingRuntimeDeps& deps) { deps.model_lease = lease; });
    }

    fv::meeting::test::SpeechRig speech;
    std::shared_ptr<SpeechModelLease> lease;
    std::unique_ptr<RuntimeRig> rig;
};

} // namespace

TEST_CASE("a meeting on the dictation model borrows the engine through a lease",
          "[meeting][runtime]") {
    LeaseRig l(false);
    l.rig->rt->start_meeting(false);
    l.rig->drain();
    REQUIRE(l.rig->engines.size() == 1);
    CHECK(l.speech.service.lease_holders() == std::vector<std::string>{"meeting"});
    CHECK(l.speech.service.readiness(Slot::Dictation).state == ReadyState::Ready);

    l.rig->engines[0]->emit("asr_released");
    l.rig->engines[0]->emit("background_ready");
    l.rig->drain();
    CHECK(l.speech.service.lease_holders().empty());
    CHECK(l.speech.service.readiness(Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("a meeting with its own model releases dictation until asr_released",
          "[meeting][runtime]") {
    LeaseRig l(true);
    l.rig->rt->start_meeting(false);
    l.rig->drain();
    const auto released = l.speech.service.readiness(Slot::Dictation);
    CHECK(released.state == ReadyState::Offline);
    CHECK(released.device_info == "Released while Meeting Mode runs");
    CHECK(l.speech.service.lease_holders().empty());

    l.rig->engines[0]->emit("asr_released");
    l.rig->drain();
    CHECK(settled(l.speech, Slot::Dictation).state == ReadyState::Ready);
}

TEST_CASE("a meeting with its own model gives dictation back on background_ready or failure",
          "[meeting][runtime]") {
    SECTION("background_ready") {
        LeaseRig l(true);
        l.rig->rt->start_meeting(false);
        l.rig->drain();
        CHECK(l.speech.service.readiness(Slot::Dictation).state == ReadyState::Offline);
        l.rig->engines[0]->emit("background_ready");
        l.rig->drain();
        CHECK(settled(l.speech, Slot::Dictation).state == ReadyState::Ready);
    }
    SECTION("a failed start") {
        LeaseRig l(true);
        l.rig->make_engine = [](const MeetingEngineOptions&) {
            auto engine = std::make_shared<FakeEngine>();
            engine->on_start = []() -> Json {
                throw std::runtime_error("no devices");
            };
            return engine;
        };
        l.rig->rt->start_meeting(false);
        l.rig->drain();
        CHECK(l.rig->got.states.back()["status"] == "failed");
        CHECK(settled(l.speech, Slot::Dictation).state == ReadyState::Ready);
    }
}
