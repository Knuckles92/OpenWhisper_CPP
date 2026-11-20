// Ported from the controller tests of tests/test_engine_warmup.py, plus the reload_trace golden
// vectors of services.application_controller and the debounce edges of reload_whisper_model.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <stdexcept>
#include <string>
#include <thread>

#include "appcore/engine_reload.h"
#include "golden/golden.h"
#include "reload_harness.h"

using fv::ActivitySnapshot;
using fv::Json;
using fv::appcore::kReloadDebounce;
using fv::appcore::testing::ReloadHarness;
using fv::speech::ReadyState;

namespace {

const Json kReadyInfo = Json::array({"device_info", "Parakeet TDT 0.6B v3 | cuda", true});

ReadyState ready_state(const std::string& name) {
    if (name == "Ready") {
        return ReadyState::Ready;
    }
    if (name == "Failed") {
        return ReadyState::Failed;
    }
    if (name == "ModelMissing") {
        return ReadyState::ModelMissing;
    }
    if (name == "RuntimeMissing") {
        return ReadyState::RuntimeMissing;
    }
    throw std::invalid_argument("no ReadyState " + name);
}

ActivitySnapshot activity_from(const Json& flags) {
    ActivitySnapshot activity;
    activity.recording = flags.at("recording").get<bool>();
    activity.transcribing = flags.at("transcribing").get<bool>();
    activity.meeting_active = flags.at("meeting_active").get<bool>();
    activity.host_update_active = flags.at("host_update_active").get<bool>();
    return activity;
}

// Applies one ["in", ...] event of a golden trace to the machine; the event goes into the trace
// first, so the commands it causes follow it as they do in the vector.
void apply_input(ReloadHarness& h, const Json& event) {
    const auto name = event.at(1).get<std::string>();
    if (name == "release") {
        h.trace.push_back(Json::array({"in", "release"}));
        const std::size_t at = h.trace.size() - 1;
        bool released = false;
        h.on_ui([&] { released = h.machine.release_for_lease(); });
        h.trace[at].push_back(released);
        return;
    }
    h.trace.push_back(event);
    if (name == "initial_load") {
        h.on_ui([&] { h.machine.initial_load(); });
    } else if (name == "request") {
        const auto note = event.at(2).get<std::string>();
        h.on_ui([&] { h.machine.request(note); });
    } else if (name == "timer") {
        h.fire_timer();
    } else if (name == "activity") {
        h.set_activity(activity_from(event.at(2)));
    } else if (name == "load_finished") {
        h.load_finished(ready_state(event.at(2).get<std::string>()), event.at(3).get<std::string>(),
                        event.at(4).get<std::string>());
    } else if (name == "select") {
        const auto backend = event.at(2).get<std::string>();
        h.on_ui([&] { h.machine.select(backend, "parakeet-v3"); });
    } else if (name == "warmup_finished") {
        h.on_ui([&] { h.machine.on_warmup_finished(); });
    } else if (name == "restore") {
        h.on_ui([&] { h.machine.restore_after_lease(); });
    } else if (name == "engine_closed") {
        h.on_ui([&] { h.machine.on_engine_closed(); });
    } else {
        FAIL("unknown input " << name);
    }
}

} // namespace

TEST_CASE("reload traces replay the golden vectors", "[golden][engine_reload]") {
    fv::golden::for_each("services.application_controller", "reload_trace",
                         [](const fv::golden::Case& c) {
                             const Json expected = c.result<Json>();
                             ReloadHarness h;
                             for (const auto& event : expected) {
                                 if (event.at(0) == "in") {
                                     apply_input(h, event);
                                 }
                             }
                             INFO("expected " << expected.dump(1));
                             INFO("actual   " << h.trace.dump(1));
                             CHECK(fv::golden::equal(expected, h.trace));
                         });
}

// from
// tests/test_engine_warmup.py::test_reload_warms_after_load_then_clears_busy_and_sets_up_preview
TEST_CASE("reload warms after load then clears busy and sets up preview", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    REQUIRE(h.count("start_load") == 1);
    h.load_ready();
    REQUIRE(h.count("start_warmup") == 1);
    // The warm-up holds no lock of the machine: a Meeting Mode lease can act while it runs.
    CHECK_FALSE(h.machine.settled());
    h.on_ui([&] { h.machine.on_warmup_finished(); });

    // Ready as soon as the model is loaded, so a recording may start while it warms; busy and
    // the shared-engine preview wait for the warm-up.
    const auto warmup = h.index_of("start_warmup");
    CHECK(h.index_of("device_info") < warmup);
    const auto not_busy = h.index_of("busy_changed", warmup);
    REQUIRE(not_busy != ReloadHarness::npos);
    CHECK(h.trace[not_busy] == Json::array({"busy_changed", false}));
    CHECK(not_busy < h.index_of("streaming_setup_due"));
    CHECK(h.contains(kReadyInfo));
    CHECK_FALSE(h.machine.in_flight());
    CHECK(h.count("missing_runtime") == 0);
}

// from tests/test_engine_warmup.py::test_failed_warmup_still_finishes_the_reload
TEST_CASE("failed warmup still finishes the reload", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    REQUIRE(h.count("start_warmup") == 1);
    // The warm-up decode failed: the driver reports it finished all the same.
    h.on_ui([&] { h.machine.on_warmup_finished(); });

    CHECK(h.contains(Json::array({"busy_changed", false})));
    CHECK(h.count("streaming_setup_due") == 1);
    CHECK_FALSE(h.machine.in_flight());
    CHECK(h.machine.settled());
}

// from tests/test_engine_warmup.py::test_failed_load_is_not_warmed
TEST_CASE("failed load is not warmed", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_finished(ReadyState::Failed, {}, "native load failed");

    CHECK(h.count("start_warmup") == 0);
    CHECK(h.contains(Json::array({"status", "Engine load failed: native load failed"})));
    CHECK(h.contains(Json::array({"device_info", "native load failed", false})));
    CHECK(h.contains(Json::array({"busy_changed", false})));
    CHECK(h.count("streaming_setup_due") == 1);
    CHECK_FALSE(h.machine.in_flight());
}

// from tests/test_engine_warmup.py::test_missing_model_is_not_warmed
TEST_CASE("missing model is not warmed", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_finished(ReadyState::ModelMissing, "Download Parakeet TDT 0.6B v3 in Downloads.");

    CHECK(h.count("start_warmup") == 0);
    CHECK(h.count("ensure_model_available") == 1);
    CHECK(h.contains(Json::array({"busy_changed", false})));
    CHECK(h.count("missing_runtime") == 0);
    CHECK_FALSE(h.machine.in_flight());
}

// from tests/test_engine_warmup.py::test_missing_runtime_is_not_warmed
TEST_CASE("missing runtime is not warmed", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_finished(ReadyState::RuntimeMissing, "Install Parakeet's GPU runtime in Downloads.");

    CHECK(h.count("start_warmup") == 0);
    CHECK(h.contains(Json::array({"missing_runtime", "parakeet-v3"})));
    CHECK_FALSE(h.machine.in_flight());
}

// from tests/test_engine_warmup.py::test_engine_switched_during_load_is_released_not_warmed
TEST_CASE("engine switched during load is released not warmed", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.on_ui([&] { h.machine.select("api", ""); });
    h.load_ready();

    CHECK(h.count("start_warmup") == 0);
    CHECK(h.count("discard_load") == 1);
    CHECK_FALSE(h.contains(kReadyInfo));
    CHECK(h.contains(Json::array({"busy_changed", false})));
    CHECK(h.count("missing_runtime") == 0);
}

// from
// tests/test_engine_warmup.py::test_meeting_lease_during_warmup_ends_it_and_finishes_the_reload
TEST_CASE("meeting lease during warmup ends it and finishes the reload", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    REQUIRE(h.count("start_warmup") == 1);
    // The lease closes the engine without waiting for the warm-up, which then ends.
    h.on_ui([&] { h.machine.on_engine_closed(); });
    h.on_ui([&] { h.machine.on_warmup_finished(); });

    CHECK(h.contains(Json::array({"busy_changed", false})));
    CHECK_FALSE(h.machine.in_flight());
}

// from tests/test_engine_warmup.py::test_lease_returned_during_warmup_reloads_the_engine_it_closed
TEST_CASE("lease returned during warmup reloads the engine it closed", "[test_engine_warmup]") {
    // A meeting that fails fast: release and restore both land while the reload is still
    // warming, so its finish has to load the engine again.
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    REQUIRE(h.count("start_warmup") == 1);
    bool released = false;
    h.on_ui([&] { released = h.machine.release_for_lease(); });
    CHECK(released);
    h.on_ui([&] { h.machine.restore_after_lease(); });
    const auto before_finish = h.trace.size();
    h.on_ui([&] { h.machine.on_warmup_finished(); });

    CHECK(h.count("start_load") == 2);
    CHECK(h.machine.in_flight());
    CHECK(h.count("missing_runtime") == 0);
    const Json tail(h.trace.begin() + static_cast<std::ptrdiff_t>(before_finish), h.trace.end());
    CHECK(tail == Json::parse(R"([["busy_changed", true], ["status", "Reloading speech engine..."],
                                  ["start_load"]])"));
}

// from tests/test_engine_warmup.py::test_lease_still_held_after_warmup_asks_for_no_runtime
TEST_CASE("lease still held after warmup asks for no runtime", "[test_engine_warmup]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    bool released = false;
    h.on_ui([&] { released = h.machine.release_for_lease(); }); // the meeting keeps it
    CHECK(released);
    h.on_ui([&] { h.machine.on_warmup_finished(); });

    CHECK(h.count("missing_runtime") == 0);
    CHECK(h.count("start_load") == 1);
    CHECK(h.contains(Json::array({"busy_changed", false})));
}

TEST_CASE("a request during a recording waits for it, then reloads, warms and sets up the preview",
          "[engine_reload]") {
    ReloadHarness h;
    // The preview is waiting for the engine, as it does while the engine reloads.
    h.on_ui([&] { h.machine.defer_streaming_setup(); });
    h.on_ui([&] { h.machine.request(); });
    CHECK(h.trace == Json::parse(R"([["timer", 400]])"));

    ActivitySnapshot recording;
    recording.recording = true;
    h.set_activity(recording);
    h.fire_timer();
    h.fire_timer();
    // Each firing during the recording re-arms the 400 ms debounce and starts nothing.
    CHECK(h.trace == Json::parse(R"([["timer", 400], ["timer", 400], ["timer", 400]])"));
    CHECK_FALSE(h.machine.settled());

    h.set_activity(ActivitySnapshot{});
    h.trace.clear();
    h.fire_timer();
    CHECK(h.trace == Json::parse(R"([["busy_changed", true],
                                      ["status", "Reloading speech engine..."],
                                      ["start_load"]])"));
    h.trace.clear();
    h.load_ready();
    CHECK(h.trace.back() == Json::array({"start_warmup"}));
    h.trace.clear();
    h.on_ui([&] { h.machine.on_warmup_finished(); });
    CHECK(h.trace == Json::parse(R"([["busy_changed", false], ["streaming_setup_due"]])"));
    CHECK(h.machine.settled());
}

// from
// tests/test_controller_thread_hops.py::test_a_hotkey_readiness_check_really_reloads_the_engine
// (the machine's half: the controller's readiness check is M1.16's)
TEST_CASE("a request from another thread is seen at once and armed on the UI executor",
          "[engine_reload]") {
    ReloadHarness h;
    std::thread worker([&] { h.machine.request(); });
    worker.join();
    // Other threads see the reload coming before the UI executor arms it.
    CHECK_FALSE(h.machine.settled());
    CHECK(h.executor.pending_timers() == 0);

    h.executor.run_all();
    CHECK(h.executor.pending_timers() == 1);
    h.fire_timer();
    CHECK(h.count("start_load") == 1);
    CHECK(h.machine.in_flight());
}

// from
// tests/test_controller_thread_hops.py::test_a_reload_asked_for_on_the_qt_thread_is_armed_at_once
TEST_CASE("a reload asked for on the UI executor is armed at once", "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] {
        h.machine.request();
        CHECK(h.executor.pending_timers() == 1);
    });
}

TEST_CASE("the debounce fires 400 ms after the newest request", "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.request(); });
    h.executor.advance(std::chrono::milliseconds(300));
    h.on_ui([&] { h.machine.request(); });
    h.executor.advance(std::chrono::milliseconds(300));
    CHECK(h.count("start_load") == 0);
    h.executor.advance(std::chrono::milliseconds(100));
    CHECK(h.count("start_load") == 1);
    CHECK(kReloadDebounce == std::chrono::milliseconds(400));
}

TEST_CASE("shutdown stops the debounce and finishes nothing", "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.on_ui([&] { h.machine.request(); });
    h.on_ui([&] { h.machine.shutdown(); });
    h.trace.clear();
    h.fire_timer();
    CHECK(h.trace.empty());
    h.load_ready();
    // Announced, as the worker already ran; no warm-up and no finish once shutting down.
    CHECK(h.count("start_warmup") == 0);
    CHECK(h.count("busy_changed") == 0);
    CHECK(h.count("streaming_setup_due") == 0);
}

TEST_CASE("a lease taken during a load releases what it loads", "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    bool released = false;
    h.on_ui([&] { released = h.machine.release_for_lease(); });
    CHECK(released);
    CHECK(h.machine.released_for_lease());
    h.trace.clear();
    h.load_ready();
    CHECK(h.trace == Json::parse(R"([["device_info", "Parakeet TDT 0.6B v3 | cuda", true],
                                      ["status", "Parakeet TDT 0.6B v3 | cuda"],
                                      ["device_info", "Released while Meeting Mode runs", false],
                                      ["busy_changed", false], ["streaming_setup_due"]])"));
    h.trace.clear();
    h.on_ui([&] { h.machine.restore_after_lease(); });
    CHECK(h.count("start_load") == 1);
}

// OpenWhisper's release_local_engine waits for the load under the engine lock; a load that ends
// without a model leaves nothing to free, so the lease returns False and owes no restore, and
// the failed load's finish still offers the runtime (application_controller.py 564-591, 651-688).
TEST_CASE("a lease taken during a load that fails releases nothing and owes no restore",
          "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.on_ui([&] { CHECK(h.machine.release_for_lease()); });
    h.trace.clear();
    h.load_finished(ReadyState::Failed, {}, "native load failed");
    CHECK(h.trace == Json::parse(R"([["status", "Engine load failed: native load failed"],
                                      ["device_info", "native load failed", false],
                                      ["busy_changed", false],
                                      ["missing_runtime", "parakeet-v3"],
                                      ["streaming_setup_due"]])"));
    CHECK_FALSE(h.machine.released_for_lease());
    h.trace.clear();
    h.on_ui([&] { h.machine.restore_after_lease(); });
    CHECK(h.trace.empty());
}

TEST_CASE("a lease and its return during a load that finds no runtime reload nothing",
          "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.on_ui([&] { CHECK(h.machine.release_for_lease()); });
    h.on_ui([&] { h.machine.restore_after_lease(); });
    h.trace.clear();
    h.load_finished(ReadyState::RuntimeMissing, "Install Parakeet's GPU runtime in Downloads.");
    CHECK(h.count("start_load") == 0);
    CHECK(h.contains(Json::array({"missing_runtime", "parakeet-v3"})));
    CHECK(h.machine.settled());
    CHECK_FALSE(h.machine.released_for_lease());
}

TEST_CASE("a lease during a load keeps the restore an earlier lease owes", "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    h.on_ui([&] { h.machine.on_warmup_finished(); });
    h.on_ui([&] { CHECK(h.machine.release_for_lease()); });
    // The engine is reloaded while the first lease is still held (a reload already queued).
    h.on_ui([&] { h.machine.request(); });
    h.fire_timer();
    REQUIRE(h.count("start_load") == 2);
    h.on_ui([&] { CHECK(h.machine.release_for_lease()); });
    h.load_finished(ReadyState::Failed, {}, "native load failed");
    // The first lease released a loaded engine: its restore is still owed.
    CHECK(h.machine.released_for_lease());
    h.on_ui([&] { h.machine.restore_after_lease(); });
    CHECK(h.count("start_load") == 3);
}

TEST_CASE("a lease stops the live preview, which is set up again after the restore",
          "[engine_reload]") {
    ReloadHarness h;
    h.on_ui([&] { h.machine.initial_load(); });
    h.load_ready();
    h.on_ui([&] { h.machine.on_warmup_finished(); });
    h.on_ui([&] { h.machine.set_preview_active(true); });
    h.trace.clear();
    h.on_ui([&] { CHECK(h.machine.release_for_lease()); });
    CHECK(h.trace == Json::parse(R"([["stop_preview"],
                                      ["device_info", "Released while Meeting Mode runs", false]])"));
    h.on_ui([&] { h.machine.restore_after_lease(); });
    h.load_ready();
    h.trace.clear();
    h.on_ui([&] { h.machine.on_warmup_finished(); });
    CHECK(h.trace == Json::parse(R"([["busy_changed", false], ["streaming_setup_due"]])"));
}
