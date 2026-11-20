// The meeting runtime's status lines and errors no other test pinned (runtime audit A04-80,
// A04-89, A04-96, A04-97, A04-100, A04-101, A04-102, A04-106) and the meeting functions of
// tests/test_application_controller.py, each against services/runtime/meeting.py's text.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <future>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/log.h"
#include "meeting/runtime_messages.h"
#include "runtime_support.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using fv::Json;

namespace {

bool starts_with_any(const std::vector<std::string>& lines, std::string_view prefix) {
    return std::ranges::any_of(
        lines, [prefix](const std::string& line) { return line.starts_with(prefix); });
}

bool has(const std::vector<std::string>& lines, std::string_view line) {
    return std::ranges::find(lines, std::string(line)) != lines.end();
}

// A running meeting on a FakeEngine.
std::shared_ptr<FakeEngine> start(RuntimeRig& rig) {
    rig.rt->start_meeting(false);
    rig.drain();
    REQUIRE_FALSE(rig.engines.empty());
    return rig.engines.back();
}

// Every warning of the runtime's logger while it lives.
class Warnings final : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        if (record.logger == "services.runtime.meeting") {
            const std::lock_guard lock(mutex_);
            lines_.emplace_back(record.message);
        }
    }
    std::vector<std::string> lines() const {
        const std::lock_guard lock(mutex_);
        return lines_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::string> lines_;
};

} // namespace

TEST_CASE("pause, resume and end say what they did or why they could not",
          "[test_meeting_runtime]") {
    // A04-80: "Meeting paused", "Meeting in progress", "Ending meeting — finishing
    // transcription...", "Could not resume the meeting: {e}", "Could not end the meeting: {e}".
    RuntimeRig rig;
    auto engine = start(rig);
    rig.got = Recorded{};
    rig.rt->pause();
    rig.drain();
    CHECK(rig.got.statuses == std::vector<std::string>{"Meeting paused"});
    rig.rt->resume();
    rig.drain();
    CHECK(rig.got.statuses == std::vector<std::string>{"Meeting paused", "Meeting in progress"});

    engine->on_resume = [] {
        throw std::runtime_error("device lost");
    };
    rig.got = Recorded{};
    rig.rt->resume();
    rig.drain();
    CHECK(rig.got.errors == std::vector<std::string>{"Could not resume the meeting: device lost"});
    CHECK(rig.got.statuses.empty());

    engine->on_end = [] {
        throw std::runtime_error("drain stuck");
    };
    rig.rt->end();
    rig.drain();
    CHECK(rig.got.errors.back() == "Could not end the meeting: drain stuck");
    engine->on_end = {};
    rig.got = Recorded{};
    rig.rt->end();
    rig.drain();
    CHECK(rig.got.statuses ==
          std::vector<std::string>{"Ending meeting \xE2\x80\x94 finishing transcription..."});
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back()["status"] == "ending");
}

TEST_CASE("the meeting hotkey ends a meeting or asks for the readiness gate",
          "[test_meeting_runtime]") {
    // A04-96: toggle_from_hotkey ends an active meeting; otherwise it asks the tab for the
    // platform acknowledgement once and starts nothing itself (the answer calls start).
    RuntimeRig rig;
    rig.rt->toggle_from_hotkey();
    rig.drain();
    CHECK(rig.got.platform_acks == 1);
    CHECK(rig.built.empty());

    auto engine = start(rig);
    rig.rt->toggle_from_hotkey();
    rig.drain();
    CHECK(engine->end_calls.load() == 1);
    CHECK(rig.got.platform_acks == 1);
}

TEST_CASE("a demo meeting is built as OpenWhisper's demo and says so", "[test_meeting_runtime]") {
    // A04-97: title "Demo Planning Sync", local speakers, no audio-upload consent, no end
    // redecode, and "Demo meeting loaded — End to test cleanup and the report".
    RuntimeRig rig;
    rig.settings.update(Json{{"meeting_speaker_id_backend", "openai"},
                             {"meeting_audio_upload_consent_given", true},
                             {"meeting_end_redecode", true}});
    const auto demo = rig.probe().build_options(false, true);
    CHECK(demo.title == "Demo Planning Sync");
    CHECK(demo.speaker_id_backend == "local");
    CHECK_FALSE(demo.speaker_id_audio_consent);
    CHECK_FALSE(demo.end_redecode);
    CHECK(demo.demo_mode);
    const auto normal = rig.probe().build_options(false, false);
    CHECK(normal.title.empty());
    CHECK(normal.end_redecode);

    rig.rt->start_demo_meeting(false);
    rig.drain();
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built[0].demo_mode);
    CHECK(has(rig.got.statuses,
              "Demo meeting loaded \xE2\x80\x94 End to test cleanup and the report"));
}

TEST_CASE("intelligence events show online and offline", "[test_meeting_runtime]") {
    // A04-101.
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.on_engine_event("intelligence", Json{{"online", true}});
    probe.on_engine_event("intelligence", Json{{"online", false}});
    rig.drain();
    CHECK(rig.got.statuses ==
          std::vector<std::string>{"Meeting intelligence online",
                                   "Meeting intelligence offline \xE2\x80\x94 transcript-only"});
}

TEST_CASE("open report with nothing selected says what to do", "[test_meeting_runtime]") {
    // A04-106.
    RuntimeRig rig;
    rig.rt->open_report();
    rig.drain();
    CHECK(rig.got.errors ==
          std::vector<std::string>{
              "No meeting report is available. Select a meeting from Past Meetings."});
    CHECK(rig.opened_urls().empty());
}

TEST_CASE("keep for later refuses while a meeting runs and says when it could not save",
          "[test_meeting_runtime]") {
    // A04-102.
    RuntimeRig rig;
    rig.make_ended_meeting("m_card", false, "completed");
    SECTION("a meeting in progress") {
        start(rig);
        rig.probe().set_card("m_card");
        rig.got = Recorded{};
        rig.rt->defer_finalization_card();
        rig.drain();
        CHECK(rig.got.statuses == std::vector<std::string>{"A meeting is already in progress"});
        CHECK(rig.saved_finalization("m_card")["card_deferred"] == false);
    }
    SECTION("a meeting that cannot be saved") {
        rig.probe().set_card("m_card");
        rig.probe().set_finalization(Json{{"status", "completed"}});
        rig.database->close();
        rig.rt->defer_finalization_card();
        rig.drain();
        CHECK(rig.got.errors == std::vector<std::string>{"Could not save this meeting for later"});
    }
}

TEST_CASE("past meeting actions name what failed", "[test_meeting_runtime]") {
    // A04-100: the "Could not …" texts of services/runtime/meeting.py with the error, and the
    // two plain ones.
    RuntimeRig rig;
    rig.make_ended_meeting("m_old");
    SECTION("a meeting that no longer exists") {
        rig.rt->show_past_meeting("m_gone");
        rig.drain();
        CHECK(rig.got.errors == std::vector<std::string>{"That meeting no longer exists"});
    }
    SECTION("the live dashboard is unavailable") {
        auto engine = start(rig);
        rig.make_meeting(engine->meeting_id(), Json::object(), "active");
        rig.probe().set_host_url("");
        rig.got = Recorded{};
        rig.rt->open_past_meeting(engine->meeting_id());
        rig.drain();
        CHECK(has(rig.got.errors, "The live meeting dashboard is unavailable"));
    }
    SECTION("a repository that fails") {
        rig.database->close();
        rig.rt->show_past_meeting("m_old");
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not load the past meeting: "));
        rig.rt->open_past_meeting("m_old");
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not open the past meeting: "));
        CHECK(rig.rt->copy_past_meeting_transcript("m_old").empty());
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not copy the transcript: "));
        rig.rt->delete_past_meeting("m_old");
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not delete the meeting: "));
        rig.rt->clear_past_meetings(false);
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not clear past meetings: "));
        rig.rt->discard_recovered("m_old");
        rig.drain();
        CHECK(starts_with_any(rig.got.errors, "Could not discard the meeting: "));
    }
}

// from tests/test_application_controller.py::TestApplicationController::
// test_meeting_cleanup_gives_up_on_a_hanging_engine_shutdown (and audit A04-89)
TEST_CASE("test_meeting_cleanup_gives_up_on_a_hanging_engine_shutdown", "[test_meeting_runtime]") {
    // The budget is kShutdownJoinTimeout (5 s, SHUTDOWN_JOIN_TIMEOUT_S); the test waits it out.
    auto warnings = std::make_shared<Warnings>();
    fv::log::add_sink(warnings);
    std::promise<void> release;
    auto released = release.get_future().share();
    {
        RuntimeRig rig;
        auto engine = start(rig);
        engine->on_shutdown = [released] {
            released.wait_for(std::chrono::seconds(30));
        };
        rig.probe().set_controller_active(true);
        const auto started = std::chrono::steady_clock::now();
        rig.rt->cleanup();
        const auto elapsed = std::chrono::steady_clock::now() - started;
        CHECK(elapsed >= kShutdownJoinTimeout);
        CHECK(elapsed < kShutdownJoinTimeout + std::chrono::seconds(3));
        CHECK_FALSE(rig.probe().controller_active());
        CHECK_FALSE(rig.rt->is_active());
        CHECK(engine->shutdown_calls.load() == 1);
        release.set_value();
    }
    fv::log::remove_sink(warnings);
    CHECK(std::ranges::any_of(warnings->lines(), [](const std::string& line) {
        return line.starts_with("Meeting engine shutdown still running after 5.0s; abandoning it "
                                "so the application can exit.");
    }));
}

// from tests/test_application_controller.py::TestApplicationController::
// test_ended_event_does_not_restore_the_engine
TEST_CASE("test_ended_event_does_not_restore_the_engine", "[test_meeting_runtime]") {
    // "ended" fires before the offline final pass, which still decodes on the meeting's model:
    // only "asr_released" gives dictation its model back.
    RuntimeRig rig;
    auto engine = start(rig);
    // CountingLease records the dictation engine's "release" and its "restore".
    REQUIRE(std::ranges::count(rig.lease->calls(), std::string("release")) == 1);
    engine->emit("ended", Json{{"meeting_id", engine->meeting_id()}, {"status", "ended"}});
    rig.drain();
    CHECK(std::ranges::count(rig.lease->calls(), std::string("restore")) == 0);
    engine->emit("asr_released", Json{{"meeting_id", engine->meeting_id()}});
    rig.drain();
    CHECK(std::ranges::count(rig.lease->calls(), std::string("restore")) == 1);
}

// from tests/test_application_controller.py::TestApplicationController::
// test_second_meeting_start_is_refused_while_one_is_running
TEST_CASE("test_second_meeting_start_is_refused_while_one_is_running", "[test_meeting_runtime]") {
    // Exclusive mode, not engine state, guards against a second engine.
    RuntimeRig rig;
    rig.probe().set_controller_active(true);
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(rig.built.empty());
    CHECK(has(rig.got.statuses, "A meeting is already in progress"));
}
