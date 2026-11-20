// The engine's side of the Fast judgments (meeting/engine.py _start_fast_features,
// _finalize_highlights, the chunk feed, _voice_commands_allowed, _apply_spoken_action) from
// tests/test_live_signals_lifecycle.py, tests/test_text_judge.py and
// tests/test_meeting_fast_features.py. FioriVoce asks only a judge on this computer
// (EngineDeps::local_fast_judge, looked up per judgment), so a hosted judge stands for
// OpenWhisper's Jev and hosted text-model routes and is never asked, AI insights on or off.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "engine_support.h"
#include "llm/testing/fake_judge.h"
#include "meeting/testing/fake_intelligence.h"
#include "meeting_agent/testing/fake_agent_core.h"
#include "meeting_state/schema.h"

using fv::Json;
using fv::llm::FakeJudge;
using fv::meeting::MeetingEngine;
using fv::meeting::MeetingEngineProbe;
using fv::meeting::test::EngineRig;
using fv::meeting::test::EngineSetup;
using fv::meeting::test::eventually;
using fv::meeting::test::mic_segment;

namespace {

using std::chrono::milliseconds;
using std::chrono::seconds;

std::string first_passage(const Json& state) {
    return state.at("passages").begin().key();
}

// A judge that finds a number in the first passage it is shown.
std::shared_ptr<FakeJudge> number_judge(bool local = true) {
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(local);
    judge->set_handler([](const Json& asked, const Json& questions) -> std::optional<Json> {
        if (!asked.contains("passages") || !questions.contains("number")) {
            return std::nullopt;
        }
        return Json{{"number", {{"noul", 0.95}}},
                    {"number_anchor", {{"choice", first_passage(asked)}}}};
    });
    return judge;
}

// The judge the composition hands out now: switched from the test thread mid-meeting, as a
// Settings change switches OpenWhisper's FollowingJudge.
struct JudgeSwitch {
    std::shared_ptr<fv::llm::Judge> get() {
        ++lookups;
        std::lock_guard lock(mutex);
        return judge;
    }
    void set(std::shared_ptr<fv::llm::Judge> next) {
        std::lock_guard lock(mutex);
        judge = std::move(next);
    }
    std::mutex mutex;
    std::shared_ptr<fv::llm::Judge> judge;
    std::atomic<int> lookups{0};
};

EngineSetup fast_setup(bool cloud_enabled, JudgeSwitch& judges, Json settings) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = cloud_enabled;
    settings["typesafe_enabled"] = true;
    setup.settings = [settings] {
        return settings;
    };
    setup.local_fast_judge = [&judges] {
        return judges.get();
    };
    return setup;
}

const Json kHighlights{{"typesafe_highlights_enabled", true}};
const Json kVoice{{"typesafe_voice_commands_enabled", true}};

// One committed microphone chunk from `start` for `duration` seconds holding `segments`.
void commit(EngineRig& rig, MeetingEngine& engine, double start, double duration,
            std::vector<fv::meeting_state::TranscriptSegment> segments) {
    const auto id = engine.meeting_id();
    const auto chunk =
        rig.register_chunk(id, rig.sql.repo->next_chunk_seq(id, "mic"), start, "mic", duration);
    for (auto& segment : segments) {
        segment.chunk_id = chunk.chunk_id;
    }
    MeetingEngineProbe(engine).on_chunk_result(chunk, std::move(segments));
}

std::vector<std::string> feedback_messages(EngineRig& rig) {
    std::vector<std::string> out;
    for (const auto& message : rig.server(0)->messages_of("voice_feedback")) {
        out.push_back(message.value("message", std::string()));
    }
    return out;
}

} // namespace

TEST_CASE("engine final pulses do not require the notes agent", "[fast_features]") {
    // from
    // tests/test_live_signals_lifecycle.py::test_engine_final_pulses_do_not_require_the_notes_agent
    JudgeSwitch judges;
    auto judge = number_judge();
    judges.set(judge);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(false, judges, kHighlights)); // no intelligence
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    rig.sql.repo->add_segments(
        {mic_segment(engine.meeting_id(), "sg_final", 8, 12, "The budget is 1000 dollars.")});

    EngineRig::end_and_wait(engine);

    const auto pulses = engine.store()->snapshot().at("live_highlights");
    REQUIRE(pulses.size() == 1);
    CHECK(pulses[0].at("segment_id") == "sg_final");
    CHECK(judge->calls().size() == 1);
}

TEST_CASE("empty committed audio advances pulse frontier", "[fast_features]") {
    // from tests/test_live_signals_lifecycle.py::test_empty_committed_audio_advances_pulse_frontier
    // OpenWhisper checks observe([], frontier=61); here the effect: the completed first minute
    // is judged although the chunk that completed it held no speech.
    JudgeSwitch judges;
    auto judge = number_judge();
    judges.set(judge);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(false, judges, kHighlights));
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    commit(rig, engine, 0, 10,
           {mic_segment(engine.meeting_id(), "sg_first", 8, 9, "The budget is 1000 dollars.")});
    CHECK(judge->calls().empty());
    commit(rig, engine, 55, 6, {});
    REQUIRE(eventually([&] { return !engine.store()->snapshot().at("live_highlights").empty(); }));
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(first_passage(calls[0].state) == "sg_first");
}

TEST_CASE("fast-feature gate matrix", "[fast_features]") {
    // from tests/test_text_judge.py::test_gate_matrix. A model on this computer runs with or
    // without AI insights ("on"); FioriVoce never sends meeting text to a hosted Fast judgments
    // model, so where OpenWhisper says "needs_insights" or, with insights on, "on", the hosted
    // model is "unavailable" and is never asked.
    const bool local = GENERATE(true, false);
    const bool consent = GENERATE(false, true);
    CAPTURE(local, consent);
    JudgeSwitch judges;
    auto judge = number_judge(local);
    judges.set(judge);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(consent, judges, kHighlights));
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK(engine.store()->snapshot().at("live_highlights_status") ==
          (local ? "on" : "unavailable"));
    commit(rig, engine, 0, 61,
           {mic_segment(engine.meeting_id(), "sg_first", 8, 9, "The budget is 1000 dollars.")});
    if (local) {
        REQUIRE(eventually([&] { return judge->calls().size() == 1; }));
    } else {
        REQUIRE(eventually([&] { return judges.lookups.load() >= 2; }));
        std::this_thread::sleep_for(milliseconds(50));
        CHECK(judge->calls().empty());
        CHECK(engine.store()->snapshot().at("live_highlights").empty());
    }
}

TEST_CASE("local workers start without ai insights and stop sending after a switch",
          "[fast_features]") {
    // from
    // tests/test_text_judge.py::test_local_workers_start_without_ai_insights_and_stop_sending_after_a_switch
    JudgeSwitch judges;
    auto local = number_judge(true);
    auto hosted = number_judge(false);
    judges.set(local);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(false, judges, kHighlights));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    REQUIRE(probe.fast_features_running());
    commit(rig, engine, 0, 61,
           {mic_segment(engine.meeting_id(), "sg_first", 8, 9, "The budget is 1000 dollars.")});
    REQUIRE(eventually([&] { return local->calls().size() == 1; }));
    // Switching to a hosted model mid-meeting must not carry text off this computer.
    judges.set(hosted);
    const int before = judges.lookups.load();
    commit(rig, engine, 61, 60,
           {mic_segment(engine.meeting_id(), "sg_second", 70, 71, "We spent 20 dollars.")});
    REQUIRE(eventually([&] { return judges.lookups.load() > before; }));
    std::this_thread::sleep_for(milliseconds(50));
    CHECK(hosted->calls().empty());
    CHECK(local->calls().size() == 1);
}

TEST_CASE("local voice commands apply without ai insights", "[fast_features]") {
    // from tests/test_text_judge.py::test_local_voice_commands_apply_without_ai_insights
    JudgeSwitch judges;
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("set_topic", 0.99));
    });
    auto hosted = std::make_shared<FakeJudge>();
    judges.set(local);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(false, judges, kVoice));
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    commit(rig, engine, 0, 10,
           {mic_segment(engine.meeting_id(), "sg_topic", 2, 4, "Note taker, new topic: budget.")});
    REQUIRE(eventually([&] {
        return engine.store()->snapshot().at("topic").value("current", Json()) == "budget";
    }));
    REQUIRE(eventually([&] { return !rig.events_of("voice_command").empty(); }));
    CHECK(rig.events_of("voice_command")[0] ==
          Json{{"command", "set_topic"}, {"applied", 1}, {"rejected", 0}});
    CHECK(eventually([&] {
        const auto messages = feedback_messages(rig);
        return !messages.empty() && messages.back() == "Topic updated";
    }));
    judges.set(hosted);
    commit(rig, engine, 10, 10,
           {mic_segment(engine.meeting_id(), "sg_next", 12, 14, "Note taker, new topic: hiring.")});
    REQUIRE(eventually([&] {
        for (const auto& message : rig.server(0)->messages_of("voice_feedback")) {
            if (message.value("phase", std::string()) == "unavailable") {
                return true;
            }
        }
        return false;
    }));
    CHECK(hosted->calls().empty());
    CHECK(engine.store()->snapshot().at("topic").at("current") == "budget");
}

TEST_CASE("recap routes to note agent and reports completion", "[fast_features]") {
    // from
    // tests/test_meeting_fast_features.py::test_recap_routes_to_note_agent_and_reports_completion
    // The spoken recap arrives through the voice listener; the note agent is a FakeAgentCore.
    class RequestProvider final : public fv::meeting::IntelligenceProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    JudgeSwitch judges;
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("recap", 0.99));
    });
    judges.set(local);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    auto setup = fast_setup(true, judges, kVoice);
    auto provider = std::make_shared<RequestProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    provider->core->set_hook([](const auto&, auto*) {
        fv::meeting_agent::AgentResult answer;
        answer.ok = true;
        answer.op_results = {fv::meeting_state::OpResult{.ok = true}};
        return answer;
    });
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    commit(
        rig, engine, 0, 10,
        {mic_segment(engine.meeting_id(), "sg_recap", 2, 4, "Note taker, recap the decisions.")});
    REQUIRE(eventually([&] {
        const auto feedback = engine.store()->snapshot().value("voice_feedback", Json());
        return feedback.is_object() &&
               feedback.value("message", std::string()).find("added") != std::string::npos;
    }));
    const auto passes = provider->core->passes();
    REQUIRE_FALSE(passes.empty());
    CHECK(passes.back()
              .payload.state_snapshot.at("note_adjustment_request")
              .get<std::string>()
              .find("recap the decisions") != std::string::npos);
    const auto messages = feedback_messages(rig);
    CHECK(std::ranges::find(messages, "Preparing a recap…") != messages.end());
    CHECK(messages.back() == "Recap added to the live notes.");
    // engine._voice_feedback stores the voice_feedback op's {message, at}.
    const auto stored = engine.store()->snapshot().at("voice_feedback");
    CHECK(stored.at("message") == "Recap added to the live notes.");
    CHECK_FALSE(stored.at("at").get<std::string>().empty());
}

TEST_CASE("a spoken recap says it is preparing only once the note agent took it",
          "[fast_features]") {
    // engine.py _apply_spoken_action (audit A24-53): the engine's "Preparing a recap…" follows an
    // accepted request; for an offline agent it says only that it is offline; a pass that fails
    // and one that saves nothing both read "The recap produced no saved notes…", as Python's
    // note request completes with the failed AgentResult rather than raising.
    class RequestProvider final : public fv::meeting::IntelligenceProvider {
    public:
        explicit RequestProvider(bool online) : online_(online) {}
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            if (!online_) {
                throw std::runtime_error("The meeting agent is not available.");
            }
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }

    private:
        bool online_;
    };
    const std::string outcome = GENERATE("offline", "failed", "empty");
    JudgeSwitch judges;
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("recap", 0.99));
    });
    judges.set(local);
    EngineRig rig;
    auto setup = fast_setup(true, judges, kVoice);
    auto provider = std::make_shared<RequestProvider>(outcome != "offline");
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    provider->core->set_hook([outcome](const auto&, auto*) {
        fv::meeting_agent::AgentResult answer;
        answer.ok = outcome != "failed";
        return answer;
    });
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    commit(
        rig, engine, 0, 10,
        {mic_segment(engine.meeting_id(), "sg_recap", 2, 4, "Note taker, recap the decisions.")});
    // The sockets' last line, not the store's: the engine saves the feedback before it
    // broadcasts it, so the store can be ahead of the socket for a moment.
    REQUIRE(eventually([&] {
        const auto said = feedback_messages(rig);
        const auto message = said.empty() ? std::string() : said.back();
        return message.starts_with("Recap is unavailable") || message.starts_with("The recap");
    }));
    // The listener says "Preparing a recap…" once it recognized the command (voice_commands.py
    // _WORKING_MESSAGES); the engine says it again only once the note agent took the request.
    const auto messages = feedback_messages(rig);
    const auto prepared = std::ranges::count(messages, std::string("Preparing a recap…"));
    if (outcome == "offline") {
        CHECK(prepared == 1);
        CHECK(messages.back() == "Recap is unavailable while the note agent is offline.");
    } else {
        CHECK(prepared == 2);
        CHECK(messages.back() ==
              "The recap produced no saved notes. Try again from the notes request box.");
    }
}

TEST_CASE("voice feedback reaches every socket numbered and the listeners", "[fast_features]") {
    // engine.py _on_voice_feedback (audit A30-36): every socket, guests included, gets
    // {type, phase, message, command, seq} with an increasing seq; the listeners get a
    // voice_feedback event; nothing is said while the meeting is paused.
    JudgeSwitch judges;
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("set_topic", 0.99));
    });
    judges.set(local);
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(fast_setup(false, judges, kVoice));
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    commit(rig, engine, 0, 10,
           {mic_segment(engine.meeting_id(), "sg_topic", 2, 4, "Note taker, new topic: budget.")});
    REQUIRE(eventually([&] {
        const auto messages = feedback_messages(rig);
        return !messages.empty() && messages.back() == "Topic updated";
    }));
    std::int64_t last = 0;
    std::vector<std::string> phases;
    for (const auto& [message, host_only] : rig.server(0)->sent_host_only()) {
        if (message.value("type", std::string()) != "voice_feedback") {
            continue;
        }
        CHECK_FALSE(host_only);
        CHECK(message.contains("phase"));
        CHECK(message.contains("message"));
        CHECK(message.contains("command"));
        REQUIRE(message.contains("seq"));
        CHECK(message.at("seq").get<std::int64_t>() > last);
        last = message.at("seq").get<std::int64_t>();
        phases.push_back(message.at("phase").get<std::string>());
    }
    CHECK((phases == std::vector<std::string>{"heard", "working", "saved"}));
    const auto events = rig.events_of("voice_feedback");
    REQUIRE(events.size() == 3);
    CHECK(events.back().at("seq") == last);
    CHECK_FALSE(events.back().contains("type"));

    engine.pause();
    const auto before = rig.server(0)->messages_of("voice_feedback").size();
    commit(rig, engine, 10, 10,
           {mic_segment(engine.meeting_id(), "sg_next", 12, 14, "Note taker, new topic: hiring.")});
    std::this_thread::sleep_for(milliseconds(200));
    CHECK(rig.server(0)->messages_of("voice_feedback").size() == before);
}

TEST_CASE("a host hello carries the last 50 agent ticks", "[fast_features]") {
    // engine.py AGENT_ACTIVITY_HISTORY = 50 (audit A30-36, A30-29).
    class LiveProvider final : public fv::meeting::IntelligenceProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        bool available() const override { return true; }
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    EngineRig rig;
    EngineSetup setup{.options = EngineRig::default_options()};
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    engine.start();
    REQUIRE(engine.store()->snapshot().at("intelligence_online") == true);
    // A distinct tool per tick: the same (kind, tool) is published once a second at most.
    for (int i = 0; i < 60; ++i) {
        provider->core->emit_activity(
            {"thinking", "Tick " + std::to_string(i), "tool_" + std::to_string(i), "cards", ""});
    }
    const auto ticks = engine.recent_agent_activity();
    REQUIRE(ticks.size() == 50);
    CHECK(ticks.front().at("label") == "Tick 10");
    CHECK(ticks.back().at("label") == "Tick 59");
}

// --- tests/test_typesafe_meeting_runtime.py: runtime changes reach a running meeting --------

namespace {

// settings_manager.load_all_settings, changed by the test while the meeting runs.
struct LiveSettings {
    Json get() {
        std::lock_guard lock(mutex);
        return values;
    }
    void set(const std::string& key, Json value) {
        std::lock_guard lock(mutex);
        values[key] = std::move(value);
    }
    std::mutex mutex;
    Json values = Json{{"typesafe_enabled", false}};
};

EngineSetup live_setup(LiveSettings& settings, JudgeSwitch& judges) {
    EngineSetup setup{.options = EngineRig::default_options()};
    setup.options.cloud_enabled = false;
    setup.settings = [&settings] {
        return settings.get();
    };
    setup.local_fast_judge = [&judges] {
        return judges.get();
    };
    return setup;
}

std::shared_ptr<FakeJudge> topic_judge() {
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("set_topic", 0.99));
    });
    return judge;
}

std::vector<std::string> statuses(EngineRig& rig) {
    std::vector<std::string> out;
    for (const auto& message : rig.server(0)->messages_of("status")) {
        out.push_back(message.value("live_highlights_status", std::string()));
    }
    return out;
}

} // namespace

TEST_CASE("voice listener can start after first chunk and reenable", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_voice_listener_can_start_after_first_chunk_and_reenable
    LiveSettings settings;
    JudgeSwitch judges;
    judges.set(topic_judge());
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK(probe.voice_listener() == nullptr);
    settings.set("typesafe_enabled", true);
    probe.start_fast_features();
    CHECK(probe.voice_listener() == nullptr);
    settings.set("typesafe_voice_commands_enabled", true);
    commit(rig, engine, 0, 10,
           {mic_segment(engine.meeting_id(), "sg_1", 2, 4, "Note taker, new topic: budget.")});
    const void* listener = probe.voice_listener();
    REQUIRE(listener != nullptr);
    REQUIRE(eventually([&] {
        return engine.store()->snapshot().at("topic").value("current", Json()) == "budget";
    }));
    settings.set("typesafe_voice_commands_enabled", false);
    commit(rig, engine, 10, 10,
           {mic_segment(engine.meeting_id(), "sg_2", 12, 14, "Note taker, new topic: hiring.")});
    settings.set("typesafe_voice_commands_enabled", true);
    probe.start_fast_features();
    CHECK(probe.voice_listener() == listener);
    EngineRig::end_and_wait(engine);
    CHECK(probe.voice_listener() == nullptr);
    probe.start_fast_features();
    CHECK(probe.voice_listener() == nullptr);
    CHECK(engine.store()->snapshot().at("topic").at("current") == "budget");
}

TEST_CASE("existing unkeyed voice listener uses key added later", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_existing_unkeyed_voice_listener_uses_key_added_later
    // FioriVoce's missing key is a missing device-local judge.
    const bool preview = GENERATE(false, true);
    CAPTURE(preview);
    LiveSettings settings;
    settings.set("typesafe_enabled", true);
    settings.set("typesafe_voice_commands_enabled", true);
    JudgeSwitch judges;
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    REQUIRE(probe.voice_listener() != nullptr);
    const auto phase_said = [&](const std::string& phase, std::size_t n) {
        return eventually([&] {
            std::size_t seen = 0;
            for (const auto& message : rig.server(0)->messages_of("voice_feedback")) {
                seen += message.value("phase", std::string()) == phase ? 1 : 0;
            }
            return seen >= n;
        });
    };
    const auto speak = [&](int index) {
        const double start = index * 10;
        const std::string text = "Note taker, new topic: budget " + std::to_string(index) + ".";
        if (preview) {
            probe.on_speech_preview(Json{{"channel", "mic"},
                                         {"start_s", start},
                                         {"end_s", start + 2},
                                         {"text", text},
                                         {"final", false}});
        } else {
            commit(rig, engine, start, 10,
                   {mic_segment(engine.meeting_id(), "sg_" + std::to_string(index), start,
                                start + 2, text)});
        }
    };
    speak(1);
    REQUIRE(phase_said("unavailable", 1));
    auto judge = topic_judge();
    judges.set(judge);
    speak(2);
    REQUIRE(phase_said(preview ? "recognized" : "saved", 1));
    CHECK(judge->calls().size() == 1);
    settings.set("typesafe_voice_commands_enabled", false);
    speak(3);
    std::this_thread::sleep_for(milliseconds(100));
    CHECK(judge->calls().size() == 1);
}

TEST_CASE("later transcript starts pulses radar and citations", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_later_transcript_starts_pulses_radar_and_citations
    const std::string initial = GENERATE("disabled", "unkeyed");
    CAPTURE(initial);
    LiveSettings settings;
    settings.set("typesafe_enabled", initial == "unkeyed");
    settings.set("typesafe_highlights_enabled", true);
    settings.set("typesafe_question_radar_enabled", true);
    settings.set("typesafe_citations_enabled", true);
    JudgeSwitch judges;
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json& asked, const Json&) -> std::optional<Json> {
        if (asked.contains("claim")) {
            return FakeJudge::choice_answer("supported", 0.99);
        }
        return Json::object();
    });
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    if (initial == "disabled") {
        CHECK(probe.live_highlights() == nullptr);
    }
    settings.set("typesafe_enabled", true);
    judges.set(judge);
    commit(rig, engine, 0, 60,
           {mic_segment(engine.meeting_id(), "sg_one", 10, 20, "Who owns the launch?")});
    const void* signals = probe.live_highlights();
    const void* verifier = probe.citation_verifier();
    REQUIRE(signals != nullptr);
    REQUIRE(verifier != nullptr);
    REQUIRE(eventually([&] { return !judge->calls().empty(); }));
    const auto asked = judge->calls().front().questions;
    CHECK(asked.contains("decision"));
    CHECK(asked.contains("candidate_0"));
    probe.start_fast_features();
    CHECK(probe.live_highlights() == signals);
    CHECK(probe.citation_verifier() == verifier);
    REQUIRE(engine.store()
                ->apply("agent", std::string("notes"),
                        Json::array({Json{{"op", "add_item"},
                                          {"card", "decisions"},
                                          {"text", "The launch has an owner."},
                                          {"evidence", Json::array({"sg_one"})}}}))[0]
                .ok);
    REQUIRE(eventually([&] {
        const auto check =
            engine.store()->snapshot().at("cards").at("decisions")[0].at("citation_check");
        return check.is_object() && check.value("status", std::string()) == "supported";
    }));
}

TEST_CASE("heartbeat retries workers and updates the highlight status without new speech",
          "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_heartbeat_retries_workers_without_new_speech and
    // ::test_heartbeat_updates_highlight_status_after_settings_change_without_speech
    fv::FakeClock clock;
    fv::ManualExecutor strand(clock);
    LiveSettings settings;
    JudgeSwitch judges;
    auto judge = number_judge();
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    EngineSetup setup = live_setup(settings, judges);
    setup.clock = &clock;
    setup.strand = &strand;
    setup.options.title = "Heartbeat";
    MeetingEngine& engine = rig.make(setup);
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    CHECK_FALSE(probe.fast_features_running());
    CHECK(engine.store()->snapshot().at("live_highlights_status") == "off");
    const auto heartbeat = [&] {
        strand.advance(seconds(10));
    };

    settings.set("typesafe_enabled", true);
    settings.set("typesafe_highlights_enabled", true);
    settings.set("typesafe_citations_enabled", true);
    heartbeat();
    CHECK(probe.live_highlights() != nullptr);
    CHECK(probe.citation_verifier() != nullptr);
    REQUIRE_FALSE(statuses(rig).empty());
    CHECK(statuses(rig).back() == "unavailable");
    judges.set(judge);
    heartbeat();
    CHECK(statuses(rig).back() == "on");
    settings.set("typesafe_highlights_enabled", false);
    heartbeat();
    CHECK(statuses(rig).back() == "off");
    const auto count = statuses(rig).size();
    heartbeat();
    CHECK(statuses(rig).size() == count);
    CHECK(judge->calls().empty());
}

TEST_CASE("worker retry does not restart after teardown", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_worker_retry_does_not_restart_after_teardown
    LiveSettings settings;
    settings.set("typesafe_enabled", true);
    settings.set("typesafe_highlights_enabled", true);
    settings.set("typesafe_citations_enabled", true);
    JudgeSwitch judges;
    judges.set(number_judge());
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    REQUIRE(probe.live_highlights() != nullptr);
    REQUIRE(probe.citation_verifier() != nullptr);
    EngineRig::end_and_wait(engine);
    probe.start_fast_features();
    CHECK(probe.live_highlights() == nullptr);
    CHECK(probe.citation_verifier() == nullptr);
    CHECK(probe.voice_listener() == nullptr);
}

TEST_CASE("highlight status reaches snapshots and live clients", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_highlight_status_reaches_snapshots_and_live_clients
    const auto [master, highlights, key, expected] =
        GENERATE(table<bool, bool, bool, std::string>({{false, true, true, "off"},
                                                       {true, false, true, "off"},
                                                       {true, true, false, "unavailable"},
                                                       {true, true, true, "on"}}));
    CAPTURE(master, highlights, key);
    LiveSettings settings;
    settings.set("typesafe_enabled", master);
    settings.set("typesafe_highlights_enabled", highlights);
    JudgeSwitch judges;
    auto judge = number_judge();
    if (key) {
        judges.set(judge);
    }
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    probe.emit_status();
    const auto snapshot = engine.store()->snapshot();
    CHECK(snapshot.at("live_highlights_status") == expected);
    CHECK(fv::meeting_state::MeetingState::from_json(snapshot).to_json().at(
              "live_highlights_status") == expected);
    const auto status = rig.server(0)->messages_of("status").back();
    CHECK(status.at("type") == "status");
    CHECK(status.at("live_highlights_status") == expected);
    CHECK(judge->calls().empty());
}

TEST_CASE("highlight status does not apply current settings to ended meetings", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_highlight_status_does_not_apply_current_settings_to_ended_meetings
    CHECK(fv::meeting_state::MeetingState::from_json(Json{{"meeting_id", "legacy"}})
              .live_highlights_status == "unknown");
    LiveSettings settings; // Fast judgments off: the meeting ends with "off"
    JudgeSwitch judges;
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine& engine = rig.make(live_setup(settings, judges));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    EngineRig::end_and_wait(engine);
    REQUIRE(engine.store()->snapshot().at("status") == "ended");
    REQUIRE(engine.store()->snapshot().at("live_highlights_status") == "off");
    settings.set("typesafe_enabled", true);
    settings.set("typesafe_highlights_enabled", true);
    judges.set(number_judge());
    probe.emit_status();
    CHECK(engine.store()->snapshot().at("live_highlights_status") == "off");
}

TEST_CASE("a spoken correction wakes the agent and keeps its outcome", "[fast_features]") {
    // engine.py _apply_spoken_action("fix_transcript") (audit A29-37): the correction note goes
    // through _notify_human_guidance, and the outcome is stored with the voice_feedback op
    // ({message, at}); the listener's own acknowledgements are only heard.
    class LiveProvider final : public fv::meeting::test::OnlineProvider {
    public:
        std::shared_ptr<fv::meeting_agent::testing::FakeAgentCore> core =
            std::make_shared<fv::meeting_agent::testing::FakeAgentCore>();
        std::shared_ptr<fv::meeting_agent::AgentCore>
        create_live_core(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return core;
        }
        Json polish(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
        Json consolidate(const fv::meeting::RerunContext&, const fv::CancelToken&) override {
            return {{"status", "completed"}};
        }
    };
    JudgeSwitch judges;
    auto local = std::make_shared<FakeJudge>();
    local->set_local(true);
    local->set_handler([](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer("fix_transcript", 0.99));
    });
    judges.set(local);
    EngineRig rig;
    auto setup = fast_setup(true, judges, kVoice);
    auto provider = std::make_shared<LiveProvider>();
    setup.intelligence = provider;
    MeetingEngine& engine = rig.make(std::move(setup));
    MeetingEngineProbe probe(engine);
    engine.start();
    REQUIRE(engine.wait_for_processing(seconds(5)));
    const auto id = engine.meeting_id();
    commit(rig, engine, 0, 10,
           {mic_segment(id, "sg_term", 1, 2, "The Spork release is next."),
            mic_segment(id, "sg_fix", 3, 5, "Note taker, replace Spork with Spark.")});
    const std::string applied =
        "Transcript correction applied. Remove the spoken correction note to undo it.";
    REQUIRE(eventually([&] {
        const auto feedback = engine.store()->snapshot().value("voice_feedback", Json());
        return feedback.is_object() && feedback.value("message", std::string()) == applied;
    }));
    const auto stored = engine.store()->snapshot().at("voice_feedback");
    CHECK_FALSE(stored.contains("phase"));
    CHECK_FALSE(stored.at("at").get<std::string>().empty());
    CHECK(probe.guidance_notices() == 1);
    const auto notes = engine.store()->snapshot().at("cards").at("user_notes");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].at("data").at("kind") == "term_correction");
}

// --- tests/test_typesafe_meeting_runtime.py: the topic-shift judge the live agent's scheduler
// holds. OpenWhisper's "no key" is no device-local judge chosen here, and its hosted judge is
// never asked (a29-local-meeting-judgments).

namespace {

// A device-local judge answering every topic question with `probability`.
std::shared_ptr<FakeJudge> topic_probability_judge(double probability) {
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([probability](const Json&, const Json&) {
        return std::optional(FakeJudge::noul_answer(probability));
    });
    return judge;
}

// A meeting whose live agent comes online with typesafe off; returns the attached judge.
// meeting_agent::TopicJudge: (previous window, window) -> probability, nullopt for no answer.
using TopicCallback =
    std::function<std::optional<double>(const std::string& previous, const std::string& current)>;

TopicCallback start_live_meeting(EngineRig& rig, LiveSettings& settings, JudgeSwitch& judges,
                                 MeetingEngine*& engine) {
    auto setup = live_setup(settings, judges);
    setup.options.cloud_enabled = true;
    setup.intelligence = std::make_shared<fv::meeting::FakeIntelligence>();
    engine = &rig.make(std::move(setup));
    engine->start();
    REQUIRE(engine->wait_for_processing(seconds(5)));
    MeetingEngineProbe probe(*engine);
    REQUIRE(eventually([&] { return static_cast<bool>(probe.attached_topic_judge()); }));
    return probe.attached_topic_judge();
}

} // namespace

TEST_CASE("attached topic callback follows switches key and cloud", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_attached_topic_callback_follows_switches_key_and_cloud
    LiveSettings settings;
    JudgeSwitch judges;
    // After the settings and judges it reads, so the engine shuts down first.
    EngineRig rig;
    MeetingEngine* engine = nullptr;
    const auto callback = start_live_meeting(rig, settings, judges, engine);
    CHECK_FALSE(callback("budget", "hiring").has_value());
    settings.set("typesafe_enabled", true);
    settings.set("typesafe_topic_shift_enabled", false);
    CHECK_FALSE(callback("budget", "hiring").has_value());
    settings.set("typesafe_topic_shift_enabled", true);
    // No judge chosen (OpenWhisper: no key), then a hosted one: neither answers nor is asked.
    CHECK_FALSE(callback("budget", "hiring").has_value());
    auto hosted = topic_probability_judge(0.9);
    hosted->set_local(false);
    judges.set(hosted);
    CHECK_FALSE(callback("budget", "hiring").has_value());
    CHECK(hosted->calls().empty());

    auto local = topic_probability_judge(0.9);
    judges.set(local);
    CHECK(callback("budget", "hiring") == 0.9);
    REQUIRE(local->calls().size() == 1);
    CHECK(local->calls()[0].state.at("previous_window") == "budget");
    CHECK(local->calls()[0].state.at("window") == "hiring");
    for (const char* flag : {"typesafe_topic_shift_enabled", "typesafe_enabled"}) {
        settings.set(flag, false);
        CHECK_FALSE(callback("budget", "hiring").has_value());
        settings.set(flag, true);
    }
    CHECK(local->calls().size() == 1);
    // OpenWhisper's cloud switch gates only a hosted judge (_consented_judge passes a local one
    // whatever cloud_enabled says); a device-local judge keeps answering with insights off.
    REQUIRE(engine->store()->update_runtime_fields(Json{{"cloud_enabled", false}}));
    CHECK(callback("budget", "hiring") == 0.9);
    REQUIRE(engine->store()->update_runtime_fields(Json{{"cloud_enabled", true}}));
    CHECK(callback("budget", "hiring") == 0.9);
    CHECK(local->calls().size() == 3);
}

TEST_CASE("topic result is discarded when disabled in flight", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_topic_result_is_discarded_when_disabled_in_flight
    LiveSettings settings;
    JudgeSwitch judges;
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    judge->set_handler([&settings](const Json&, const Json&) {
        settings.set("typesafe_topic_shift_enabled", false);
        return std::optional(FakeJudge::noul_answer(0.99));
    });
    judges.set(judge);
    EngineRig rig;
    MeetingEngine* engine = nullptr;
    const auto callback = start_live_meeting(rig, settings, judges, engine);
    settings.set("typesafe_enabled", true);
    CHECK_FALSE(callback("budget", "hiring").has_value());
    CHECK(judge->calls().size() == 1);
}

TEST_CASE("intelligence attaches topic callback before typesafe is enabled", "[fast_features]") {
    // from
    // tests/test_typesafe_meeting_runtime.py::test_intelligence_attaches_topic_callback_before_typesafe_is_enabled
    LiveSettings settings;
    JudgeSwitch judges;
    EngineRig rig;
    MeetingEngine* engine = nullptr;
    // The live agent comes online with Fast judgments off and no judge chosen: the scheduler is
    // still handed the callback, which resolves the switches and the judge per check.
    const auto callback = start_live_meeting(rig, settings, judges, engine);
    CHECK(engine->store()->snapshot().at("intelligence_online") == true);
    CHECK_FALSE(callback("budget", "hiring").has_value());
    settings.set("typesafe_enabled", true);
    judges.set(topic_probability_judge(0.9));
    CHECK(callback("budget", "hiring") == 0.9);
}
