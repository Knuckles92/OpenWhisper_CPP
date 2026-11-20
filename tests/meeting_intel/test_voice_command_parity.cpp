// tests/test_voice_commands.py: wake names, topics, referents, the ops a command becomes and the
// listener's feedback. OpenWhisper's FakeStore is the real MeetingStateStore here, its FakeJudge
// is llm::FakeJudge marked device-local (OpenWhisper_CPP sends spoken commands to no other judge),
// and its ImmediateExecutor and ManualExecutor are the listener's own worker, ordered by waiting on
// the feedback it says.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "base/identity.h"
#include "llm/testing/fake_judge.h"
#include "meeting_intel/intel_settings.h"
#include "meeting_intel/voice_commands.h"
#include "meeting_state/corrections.h"
#include "meeting_state/fast_ops.h"
#include "meeting_state/ids.h"
#include "meeting_state/schema.h"
#include "meeting_state/segment_ops.h"
#include "meeting_state/testing/fake_meeting_repository.h"
#include "pyport/json.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using fv::llm::FakeJudge;
using namespace fv::meeting_intel;

// services.settings.DEFAULT_VOICE_COMMAND_NAMES, OpenWhisper's names included.
const std::vector<std::string>& names() {
    static const auto defaults = default_voice_command_names(fv::legacy_identity());
    return defaults;
}

Json seg(std::string id, double start, double end, std::string text, std::string channel = "") {
    Json row{{"id", std::move(id)}, {"start_s", start}, {"end_s", end}, {"text", std::move(text)}};
    if (!channel.empty()) {
        row["channel"] = std::move(channel);
    }
    return row;
}

// rows(): a decision, an owner, then the command.
std::vector<Json> rows() {
    return {seg("sg_1", 100.0, 104.0, "We'll ship the beta on Tuesday."),
            seg("sg_2", 104.5, 106.0, "Maya owns the release notes."),
            seg("sg_3", 107.0, 109.0, "Note taker, mark that as a decision.")};
}

// preview(): a provisional row on the microphone.
Json preview(std::string text = "Assistant, note that the launch is Friday.", double start = 0,
             double end = 3, std::string channel = "mic") {
    return Json{{"channel", std::move(channel)},
                {"start_s", start},
                {"end_s", end},
                {"text", std::move(text)},
                {"final", false}};
}

Json with_id(Json row, std::string id) {
    row["id"] = std::move(id);
    return row;
}

std::shared_ptr<fv::meeting_state::MeetingStateStore> store() {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_voice";
    state.cloud_enabled = true;
    return std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state));
}

std::shared_ptr<FakeJudge> local_judge() {
    auto judge = std::make_shared<FakeJudge>();
    judge->set_local(true);
    return judge;
}

std::shared_ptr<FakeJudge> answering(const std::string& choice, double confidence) {
    auto judge = local_judge();
    judge->set_handler([choice, confidence](const Json&, const Json&) {
        return std::optional(FakeJudge::choice_answer(choice, confidence));
    });
    return judge;
}

// on_feedback's events, waitable from the test thread.
struct Feedback {
    void operator()(const Json& event) {
        {
            std::lock_guard lock(mutex);
            events.push_back(event);
        }
        changed.notify_all();
    }
    // Waits until an event with `phase` (the n-th one) was said.
    bool wait_for(const std::string& phase, std::size_t n = 1) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 3s, [&] {
            std::size_t seen = 0;
            for (const auto& event : events) {
                seen += event.at("phase") == phase ? 1 : 0;
            }
            return seen >= n;
        });
    }
    std::vector<std::string> phases() const {
        std::lock_guard lock(mutex);
        std::vector<std::string> out;
        for (const auto& event : events) {
            out.push_back(event.at("phase").get<std::string>());
        }
        return out;
    }
    Json last() const {
        std::lock_guard lock(mutex);
        return events.empty() ? Json() : events.back();
    }
    std::size_t size() const {
        std::lock_guard lock(mutex);
        return events.size();
    }
    mutable std::mutex mutex;
    std::condition_variable changed;
    std::vector<Json> events;
};

// streaming_listener: the default names, a device-local judge and recorded feedback.
struct Listening {
    explicit Listening(
        std::shared_ptr<FakeJudge> judge_ = answering("note_this", 0.95),
        std::function<bool()> allowed = [] { return true; })
        : judge(std::move(judge_)), listener(state, names(),
                                             {.judge = [this] { return judge; },
                                              .allowed = std::move(allowed),
                                              .feedback =
                                                  [this](const Json& event) {
                                                      feedback(event);
                                                  }}) {}

    Json notes() const { return state->snapshot().at("cards").at("live_notes"); }
    bool unchanged() const { return state->seq() == 0; }

    std::shared_ptr<fv::meeting_state::MeetingStateStore> state = store();
    std::shared_ptr<FakeJudge> judge;
    Feedback feedback;
    VoiceCommandListener listener;
};

TEST_CASE("TestWakeNames ordinary speech does not wake", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestWakeNames::test_ordinary_speech_does_not_wake
    const auto pattern = compile_wake_pattern(names());
    CHECK_FALSE(mentions_assistant("she had to whisper because the baby slept", pattern));
    CHECK_FALSE(mentions_assistant("the assistants were helpful", pattern));
    CHECK_FALSE(mentions_assistant("can you write that down for me in your notebook", pattern));
    CHECK_FALSE(mentions_assistant("", pattern));
    CHECK(mentions_assistant("hey notetaker capture that", pattern));
    CHECK(mentions_assistant("open-whisper new topic budget", pattern));
}

TEST_CASE("TestWakeNames empty names disable", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestWakeNames::test_empty_names_disable
    CHECK_FALSE(compile_wake_pattern({}));
    CHECK_FALSE(mentions_assistant("note taker", std::nullopt));
    auto judge = answering("mark_decision", 0.99);
    auto state = store();
    VoiceCommandListener listener(state, {}, {.judge = [judge] {
                                      return judge;
                                  }});
    CHECK_FALSE(listener.enabled());
    CHECK(listener.observe(rows()) == 0);
    listener.shutdown();
    CHECK(judge->calls().empty());
}

TEST_CASE("TestTopicExtraction never invents", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestTopicExtraction::test_never_invents
    CHECK_FALSE(extract_spoken_topic("note taker mark that as a decision"));
    CHECK_FALSE(extract_spoken_topic("assistant new topic:"));
}

TEST_CASE("TestReferents takes two recent segments before command", "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::TestReferents::test_takes_two_recent_segments_before_command
    const auto rs = rows();
    const auto picked = referent_rows({rs[0], rs[1]}, rs[2]);
    REQUIRE(picked.size() == 2);
    CHECK(picked[0].at("id") == "sg_1");
    CHECK(picked[1].at("id") == "sg_2");
}

TEST_CASE("TestReferents skips old and excluded segments", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestReferents::test_skips_old_and_excluded_segments
    auto rs = rows();
    rs[0]["end_s"] = 70.0; // 37 s before the command
    CHECK(referent_rows({rs[0], rs[1]}, rs[2], {"sg_2"}).empty());
    const auto fresh = rows();
    const auto picked = referent_rows({fresh[0], fresh[1]}, fresh[2], {"sg_2"});
    REQUIRE(picked.size() == 1);
    CHECK(picked[0].at("id") == "sg_1");
}

TEST_CASE("TestBuildOps decision cites referents and command", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestBuildOps::test_decision_cites_referents_and_command
    const auto rs = rows();
    const auto ops = voice_command_ops("mark_decision", rs[2], {rs[0], rs[1]});
    REQUIRE(ops.size() == 1);
    CHECK(ops[0].at("op") == "add_item");
    CHECK(ops[0].at("card") == "decisions");
    CHECK(ops[0].at("text") == "We'll ship the beta on Tuesday. Maya owns the release notes.");
    CHECK(ops[0].at("evidence") == Json::array({"sg_1", "sg_2", "sg_3"}));
    CHECK(ops[0].at("data") == Json{{"source", "voice_command"},
                                    {"command", "mark_decision"},
                                    {"command_segment_id", "sg_3"}});
    CHECK(ops[0].size() == 5);
}

TEST_CASE("TestBuildOps action and note cards", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestBuildOps::test_action_and_note_cards
    const auto rs = rows();
    CHECK(voice_command_ops("mark_action", rs[2], {rs[1]})[0].at("card") == "action_items");
    const auto note = voice_command_ops("note_this", rs[2], {rs[1]})[0];
    CHECK(note.at("card") == "live_notes");
    CHECK(note.at("data").at("heading") == "Requested note");
    CHECK(note.at("data").at("start_s") == rs[1].at("start_s"));
}

TEST_CASE("TestBuildOps set topic uses extracted phrase only", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestBuildOps::test_set_topic_uses_extracted_phrase_only
    auto cmd = seg("sg_9", 1.0, 2.0, "note taker new topic: vendor review");
    CHECK(
        voice_command_ops("set_topic", cmd, {}) ==
        Json::array({Json{
            {"op", "set_topic"}, {"text", "vendor review"}, {"evidence", Json::array({"sg_9"})}}}));
    cmd["text"] = "note taker let's talk about something else";
    CHECK(voice_command_ops("set_topic", cmd, {}).empty());
}

TEST_CASE("TestBuildOps nothing to mark and unsupported commands", "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::TestBuildOps::test_nothing_to_mark_and_unsupported_commands
    const auto rs = rows();
    CHECK(voice_command_ops("mark_decision", rs[2], {}).empty());
    CHECK(voice_command_ops("recap", rs[2], {rs[0], rs[1]}).empty());
    CHECK(voice_command_ops("fix_transcript", rs[2], {rs[0], rs[1]}).empty());
}

TEST_CASE("TestListener applies decision as system actor", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestListener::test_applies_decision_as_system_actor
    Listening rig(answering("mark_decision", 0.93));
    CHECK(rig.listener.observe(rows()) == 1);
    REQUIRE(rig.feedback.wait_for("saved"));
    rig.listener.shutdown();
    const auto calls = rig.judge->calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].state.at("segment") == "Note taker, mark that as a decision.");
    CHECK(calls[0].state.at("previous_segments") ==
          Json::array({"We'll ship the beta on Tuesday.", "Maya owns the release notes."}));
    const auto decisions = rig.state->snapshot().at("cards").at("decisions");
    REQUIRE(decisions.size() == 1);
    CHECK(decisions[0].at("author_type") == "system");
    CHECK(decisions[0].at("author_id") == std::string(fv::meeting_state::VOICE_COMMAND_ACTOR));
    CHECK(decisions[0].at("evidence") == Json::array({"sg_1", "sg_2", "sg_3"}));
    CHECK(rig.feedback.last().at("message") == "Decision noted");
}

TEST_CASE("TestListener unnamed segments are never judged", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestListener::test_unnamed_segments_are_never_judged
    Listening rig(answering("mark_decision", 0.99));
    const auto rs = rows();
    CHECK(rig.listener.observe({rs[0], rs[1]}) == 0);
    rig.listener.shutdown();
    CHECK(rig.judge->calls().empty());
    CHECK(rig.unchanged());
    CHECK(rig.feedback.size() == 0);
}

TEST_CASE("TestListener none and low confidence do not apply", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestListener::test_none_and_low_confidence_do_not_apply
    struct Case {
        std::optional<Json> answer;
        std::string phase;
    };
    for (const auto& [answer, phase] :
         {Case{FakeJudge::choice_answer("none", 0.99), "uncertain"},
          Case{FakeJudge::choice_answer("mark_action", 0.4), "uncertain"},
          Case{std::nullopt, "error"}}) {
        auto judge = local_judge();
        judge->push(answer);
        Listening rig(judge);
        CHECK(rig.listener.observe(rows()) == 1);
        REQUIRE(rig.feedback.wait_for(phase));
        rig.listener.shutdown();
        CHECK(rig.unchanged());
        CHECK(rig.feedback.last().at("phase") == phase);
    }
}

TEST_CASE("TestListener callback receives results", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestListener::test_callback_receives_results
    auto judge = answering("note_this", 0.8);
    auto state = store();
    std::promise<std::pair<std::string, std::size_t>> seen;
    auto result = seen.get_future();
    VoiceCommandListener listener(state, {"note taker"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .applied =
                                       [&](const std::string& command, const auto& results) {
                                           seen.set_value({command, results.size()});
                                       }});
    listener.observe(rows());
    REQUIRE(result.wait_for(3s) == std::future_status::ready);
    listener.shutdown();
    CHECK(result.get() == std::pair<std::string, std::size_t>{"note_this", 1});
}

TEST_CASE("TestListener shutdown stops observing", "[voice_commands]") {
    // from tests/test_voice_commands.py::TestListener::test_shutdown_stops_observing
    Listening rig(answering("mark_decision", 0.9));
    rig.listener.shutdown();
    CHECK(rig.listener.observe(rows()) == 0);
    rig.listener.observe_preview(preview());
    CHECK(rig.judge->calls().empty());
    CHECK(rig.feedback.size() == 0);
}

TEST_CASE("changed preview is coalesced while judge is busy", "[voice_commands]") {
    // from tests/test_voice_commands.py::test_changed_preview_is_coalesced_while_judge_is_busy
    // A committed command on another channel holds the worker, as the unrun ManualExecutor does.
    auto judge = local_judge();
    std::promise<void> entered;
    std::promise<void> release;
    auto holding = entered.get_future();
    auto go = release.get_future().share();
    judge->set_handler([&, go](const Json& state, const Json&) -> std::optional<Json> {
        if (state.at("segment").get<std::string>().contains("hold")) {
            entered.set_value();
            go.wait_for(3s);
            return FakeJudge::choice_answer("none", 0.99);
        }
        return FakeJudge::choice_answer("note_this", 0.95);
    });
    Listening rig(judge);
    rig.listener.observe({with_id(preview("Assistant, hold on.", 0, 2, "loopback"), "sg_hold")});
    REQUIRE(holding.wait_for(3s) == std::future_status::ready);
    for (int end = 3; end < 80; ++end) {
        rig.listener.observe_preview(
            preview("Assistant, note that the count is " + std::to_string(end) + ".", 0, end));
    }
    release.set_value();
    REQUIRE(rig.feedback.wait_for("recognized"));
    rig.listener.shutdown();
    const auto calls = judge->calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1].state.at("segment").get<std::string>().contains("79"));
    CHECK(rig.unchanged());
}

TEST_CASE("same segment point is captured instead of unrelated previous point",
          "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::test_same_segment_point_is_captured_instead_of_unrelated_previous_point
    Listening rig;
    rig.listener.observe({with_id(preview("Older unrelated discussion.", 0, 1), "sg_old")});
    rig.listener.observe(
        {with_id(preview("The launch is Friday. Assistant, capture that.", 1, 4), "sg_cmd")});
    REQUIRE(rig.feedback.wait_for("saved"));
    rig.listener.shutdown();
    const auto notes = rig.notes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].at("text") == "The launch is Friday");
    CHECK(notes[0].at("evidence") == Json::array({"sg_cmd"}));
}

TEST_CASE("preview consent and shutdown block network and mutations", "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::test_preview_consent_and_shutdown_block_network_and_mutations
    std::atomic<bool> allowed{false};
    Listening rig(answering("note_this", 0.95), [&] { return allowed.load(); });
    rig.listener.observe_preview(preview());
    REQUIRE(rig.feedback.wait_for("unavailable"));
    CHECK(rig.judge->calls().empty());
    CHECK(rig.feedback.last().at("message") == std::string(VOICE_NEEDS_LOCAL_JUDGE));
    rig.listener.shutdown();
    allowed = true;
    const auto count = rig.feedback.size();
    rig.listener.observe_preview(preview("Assistant, note that Tuesday is cancelled.", 0, 4));
    rig.listener.observe({with_id(preview(), "sg_cmd")});
    CHECK(rig.judge->calls().empty());
    CHECK(rig.unchanged());
    CHECK(rig.feedback.size() == count);
}

TEST_CASE("missing key and service failure have visible feedback", "[voice_commands]") {
    // from tests/test_voice_commands.py::test_missing_key_and_service_failure_have_visible_feedback
    // No device-local judge is OpenWhisper_CPP's missing key: the message names what is missing.
    auto state = store();
    Feedback feedback;
    std::shared_ptr<FakeJudge> judge;
    std::mutex judge_mutex;
    VoiceCommandListener listener(state, names(),
                                  {.judge = [&]() -> std::shared_ptr<fv::llm::Judge> {
                                       std::lock_guard lock(judge_mutex);
                                       return judge;
                                   },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           feedback(event);
                                       }});
    listener.observe_preview(preview());
    REQUIRE(feedback.wait_for("unavailable"));
    CHECK(feedback.last().at("message") == std::string(VOICE_NEEDS_LOCAL_JUDGE));
    {
        std::lock_guard lock(judge_mutex);
        judge = local_judge(); // answers nothing: the service failed
    }
    listener.observe({with_id(preview(), "sg_cmd")});
    REQUIRE(feedback.wait_for("error"));
    listener.shutdown();
    CHECK(feedback.last().at("message") == "Voice recognition is unavailable. Please try again.");
    CHECK(state->seq() == 0);
}

TEST_CASE("committed queue is bounded without blocking caller", "[voice_commands]") {
    // from tests/test_voice_commands.py::test_committed_queue_is_bounded_without_blocking_caller
    auto judge = local_judge();
    std::promise<void> release;
    auto go = release.get_future().share();
    judge->set_handler([go](const Json&, const Json&) -> std::optional<Json> {
        go.wait_for(3s);
        return FakeJudge::choice_answer("none", 0.99);
    });
    Listening rig(judge);
    int queued = 0;
    for (int i = 0; i < 30; ++i) {
        queued += rig.listener.observe(
            {with_id(preview("Assistant, note that the launch is Friday.", i * 4, i * 4 + 3),
                     "sg_" + std::to_string(i))});
    }
    CHECK(queued == 16);
    CHECK(rig.feedback.last().at("phase") == "error");
    CHECK(rig.feedback.last().at("message") ==
          "Voice commands are busy. Please repeat that shortly.");
    CHECK(rig.unchanged());
    release.set_value();
    rig.listener.shutdown();
}

TEST_CASE("consent revoked during judgment prevents save", "[voice_commands]") {
    // from tests/test_voice_commands.py::test_consent_revoked_during_judgment_prevents_save
    std::atomic<bool> allowed{true};
    auto judge = local_judge();
    judge->set_handler([&](const Json&, const Json&) {
        allowed = false;
        return std::optional(FakeJudge::choice_answer("note_this", 0.99));
    });
    Listening rig(judge, [&] { return allowed.load(); });
    // The second command runs after the first on the one worker, so its "unavailable" says the
    // first judgment has finished.
    rig.listener.observe({with_id(preview(), "sg_cmd"),
                          with_id(preview("Assistant, note that the venue moved.", 4, 6), "sg_2")});
    REQUIRE(rig.feedback.wait_for("unavailable"));
    rig.listener.shutdown();
    CHECK(judge->calls().size() == 1);
    CHECK(rig.unchanged());
    for (const auto& phase : rig.feedback.phases()) {
        CHECK(phase != "saved");
    }
}

TEST_CASE("budget note from real meeting uses dictation not prior praise", "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::test_budget_note_from_real_meeting_uses_dictation_not_prior_praise
    Listening rig(answering("note_this", 0.66));
    const std::vector<Json> transcript{
        seg("sg_praise_1", 48.074, 49.114, "This is gonna work.", "mic"),
        seg("sg_praise_2", 49.454, 50.894, "Insanely good.", "mic"),
        seg("sg_wake", 59.174, 60.214, "Assistant.", "mic"),
        seg("sg_budget", 60.874, 66.074,
            "Add a note that we need to get a thousand dollars for budget A.", "mic")};
    CHECK(rig.listener.observe(transcript) == 1);
    REQUIRE(rig.feedback.wait_for("saved"));
    CHECK(rig.listener.observe(transcript) == 0);
    rig.listener.shutdown();
    const auto notes = rig.notes();
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].at("text") == "we need to get a thousand dollars for budget A.");
    CHECK(notes[0].at("data").at("start_s") == 59.174);
    CHECK(notes[0].at("evidence") == Json::array({"sg_wake", "sg_budget"}));
    CHECK(rig.feedback.last().at("phase") == "saved");
    CHECK(rig.judge->calls().size() == 1);
}

TEST_CASE("polite dictation and split note preamble preserve the requested content",
          "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::test_polite_dictation_and_split_note_preamble_preserve_the_requested_content
    for (const std::string lead :
         {"Add a note that", "Please add a note that", "Can you please add a note that",
          "Could you take a note that", "Would you make a note that"}) {
        CAPTURE(lead);
        Listening rig;
        rig.listener.observe({with_id(preview("Unrelated earlier point.", 0, 1), "sg_old")});
        CHECK(rig.listener.observe({with_id(preview("Assistant, " + lead, 1, 2), "sg_wake")}) == 0);
        CHECK(rig.unchanged());
        rig.listener.observe(
            {with_id(preview("budget A needs a thousand dollars.", 2, 5), "sg_budget")});
        REQUIRE(rig.feedback.wait_for("saved"));
        rig.listener.shutdown();
        const auto notes = rig.notes();
        REQUIRE(notes.size() == 1);
        CHECK(notes[0].at("text") == "budget A needs a thousand dollars.");
        CHECK(notes[0].at("evidence") == Json::array({"sg_wake", "sg_budget"}));
    }
}

TEST_CASE("spoken note is persisted and broadcast before saved feedback without an agent",
          "[voice_commands]") {
    // from
    // tests/test_voice_commands.py::test_spoken_note_is_persisted_and_broadcast_before_saved_feedback_without_an_agent
    const std::string mid = "spoken-note-immediate";
    auto repo = std::make_shared<fv::meeting_state::testing::FakeMeetingRepository>();
    repo->create_meeting(Json{{"id", mid},
                              {"title", "Budget planning"},
                              {"status", "active"},
                              {"started_at", "2026-09-19T19:24:00Z"},
                              {"host_token", "host"},
                              {"guest_token", "guest"},
                              {"cloud_enabled", true},
                              {"spool_dir", "unused"}});
    const auto row = seg("sg_budget", 60, 66,
                         "Assistant, add a note that budget A needs a thousand dollars.", "mic");
    fv::meeting_state::TranscriptSegment segment;
    segment.segment_id = "sg_budget";
    segment.meeting_id = mid;
    segment.channel = "mic";
    segment.start_s = 60;
    segment.end_s = 66;
    segment.text = row.at("text");
    repo->add_segments({segment});
    fv::meeting_state::MeetingState document;
    document.meeting_id = mid;
    fv::meeting_state::SegmentPredicates predicates;
    predicates.segment_exists = [repo, mid](std::string_view id) {
        return repo->segment_exists(mid, id);
    };
    auto state = std::make_shared<fv::meeting_state::MeetingStateStore>(
        std::move(document), repo, fv::meeting_state::SegmentHandler{}, fv::system_clock(),
        fv::meeting_state::random_ids(), predicates);
    const auto persisted_notes = [&] {
        return fv::py::loads(repo->get_meeting(mid)->at("state_json").get<std::string>())
            .at("cards")
            .at("live_notes");
    };
    std::mutex mutex;
    std::vector<std::string> order;
    std::vector<Json> patches;
    std::promise<void> saved;
    auto done = saved.get_future();
    auto broadcast = state->subscribe([&](std::int64_t, const auto& results) {
        CHECK(persisted_notes()[0].at("text") == "budget A needs a thousand dollars.");
        std::lock_guard lock(mutex);
        order.emplace_back("broadcast");
        for (const auto& result : results) {
            patches.push_back(result.effect);
        }
    });
    auto judge = answering("note_this", 0.95);
    VoiceCommandListener listener(state, names(),
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") != "saved") {
                                               return;
                                           }
                                           CHECK_FALSE(persisted_notes().empty());
                                           CHECK(event.at("message") == "Added to Meeting Notes");
                                           {
                                               std::lock_guard lock(mutex);
                                               CHECK_FALSE(patches.empty());
                                               order.emplace_back("saved");
                                           }
                                           saved.set_value();
                                       }});
    CHECK(listener.observe({row}) == 1);
    REQUIRE(done.wait_for(3s) == std::future_status::ready);
    CHECK(listener.observe({row}) == 0);
    listener.shutdown();
    CHECK((order == std::vector<std::string>{"broadcast", "saved"}));
    REQUIRE_FALSE(patches.empty());
    CHECK(patches[0].at("entity") == "item");
    CHECK(patches[0].at("item").at("card") == "live_notes");
    CHECK(patches[0].at("item").at("data").at("start_s") == 60.0);
    CHECK(state->snapshot().at("cards").at("key_points").empty());
    const auto restored = fv::meeting_state::MeetingState::from_json(
        fv::py::loads(repo->get_meeting(mid)->at("state_json").get<std::string>()));
    REQUIRE(restored.cards["live_notes"].size() == 1);
    CHECK(restored.cards["live_notes"][0].is_protected());
    CHECK(persisted_notes().size() == 1);
}

} // namespace

// --- tests/test_meeting_fast_features.py: meeting/voice_actions.py correction_op, which is
// voice_command_ops("fix_transcript", ...) here --------------------------------------------

namespace {

Json fast_row(std::string id, std::string text, double start = 10, double end = 15) {
    return Json{
        {"id", std::move(id)}, {"text", std::move(text)}, {"start_s", start}, {"end_s", end}};
}

TEST_CASE("spoken correction roundtrips and removal undoes", "[voice_commands]") {
    // from
    // tests/test_meeting_fast_features.py::test_spoken_correction_roundtrips_and_removal_undoes
    const std::string spoken =
        GENERATE("Note taker, replace Myra with Maya.", "Assistant, I said Maya not Myra.",
                 "Note taker, change \"Myra\" to \"Maya\" please.");
    CAPTURE(spoken);
    auto target = store();
    const auto ops = voice_command_ops("fix_transcript", fast_row("sg_cmd", spoken, 20, 25),
                                       {fast_row("sg_one", "Myra owns release notes.")});
    REQUIRE(ops.size() == 1);
    const auto results =
        target->apply("system", std::string(fv::meeting_state::VOICE_COMMAND_ACTOR), ops);
    REQUIRE(results.size() == 1);
    REQUIRE(results[0].ok);
    CHECK(fv::meeting_state::correct_text("Myra owns release notes.",
                                          fv::meeting_state::term_rules(target->snapshot())) ==
          "Maya owns release notes.");
    CHECK(target->snapshot().at("cards").at("user_notes")[0].at("author_type") == "system");
    REQUIRE(
        target
            ->apply("host", std::string("me"),
                    Json::array({Json{{"op", "remove_item"}, {"id", *results[0].target_id}}}))[0]
            .ok);
    CHECK(fv::meeting_state::term_rules(target->snapshot()).empty());
}

TEST_CASE("spoken correction without source or replacement is rejected", "[voice_commands]") {
    // from
    // tests/test_meeting_fast_features.py::test_spoken_correction_without_source_or_replacement_is_rejected
    const auto decision = fast_row("sg_one", "We agree to ship the beta on Friday.");
    CHECK(voice_command_ops("fix_transcript", fast_row("sg_cmd", "Note taker, fix that name."),
                            {decision})
              .empty());
    CHECK(voice_command_ops("fix_transcript",
                            fast_row("sg_cmd", "Note taker, replace Myra with Maya."), {decision})
              .empty());
}

} // namespace
