// tests/test_meeting_runtime.py: starting a meeting (gates, the start worker, its failure and
// the dictation engine's lease), the build of the engine's options, and
// tests/test_personalize_s9_audio_player.py::test_a_meeting_start_stops_playback.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include "meeting/runtime_messages.h"
#include "meeting_state/stored.h"
#include "runtime_support.h"
#include "settings/keys.h"

using namespace fv::meeting;
using namespace fv::meeting::runtime_test;
using Catch::Matchers::ContainsSubstring;
using fv::Json;

namespace {

bool any_contains(const std::vector<std::string>& lines, std::string_view text) {
    return std::ranges::any_of(
        lines, [text](const std::string& line) { return line.find(text) != std::string::npos; });
}

std::string lower(std::string text) {
    std::ranges::transform(text, text.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return text;
}

} // namespace

TEST_CASE("a saved paired meeting destination needs a fresh answer before any start worker",
          "[test_meeting_runtime]") {
    bool grant = false;
    int prompts = 0;
    const std::string pin(64, 'A');
    RuntimeRig rig([&](MeetingRuntimeDeps& deps) {
        deps.confirm_live_audio = [&](const Json& route) {
            ++prompts;
            CHECK(route.at("fingerprint") == pin);
            CHECK(route.at("host_name") == "Desk");
            return grant;
        };
    });
    rig.settings.update(Json{{"meeting_speech_separate", true},
                             {"meeting_asr_source", "remote"},
                             {"fv_meeting_remote_fingerprint", pin},
                             {"remote_engine_client",
                              {{"host", "127.0.0.1"},
                               {"port", 8750},
                               {"host_name", "Desk"},
                               {"device_id", "me"},
                               {"fingerprint", pin}}}});
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(prompts == 1);
    CHECK_FALSE(rig.rt->is_claimed());
    CHECK(rig.built.empty());
    grant = true;
    rig.rt->start_meeting(false);
    rig.drain();
    REQUIRE(prompts == 2);
    REQUIRE(rig.built.size() == 1);
    CHECK(rig.built.at(0).asr_remote->at("fingerprint") == pin);
    CHECK(rig.lease->calls().empty());
}

// from tests/test_meeting_runtime.py::test_off_backend_skips_speaker_model_download
TEST_CASE("test_off_backend_skips_speaker_model_download", "[test_meeting_runtime]") {
    std::vector<std::string> called;
    RuntimeRig rig([&called](MeetingRuntimeDeps& deps) {
        deps.sources.speaker_model_path = [] {
            return std::filesystem::path("/cached/model");
        };
        deps.sources.ensure_speaker_model = [&called](const fv::CancelToken&) {
            called.emplace_back("download");
            return std::optional<std::filesystem::path>("/should-not-use");
        };
    });
    rig.settings.update(Json{{"meeting_speaker_id_backend", "off"}});
    const MeetingEngineOptions options = rig.probe().build_options(false);
    CHECK(called.empty());
    CHECK_FALSE(options.diarization_model_path);
    CHECK_FALSE(options.diarization_model_resolver);
    CHECK(options.speaker_id_backend == "off");
}

// from tests/test_meeting_runtime.py::test_launch_reports_starting_without_claiming_active
TEST_CASE("test_launch_reports_starting_without_claiming_active", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_starting(true);
    probe.launch(false);
    rig.ui.run_all();
    CHECK_FALSE(probe.controller_active());
    CHECK(rig.rt->is_claimed());
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back()["status"] == "starting");
    CHECK(rig.got.states.back()["active"] == false);
    // The start worker was handed its thread, and only once.
    CHECK(std::ranges::count(rig.spawned_names(), std::string("meeting-start")) == 1);
}

// from tests/test_meeting_runtime.py::test_build_options_defers_missing_speaker_download
TEST_CASE("test_build_options_defers_missing_speaker_download", "[test_meeting_runtime]") {
    std::vector<std::string> downloads;
    RuntimeRig rig([&downloads](MeetingRuntimeDeps& deps) {
        deps.sources.speaker_model_path = [] {
            return std::optional<std::filesystem::path>();
        };
        deps.sources.ensure_speaker_model = [&downloads](const fv::CancelToken&) {
            downloads.emplace_back("download");
            return std::optional<std::filesystem::path>("/downloaded/model.onnx");
        };
    });
    rig.settings.update(Json{{"meeting_speaker_id_backend", "local"}});
    const MeetingEngineOptions options = rig.probe().build_options(false);
    CHECK(downloads.empty());
    CHECK_FALSE(options.diarization_model_path);
    // The resolver is the app's own, handed over unrun.
    REQUIRE(options.diarization_model_resolver);
    CHECK(options.diarization_model_resolver() == std::filesystem::path("/downloaded/model.onnx"));
    CHECK(downloads == std::vector<std::string>{"download"});
}

// from tests/test_meeting_runtime.py::test_completed_start_does_not_reactivate_a_cancelled_engine
TEST_CASE("test_completed_start_does_not_reactivate_a_cancelled_engine", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_engine = [](const MeetingEngineOptions&) {
        auto engine = std::make_shared<FakeEngine>();
        engine->on_start = [raw = engine.get()] {
            raw->stopped = true;
            return Json{{"host_url", "http://127.0.0.1:8765/host"}};
        };
        return engine;
    };
    auto probe = rig.probe();
    probe.set_starting(true);
    probe.start_worker(false);
    rig.drain();
    CHECK_FALSE(probe.starting());
    CHECK_FALSE(probe.controller_active());
    CHECK(std::ranges::none_of(
        rig.got.states, [](const Json& state) { return state.value("active", false) == true; }));
}

// from tests/test_meeting_runtime.py::test_start_worker_failure_rolls_back_active_state
TEST_CASE("test_start_worker_failure_rolls_back_active_state", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_engine = [](const MeetingEngineOptions&) {
        auto engine = std::make_shared<FakeEngine>();
        engine->on_start = []() -> Json {
            throw std::runtime_error("No audio devices could be opened.");
        };
        return engine;
    };
    auto probe = rig.probe();
    probe.set_starting(true);
    probe.set_card("m_partial");
    probe.set_finalization(Json{{"status", "pending"}});

    probe.start_worker(false);
    rig.drain();

    CHECK_FALSE(probe.starting());
    CHECK_FALSE(probe.controller_active());
    CHECK_FALSE(probe.engine());
    Json failed = Json::object();
    failed["active"] = false;
    failed["paused"] = false;
    failed["status"] = "failed";
    failed["dashboard_available"] = false;
    failed["finalization"] = nullptr;
    failed["meeting_id"] = nullptr;
    REQUIRE_FALSE(rig.got.states.empty());
    CHECK(rig.got.states.back() == failed);
    CHECK_FALSE(probe.card());
    CHECK_FALSE(probe.finalization());
    REQUIRE_FALSE(rig.got.errors.empty());
    CHECK_THAT(rig.got.errors.back(), ContainsSubstring("No audio devices"));
    // The dictation model was freed before start() and comes back: this start failed before any
    // meeting ASR existed, so no asr_released will ever arrive to do it.
    CHECK(rig.lease->calls() == std::vector<std::string>{"release", "restore"});
}

// from
// tests/test_meeting_runtime.py::test_start_releases_the_dictation_engine_before_loading_its_own
TEST_CASE("test_start_releases_the_dictation_engine_before_loading_its_own",
          "[test_meeting_runtime]") {
    RuntimeRig rig;
    std::mutex mutex;
    std::vector<std::string> order;
    rig.lease->on_acquire([&] {
        const std::lock_guard lock(mutex);
        order.emplace_back("release");
    });
    rig.make_engine = [&](const MeetingEngineOptions&) {
        auto engine = std::make_shared<FakeEngine>();
        engine->on_start = [&] {
            const std::lock_guard lock(mutex);
            order.emplace_back("engine.start");
            return Json{{"host_url", "http://127.0.0.1:8765/host"}};
        };
        return engine;
    };
    auto probe = rig.probe();
    probe.set_starting(true);
    probe.start_worker(false);
    rig.drain();
    CHECK(order == std::vector<std::string>{"release", "engine.start"});
    CHECK(probe.controller_active());
}

// from tests/test_meeting_runtime.py::test_nonfatal_engine_error_is_deferred_during_start
TEST_CASE("test_nonfatal_engine_error_is_deferred_during_start", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_starting(true);
    probe.on_engine_event("error",
                          Json{{"code", "asr_unavailable"}, {"message", "Model is unavailable"}});
    rig.drain();
    CHECK(rig.got.errors.empty());
    CHECK(probe.deferred_start_errors() == std::vector<std::string>{"Model is unavailable"});
}

// from tests/test_meeting_runtime.py::test_finalization_running_blocks_second_meeting_not_claim
TEST_CASE("test_finalization_running_blocks_second_meeting_not_claim", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_finalizing(true);
    probe.set_finalization(Json{{"status", "running"}, {"message", "Preparing…"}});
    probe.set_host_url("http://127.0.0.1:8765/host");

    CHECK_FALSE(rig.rt->is_claimed());
    CHECK(rig.rt->is_finalizing());

    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(any_contains(rig.got.statuses, "Final insights are still being prepared"));
    CHECK_FALSE(probe.controller_active());
    CHECK(rig.built.empty());
}

// from tests/test_meeting_runtime.py::test_start_demo_meeting_blocked_while_finalizing
TEST_CASE("test_start_demo_meeting_blocked_while_finalizing", "[test_meeting_runtime]") {
    RuntimeRig rig;
    auto probe = rig.probe();
    probe.set_finalizing(true);
    rig.rt->start_demo_meeting(true);
    rig.drain();
    CHECK(any_contains(rig.got.statuses, "Final insights are still being prepared"));
    CHECK_FALSE(probe.controller_active());
}

// from tests/test_meeting_runtime.py::test_start_meeting_blocked_while_whisper_loading
TEST_CASE("test_start_meeting_blocked_while_whisper_loading", "[test_meeting_runtime]") {
    RuntimeRig rig([](MeetingRuntimeDeps& deps) {
        deps.engine_loading_message = [] {
            return std::optional<std::string>("Whisper engine is still loading...");
        };
    });
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(rig.built.empty());
    CHECK(std::ranges::any_of(rig.got.statuses, [](const std::string& status) {
        return lower(status).find("still loading") != std::string::npos;
    }));
}

// from tests/test_meeting_runtime.py::test_begin_start_files_leftover_card
TEST_CASE("test_begin_start_files_leftover_card", "[test_meeting_runtime]") {
    RuntimeRig rig;
    rig.make_ended_meeting("m_card", false, "completed");
    auto probe = rig.probe();
    probe.set_card("m_card");
    probe.set_finalization(Json{{"status", "completed"}, {"message", "ready"}});

    rig.rt->start_meeting(false);
    rig.drain();

    CHECK(rig.saved_finalization("m_card")["card_deferred"] == true);
    REQUIRE(rig.built.size() == 1);
    CHECK_FALSE(rig.built[0].cloud_enabled);
    CHECK_FALSE(rig.built[0].demo_mode);
    CHECK(rig.built[0].system_audio_policy == "auto");
    CHECK_FALSE(probe.card());
}

// from tests/test_meeting_runtime.py::test_start_without_cloud_skips_consent
TEST_CASE("test_start_without_cloud_skips_consent", "[test_meeting_runtime]") {
    RuntimeRig rig;
    int consents = 0;
    rig.rt->set_consent_gate([&consents](bool, std::function<void(bool)>) {
        ++consents;
        return true;
    });
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(consents == 0);
    REQUIRE(rig.built.size() == 1);
    CHECK_FALSE(rig.built[0].cloud_enabled);
    CHECK_FALSE(rig.built[0].demo_mode);
    CHECK(rig.built[0].system_audio_policy == "auto");
}

// from tests/test_personalize_s9_audio_player.py::test_a_meeting_start_stops_playback
TEST_CASE("test_a_meeting_start_stops_playback", "[test_personalize_s9_audio_player]") {
    RuntimeRig rig;
    rig.probe().launch(false);
    rig.ui.run_all();
    CHECK(rig.got.playback_stops == std::vector<std::string>{"meeting"});
}

TEST_CASE("a start saves the remembered AI insights choice and stops behind a recording",
          "[meeting][runtime]") {
    RuntimeRig rig;
    rig.activity.set_recording(true);
    rig.rt->start_meeting(true);
    rig.drain();
    CHECK(rig.built.empty());
    CHECK(rig.got.main_statuses ==
          std::vector<std::string>{std::string(messages::kFinishRecording)});
    CHECK_FALSE(rig.rt->is_claimed());

    rig.activity.set_recording(false);
    rig.rt->start_meeting(true);
    rig.drain();
    REQUIRE(rig.built.size() == 1);
    CHECK(fv::keys::meeting_cloud_last_enabled.read(*rig.settings.snapshot()));

    // A remembered choice decides a start that names none.
    rig.restart();
    rig.settings.update(Json{{"meeting_cloud_last_enabled", true}});
    rig.rt->start_meeting();
    rig.drain();
    REQUIRE(rig.built.size() == 2);
    CHECK(rig.built[1].cloud_enabled);
}

TEST_CASE("a backup or a host update refuses a start before anything else", "[meeting][runtime]") {
    RuntimeRig rig;
    rig.activity.set_host_update_active(true);
    rig.activity.set_backup_in_progress(true);
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(rig.got.statuses == std::vector<std::string>{messages::update_preparing()});
    CHECK_THAT(rig.got.statuses.back(),
               ContainsSubstring("OpenWhisper_CPP is preparing an update"));

    rig.activity.set_host_update_active(false);
    rig.rt->start_meeting(false);
    rig.drain();
    CHECK(rig.got.statuses.back() == messages::kBackupBusy);
    CHECK(rig.built.empty());
}

TEST_CASE("a start without a dashboard address is aborted", "[meeting][runtime]") {
    RuntimeRig rig;
    rig.make_engine = [](const MeetingEngineOptions&) {
        auto engine = std::make_shared<FakeEngine>();
        engine->on_start = [] {
            return Json{{"meeting_id", "m_test"}};
        };
        return engine;
    };
    rig.rt->start_meeting(false);
    rig.drain();
    REQUIRE(rig.engines.size() == 1);
    CHECK(rig.engines[0]->abort_calls == 1);
    REQUIRE_FALSE(rig.got.errors.empty());
    CHECK(rig.got.errors.back() ==
          "Could not start the meeting: Meeting dashboard did not provide an address.");
    CHECK(rig.got.states.back()["status"] == "failed");
}

// The acceptance session: start, starting, active, end, the card, Keep for later, a restart
// that keeps the card hidden; Python's state payloads in order.
TEST_CASE("a scripted meeting publishes the runtime's states in order", "[meeting][runtime]") {
    RuntimeRig rig;
    rig.make_meeting("m_session", Json::object(), "active");
    rig.make_engine = [&rig](const MeetingEngineOptions&) {
        auto engine = std::make_shared<FakeEngine>("m_session");
        const Json row = *rig.repo->get_meeting("m_session");
        engine->set_store(
            fv::meeting_state::open_store(rig.repo, row, false, Json::object(), rig.clock));
        // End stops capture and persists the terminal status, as the engine's End does.
        engine->on_end = [raw = engine.get()] {
            raw->active = false;
            raw->store()->update_runtime_fields(Json{{"status", "ended"}});
        };
        return engine;
    };
    rig.rt->start_meeting(false);
    rig.drain();
    REQUIRE(rig.engines.size() == 1);
    auto engine = rig.engines[0];
    CHECK(rig.got.statuses ==
          std::vector<std::string>{"Starting meeting...", "Meeting in progress"});

    rig.rt->end();
    rig.drain();
    engine->emit("ended", Json{{"status", "ended"}});
    engine->emit("asr_released");
    engine->emit("background_ready");
    engine->emit("status", Json{{"status", "ended"},
                                {"finalization",
                                 Json{{"status", "disabled"},
                                      {"message", "AI insights are off for this meeting."}}}});
    rig.drain();
    rig.rt->defer_finalization_card();
    rig.drain();

    const std::vector<Json>& states = rig.got.states;
    REQUIRE(states.size() == 7);
    CHECK(states[0] == Json::parse(R"({"active":false,"paused":false,"status":"starting",
                                        "cloud_enabled":false,"finalization":null})"));
    CHECK(states[1]["active"] == true);
    CHECK(states[1]["status"] == "active");
    CHECK(states[1].contains("elapsed_s"));
    CHECK(states[1].contains("elapsed_at"));
    CHECK(states[2] == Json::parse(R"({"active":true,"paused":false,"status":"ending"})"));
    CHECK(states[3] == Json::parse(R"({"active":false,"status":"ended"})"));
    CHECK(states[4] == Json::parse(R"({"background_available":true})"));
    CHECK(states[5]["status"] == "ended");
    CHECK(states[5]["active"] == false);
    CHECK(states[5]["finalization"]["status"] == "disabled");
    CHECK(states[5]["finalization"]["content_summary"]["meeting_status"] == "ended");
    CHECK(states[6] == Json::parse(R"({"active":false,"finalization":null,"meeting_id":null})"));
    CHECK(rig.got.statuses.back() == messages::kSaved);
    CHECK(rig.lease->calls() == std::vector<std::string>{"release", "restore"});
    CHECK(rig.saved_finalization("m_session")["card_deferred"] == true);

    rig.restart();
    rig.rt->setup();
    rig.drain();
    CHECK(rig.got.states.empty());
    CHECK(rig.got.found.empty());
}

TEST_CASE("events the engine reports during End follow the runtime's ending state",
          "[meeting][runtime]") {
    // Python ends on the Qt thread, so the engine's own events, queued meanwhile, come after
    // "ending": an `ended` routed first would leave the tab locked on an ended meeting.
    RuntimeRig rig;
    rig.rt->start_meeting(false);
    rig.drain();
    REQUIRE(rig.engines.size() == 1);
    auto engine = rig.engines[0];
    engine->on_end = [raw = engine.get()] {
        raw->active = false;
        raw->emit("status", Json{{"status", "ending"}});
        raw->emit("ended", Json{{"status", "ended"}});
    };
    const std::size_t before = rig.got.states.size();
    rig.rt->end();
    rig.drain();
    REQUIRE(rig.got.states.size() == before + 3);
    CHECK(rig.got.states[before] ==
          Json::parse(R"({"active":true,"paused":false,"status":"ending"})"));
    CHECK(rig.got.states[before + 1] == Json::parse(R"({"status":"ending"})"));
    CHECK(rig.got.states[before + 2] == Json::parse(R"({"active":false,"status":"ended"})"));
    CHECK_FALSE(rig.probe().controller_active());
    CHECK(rig.got.statuses.back() == messages::kEnded);

    // Nothing stays held: a later event goes straight through.
    engine->emit("error", Json{{"message", "late"}});
    rig.drain();
    CHECK(rig.got.errors == std::vector<std::string>{"late"});
}

TEST_CASE("a failed pause shows its error before the engine's events", "[meeting][runtime]") {
    RuntimeRig rig;
    rig.rt->start_meeting(false);
    rig.drain();
    auto engine = rig.engines[0];
    engine->on_pause = [raw = engine.get()] {
        raw->emit("error", Json{{"message", "capture stalled"}});
        throw std::runtime_error("device lost");
    };
    const std::size_t before = rig.got.states.size();
    rig.rt->pause();
    rig.drain();
    CHECK(rig.got.errors ==
          std::vector<std::string>{"Could not pause the meeting: device lost", "capture stalled"});
    CHECK(rig.got.states.size() == before);
}

TEST_CASE("a meeting's speaker model download is cancelled when its engine goes",
          "[meeting][runtime]") {
    std::vector<fv::CancelToken> tokens;
    RuntimeRig rig([&tokens](MeetingRuntimeDeps& deps) {
        deps.sources.speaker_model_path = [] {
            return std::optional<std::filesystem::path>();
        };
        deps.sources.ensure_speaker_model = [&tokens](const fv::CancelToken& cancel) {
            tokens.push_back(cancel);
            return std::optional<std::filesystem::path>();
        };
    });
    rig.settings.update(Json{{"meeting_speaker_id_backend", "local"}});
    const auto resolve = [&rig](std::size_t index) {
        REQUIRE(rig.built.size() > index);
        REQUIRE(rig.built[index].diarization_model_resolver);
        rig.built[index].diarization_model_resolver();
    };

    SECTION("at cleanup") {
        rig.rt->start_meeting(false);
        rig.drain();
        resolve(0);
        REQUIRE(tokens.size() == 1);
        CHECK_FALSE(tokens[0].cancelled());
        rig.rt->cleanup();
        CHECK(tokens[0].cancelled());
        CHECK(rig.engines[0]->shutdown_calls == 1);
    }
    SECTION("when the next meeting starts") {
        rig.rt->start_meeting(false);
        rig.drain();
        rig.engines[0]->active = false;
        rig.engines[0]->emit("ended", Json{{"status", "ended"}});
        rig.drain();
        rig.rt->start_meeting(false);
        rig.drain();
        REQUIRE(rig.engines.size() == 2);
        resolve(0);
        resolve(1);
        REQUIRE(tokens.size() == 2);
        CHECK(tokens[0].cancelled());
        CHECK_FALSE(tokens[1].cancelled());
    }
    SECTION("when the start fails") {
        rig.make_engine = [](const MeetingEngineOptions&) {
            auto engine = std::make_shared<FakeEngine>();
            engine->on_start = []() -> Json {
                throw std::runtime_error("no devices");
            };
            return engine;
        };
        rig.rt->start_meeting(false);
        rig.drain();
        resolve(0);
        REQUIRE(tokens.size() == 1);
        CHECK(tokens[0].cancelled());
    }
}
