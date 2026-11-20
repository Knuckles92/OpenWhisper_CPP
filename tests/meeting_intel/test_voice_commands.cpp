#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "llm/testing/fake_judge.h"
#include "meeting_intel/voice_commands.h"
#include "meeting_state/schema.h"

namespace {

using namespace std::chrono_literals;
using fv::Json;
using namespace fv::meeting_intel;

Json row(std::string id, double start, double end, std::string text) {
    return {{"id", std::move(id)}, {"start_s", start}, {"end_s", end}, {"text", std::move(text)}};
}

std::shared_ptr<fv::meeting_state::MeetingStateStore> store() {
    fv::meeting_state::MeetingState state;
    state.meeting_id = "m_voice";
    return std::make_shared<fv::meeting_state::MeetingStateStore>(std::move(state));
}

TEST_CASE("voice commands use complete wake names and preserve cited transcript rows",
          "[voice_commands]") {
    const auto wake = compile_wake_pattern({"note taker", "A.I."});
    REQUIRE(wake);
    CHECK(wake->search("Hey NOTE-TAKER, next topic."));
    CHECK_FALSE(wake->search("Notebook and a notetakers' guide"));
    CHECK(wake->search("Hey A.I.,"));
    CHECK(extract_spoken_topic("note taker new topic: hiring plan") == "hiring plan");
    CHECK(extract_spoken_topic("assistant we're moving on to the budget now") == "the budget");
    CHECK(extract_spoken_topic("OpenWhisper set the topic to release timeline please.") ==
          "release timeline");
    CHECK(extract_spoken_topic("Note taker, next topic is Launch planning, please.") ==
          "Launch planning,");
    CHECK_FALSE(extract_spoken_topic("note taker mark that as a decision"));

    const auto note = voice_command_ops(
        "note_this", row("sg_cmd", 15, 17, "Assistant, note this"),
        {row("sg_old", 1, 5, "Old point"), row("sg_new", 13, 14, "Keep this point")});
    REQUIRE(note.size() == 1);
    CHECK(note[0].at("card") == "live_notes");
    CHECK(note[0].at("text") == "Old point Keep this point");
    CHECK(note[0].at("evidence") == Json::array({"sg_old", "sg_new", "sg_cmd"}));
    CHECK(note[0].at("data").at("start_s") == 1.0);

    const auto fix = voice_command_ops(
        "fix_transcript", row("sg_fix", 18, 19, "Assistant, replace Spork with Spark."),
        {row("sg_doc", 13, 14, "The Spork release")});
    REQUIRE(fix.size() == 1);
    CHECK(fix[0].at("data").at("selected_text") == "Spork");
    CHECK(fix[0].at("data").at("replacement") == "Spark");
    CHECK(fix[0].at("evidence") == Json::array({"sg_doc", "sg_fix"}));
}

TEST_CASE("meeting voice command listener asks only a local judge", "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    auto state = store();
    std::promise<void> lookedUp;
    auto lookup = lookedUp.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge =
                                       [&] {
                                           lookedUp.set_value();
                                           return judge;
                                       },
                                   .allowed =
                                       [] {
                                           return true;
                                       }});
    listener.observe({row("sg_cmd", 5, 6, "Assistant, next topic: Launch")});
    REQUIRE(lookup.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    CHECK(judge->calls().empty());
    CHECK(state->snapshot().dump().find("Launch") == std::string::npos);
}

TEST_CASE("a committed wake phrase applies a local voice command once", "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    auto state = store();
    std::promise<Json> callback;
    auto saved = callback.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "saved") {
                                               callback.set_value(event);
                                           }
                                       }});
    const auto command = row("sg_cmd", 5, 6, "Assistant, next topic: Launch");
    listener.observe({command, command});
    REQUIRE(saved.wait_for(2s) == std::future_status::ready);
    CHECK(saved.get().at("phase") == "saved");
    listener.shutdown();
    CHECK(judge->calls().size() == 1);
    CHECK(state->snapshot().dump().find("Launch") != std::string::npos);
}

TEST_CASE("voice preview recognizes without saving and committed text is judged again",
          "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    judge->push(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    auto state = store();
    const auto empty = state->snapshot();
    std::promise<void> recognized;
    std::promise<void> saved;
    auto previewDone = recognized.get_future();
    auto committedDone = saved.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "recognized") {
                                               recognized.set_value();
                                           }
                                           if (event.at("phase") == "saved") {
                                               saved.set_value();
                                           }
                                       }});
    listener.observe_preview(row("", 5, 6, "Assistant, next topic: Draft"));
    REQUIRE(previewDone.wait_for(2s) == std::future_status::ready);
    CHECK(state->snapshot() == empty);
    listener.observe({row("sg_committed", 5, 6, "Assistant, next topic: Final")});
    REQUIRE(committedDone.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    CHECK(judge->calls().size() == 2);
    CHECK(state->snapshot().dump().find("Final") != std::string::npos);
    CHECK(state->snapshot().dump().find("Draft") == std::string::npos);
}

// from
// tests/test_voice_commands.py::test_preview_acknowledges_without_mutating_and_commit_saves_once
TEST_CASE("test_preview_acknowledges_without_mutating_and_commit_saves_once", "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json&, const Json&) {
        return std::optional(fv::llm::FakeJudge::choice_answer("note_this", 0.95));
    });
    auto state = store();
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> phases;
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           const std::lock_guard lock(mutex);
                                           phases.push_back(event.at("phase"));
                                           changed.notify_all();
                                       }});
    const auto wait_for = [&](std::size_t count) {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s, [&] { return phases.size() >= count; });
    };
    Json preview{{"channel", "mic"},
                 {"start_s", 0},
                 {"end_s", 3},
                 {"text", "Assistant, note that the launch is Friday."},
                 {"final", false}};
    listener.observe_preview(preview);
    REQUIRE(wait_for(2));
    {
        const std::lock_guard lock(mutex);
        CHECK(phases == std::vector<std::string>{"heard", "recognized"});
    }
    const auto empty = state->snapshot();
    CHECK(empty.at("cards").at("live_notes").empty());
    Json committed = preview;
    committed["id"] = "sg_command";
    CHECK(listener.observe({committed}) == 1);
    REQUIRE([&] {
        std::unique_lock lock(mutex);
        return changed.wait_for(lock, 2s,
                                [&] { return !phases.empty() && phases.back() == "saved"; });
    }());
    const auto notes = state->snapshot().at("cards").at("live_notes");
    REQUIRE(notes.size() == 1);
    CHECK(notes[0].at("text") == "the launch is Friday.");
    CHECK(notes[0].at("evidence") == Json::array({"sg_command"}));
    listener.observe({committed});
    std::size_t count = 0;
    {
        const std::lock_guard lock(mutex);
        count = phases.size();
    }
    preview["end_s"] = 4;
    listener.observe_preview(preview);
    std::this_thread::sleep_for(100ms);
    listener.shutdown();
    CHECK(state->snapshot().at("cards").at("live_notes").size() == 1);
    const std::lock_guard lock(mutex);
    CHECK(phases.size() == count);
}

TEST_CASE("committed voice text invalidates feedback from an in-flight preview",
          "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    std::promise<void> entered;
    std::promise<void> finishPreview;
    auto inPreview = entered.get_future();
    auto finish = finishPreview.get_future();
    int judgments = 0;
    judge->set_handler([&](const Json&, const Json&) -> std::optional<Json> {
        if (++judgments == 1) {
            entered.set_value();
            if (finish.wait_for(2s) != std::future_status::ready) {
                return std::nullopt;
            }
        }
        return fv::llm::FakeJudge::choice_answer("set_topic", 0.9);
    });
    auto state = store();
    std::vector<std::string> phases;
    std::promise<void> saved;
    auto committedDone = saved.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           phases.push_back(event.at("phase").get<std::string>());
                                           if (phases.back() == "saved") {
                                               saved.set_value();
                                           }
                                       }});
    listener.observe_preview(row("", 5, 6, "Assistant, next topic: Draft"));
    REQUIRE(inPreview.wait_for(2s) == std::future_status::ready);
    listener.observe({row("sg_committed", 5, 6, "Assistant, next topic: Final")});
    finishPreview.set_value();
    REQUIRE(committedDone.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    // The preview's "heard", then the committed command's; never the preview's "recognized".
    CHECK((phases == std::vector<std::string>{"heard", "heard", "working", "saved"}));
}

TEST_CASE("split voice wake and instruction cite both committed transcript rows",
          "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    auto state = store();
    std::promise<void> saved;
    auto done = saved.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "saved") {
                                               saved.set_value();
                                           }
                                       }});
    listener.observe(
        {row("sg_wake", 1, 2, "Assistant"), row("sg_order", 3, 4, "Next topic is Launch")});
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    // build_ops cites the command row alone for a topic (audit A29-27).
    const auto topic = state->snapshot().at("topic");
    CHECK(topic.at("current") == "Launch");
    CHECK(topic.at("history").back().at("evidence") == Json::array({"sg_order"}));
    CHECK(judge->calls().size() == 1);

    // from tests/test_voice_commands.py::test_split_wake_and_instruction_preserve_both_evidence_ids
    const auto note =
        voice_command_ops("note_this",
                          Json{{"id", "sg_body"},
                               {"start_s", 0},
                               {"text", "Assistant, Note that the launch is Friday."},
                               {"_content", "the launch is Friday."},
                               {"_evidence_ids", Json::array({"sg_wake", "sg_body"})}},
                          {});
    REQUIRE(note.size() == 1);
    CHECK(note[0].at("text") == "the launch is Friday.");
    CHECK(note[0].at("evidence") == Json::array({"sg_wake", "sg_body"}));
}

TEST_CASE("provisional split wake is classified locally and leaves no durable action",
          "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    auto state = store();
    const auto before = state->snapshot();
    std::promise<void> recognized;
    auto result = recognized.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "recognized") {
                                               recognized.set_value();
                                           }
                                       }});
    listener.observe_preview(row("", 1, 2, "Assistant"));
    listener.observe_preview(row("", 3, 4, "Next topic is Launch"));
    REQUIRE(result.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    CHECK(state->snapshot() == before);
    CHECK(judge->calls().size() == 1);
}

TEST_CASE("spoken recap requests the note scheduler only after commit", "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->push(fv::llm::FakeJudge::choice_answer("recap", 0.9));
    judge->push(fv::llm::FakeJudge::choice_answer("recap", 0.9));
    auto state = store();
    std::promise<void> recognized;
    std::promise<std::string> queued;
    auto preview = recognized.get_future();
    auto recap = queued.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "recognized") {
                                               recognized.set_value();
                                           }
                                       },
                                   .recap =
                                       [&](const std::string& spoken) {
                                           queued.set_value(spoken);
                                       }});
    listener.observe_preview(row("", 5, 6, "Assistant, recap the decision"));
    REQUIRE(preview.wait_for(2s) == std::future_status::ready);
    CHECK(recap.wait_for(0ms) == std::future_status::timeout);
    listener.observe({row("sg_recap", 5, 6, "Assistant, recap the final decision")});
    REQUIRE(recap.wait_for(2s) == std::future_status::ready);
    CHECK(recap.get() == "Assistant, recap the final decision");
}

TEST_CASE("voice wake continuation cannot cross a channel or a long gap", "[voice_commands]") {
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    judge->set_local(true);
    judge->set_handler([](const Json&, const Json&) {
        return std::optional(fv::llm::FakeJudge::choice_answer("set_topic", 0.9));
    });
    auto state = store();
    std::promise<void> saved;
    auto done = saved.get_future();
    VoiceCommandListener listener(state, {"assistant"},
                                  {.judge = [judge] { return judge; },
                                   .allowed = [] { return true; },
                                   .feedback =
                                       [&](const Json& event) {
                                           if (event.at("phase") == "saved" &&
                                               state->snapshot().at("topic").at("current") ==
                                                   "Sentinel") {
                                               saved.set_value();
                                           }
                                       }});
    auto one = row("sg_wake", 1, 2, "Assistant");
    one["channel"] = "mic";
    auto other = row("sg_wrong_channel", 3, 4, "Next topic is Launch");
    other["channel"] = "loopback";
    auto late = row("sg_late", 6, 7, "Next topic is Shipped");
    late["channel"] = "mic";
    listener.observe({one, other, late, row("sg_ok", 15, 16, "Assistant next topic Sentinel")});
    REQUIRE(done.wait_for(2s) == std::future_status::ready);
    listener.shutdown();
    CHECK(judge->calls().size() == 1);
}

} // namespace
