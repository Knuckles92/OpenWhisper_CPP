// The live preview as transcription.preview, through the controller's graph
// (application_controller.py PreviewEvents; test_automation_fix_pipeline_lifecycle.py): the
// streaming runtime's text reaches the app's event bus at most four times a second, the newest
// held text still goes out, and a final preview goes out at once and drops the held one.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "automation/event_bus.h"
#include "controller_support.h"
#include "dictation/streaming_runtime.h"

using namespace std::chrono_literals;
using fvtest::waitUntil;

namespace {

struct Previews {
    std::shared_ptr<fv::automation::EventBus> bus = std::make_shared<fv::automation::EventBus>();
    std::shared_ptr<fv::automation::EventBus> previous = fv::automation::set_bus(bus);
    std::mutex mutex;
    std::vector<std::pair<std::string, bool>> sent;
    std::vector<std::string> jobs;
    std::shared_ptr<fv::automation::EventSubscription> subscription = bus->subscribe(
        [this](const fv::automation::Event& event) {
            std::lock_guard lock(mutex);
            sent.emplace_back(event.data.value("text", std::string()),
                              event.data.value("final", false));
            jobs.push_back(event.data.value("job_id", std::string()));
        },
        std::vector<std::string>{"transcription.preview"});

    ~Previews() {
        fv::automation::set_bus(previous);
        bus->close();
    }
    std::vector<std::pair<std::string, bool>> all() {
        std::lock_guard lock(mutex);
        return sent;
    }
};

using Sent = std::vector<std::pair<std::string, bool>>;

} // namespace

TEST_CASE("test_the_last_throttled_preview_still_goes_out", "[widget]") {
    Previews previews;
    fvtest::ControllerRig rig;
    REQUIRE(rig.startEngine());
    REQUIRE(rig.app().start_recording());
    REQUIRE(waitUntil([&] { return rig.runtime().is_recording(); }));
    auto& streaming = rig.context().service<fv::dictation::StreamingRuntime>();
    streaming.streaming_text_update.emit("hel", false);
    streaming.streaming_text_update.emit("hello", false);     // within a quarter second: held
    streaming.streaming_text_update.emit("hello wor", false); // replaces the held one
    REQUIRE(waitUntil([&] { return previews.all().size() >= 2; }, 5s));
    waitUntil([] { return false; }, 400ms);
    CHECK(previews.all() == Sent{{"hel", false}, {"hello wor", false}});
    // Each is the recording's job's.
    const auto job = rig.runtime().current_job();
    REQUIRE(job != nullptr);
    std::lock_guard lock(previews.mutex);
    for (const auto& id : previews.jobs) {
        CHECK(id == job->id);
    }
}

TEST_CASE("test_a_final_preview_goes_out_at_once_and_drops_the_held_one", "[widget]") {
    Previews previews;
    fvtest::ControllerRig rig;
    REQUIRE(rig.startEngine());
    REQUIRE(rig.app().start_recording());
    REQUIRE(waitUntil([&] { return rig.runtime().is_recording(); }));
    auto& streaming = rig.context().service<fv::dictation::StreamingRuntime>();
    streaming.streaming_text_update.emit("hel", false);
    streaming.streaming_text_update.emit("hello", false);
    streaming.streaming_text_update.emit("hello world", true);
    waitUntil([] { return false; }, 400ms);
    CHECK(previews.all() == Sent{{"hel", false}, {"hello world", true}});
}
