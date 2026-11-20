// A03-13 in the composed app: Meeting Mode's engine lease is the controller's
// release_local_engine / restore_local_engine (MeetingRuntime._model_lease,
// application_controller.py 651-694). MeetingFeature provides the lease and, at install, routes its
// dictation half through the ApplicationController, so the reload machine knows of it: the card
// reads "Released while Meeting Mode runs" and the restore says "Reloading speech engine..." with
// the busy state.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "meeting/model_lease.h"
#include "ui_meeting/meeting_feature.h"

using fvtest::ControllerRig;
using fvtest::waitUntil;
using namespace std::chrono_literals;
namespace speech = fv::speech;

TEST_CASE("the meeting feature's engine lease goes through the controller's reload machine",
          "[test_application_controller][widget]") {
    // The meeting services need the HTTP client and the credential store.
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    // Meetings with a model of their own release the dictation slot.
    options.settings["meeting_speech_separate"] = true;
    ControllerRig rig(options);
    fv::MeetingFeature meeting;
    meeting.provide(rig.context());
    meeting.install(rig.context());
    REQUIRE(rig.startEngine());
    auto* lease = rig.context().find<fv::meeting::SpeechModelLease>();
    REQUIRE(lease != nullptr);
    std::vector<std::string> shown;
    auto seen = rig.app().device_info_update.connect(
        rig.uiExecutor(),
        [&shown](std::string info, std::optional<bool>) { shown.push_back(info); });
    std::vector<bool> busy;
    auto busyLink = rig.app().engine_busy_changed.connect(
        rig.uiExecutor(), [&busy](bool value) { busy.push_back(value); });

    // A meeting's pass takes the lease on its own thread.
    auto acquired = std::async(std::launch::async, [lease] { return lease->acquire(); });
    REQUIRE(waitUntil([&acquired] { return acquired.wait_for(0ms) == std::future_status::ready; }));
    CHECK(acquired.get());
    CHECK(rig.app().reload_machine().released_for_lease());
    CHECK(waitUntil([&shown] {
        return std::ranges::find(shown, "Released while Meeting Mode runs") != shown.end();
    }));

    const auto loadsBefore = std::ranges::count(rig.engine().ops(), std::string("load"));
    auto released = std::async(std::launch::async, [lease] { lease->release(); });
    REQUIRE(waitUntil([&released] { return released.wait_for(0ms) == std::future_status::ready; }));
    REQUIRE(waitUntil([&rig, loadsBefore] {
        return std::ranges::count(rig.engine().ops(), std::string("load")) > loadsBefore &&
               rig.app().reload_machine().settled();
    }));
    CHECK(rig.hasStatus("Reloading speech engine..."));
    CHECK(std::ranges::find(busy, true) != busy.end());
    CHECK_FALSE(rig.app().reload_machine().released_for_lease());
    CHECK(rig.speech().readiness(speech::Slot::Dictation).state == speech::ReadyState::Ready);
}
