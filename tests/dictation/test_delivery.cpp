#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "dictation/delivery.h"
#include "dictation/events.h"
#include "focus/testing/fake_clipboard.h"
#include "focus/testing/fake_synthetic_keys.h"

// The delivery outcomes of transcription.py _apply_clipboard_and_paste, and the event payloads of
// services/automation/catalog.py the runtime publishes.

using namespace fv::dictation;

namespace {

struct Delivery {
    fv::focus::FakeClipboard clipboard{"user's own clipboard"};
    fv::focus::FakeSyntheticKeys keys;
    std::vector<std::string> statuses;

    bool apply(const fv::Json& settings, const std::string& text, std::string_view suffix = {},
               bool force = false) {
        DeliveryPort port;
        port.clipboard = &clipboard;
        port.keys = &keys;
        return apply_clipboard_and_paste(
            port, settings, text, [this](const std::string& s) { statuses.push_back(s); }, suffix,
            {}, force);
    }
};

} // namespace

TEST_CASE("copy-only delivery says Ready, or that the copy failed", "[dictation][delivery]") {
    const fv::Json copy_only{{"auto_paste", false}, {"copy_clipboard", true}};
    Delivery ok;
    CHECK_FALSE(ok.apply(copy_only, "words", " — a notice"));
    CHECK(ok.statuses == std::vector<std::string>{"Ready — a notice"});
    CHECK(ok.clipboard.text() == "words");
    CHECK(ok.clipboard.discarded_prefetches == 1);

    Delivery failing;
    failing.clipboard.fail_writes = true;
    CHECK_FALSE(failing.apply(copy_only, "words"));
    CHECK(failing.statuses == std::vector<std::string>{"Transcription complete (copy failed)"});

    Delivery neither;
    CHECK_FALSE(neither.apply({{"auto_paste", false}, {"copy_clipboard", false}}, "words"));
    CHECK(neither.statuses == std::vector<std::string>{"Ready"});
    CHECK(neither.clipboard.writes.empty());
    CHECK(neither.clipboard.text() == "user's own clipboard");
}

TEST_CASE("a forced paste pastes with auto-paste off and restores after 250 ms",
          "[dictation][delivery]") {
    Delivery delivery;
    CHECK(delivery.apply({{"auto_paste", false}, {"restore_clipboard", true}}, "words", {}, true));
    CHECK(delivery.keys.sent() == std::vector<std::string>{"paste"});
    CHECK(delivery.statuses == std::vector<std::string>{"Ready (Pasted)"});
    CHECK(delivery.clipboard.restore_delays ==
          std::vector<std::chrono::milliseconds>{std::chrono::milliseconds(250)});
    CHECK(delivery.clipboard.fire_restore() == fv::focus::RestoreOutcome::Restored);
    CHECK(delivery.clipboard.text() == "user's own clipboard");
}

TEST_CASE("event payloads carry what the catalog says and nothing more", "[dictation][events]") {
    DictationJob job;
    job.mode = JobMode::Command;
    job.source = "api";
    job.action_id = "";

    CHECK(events::job_fields(nullptr) == fv::Json{{"job_id", ""},
                                                  {"mode", "file"},
                                                  {"action_id", nullptr},
                                                  {"source", ""},
                                                  {"route", ""}});
    CHECK(events::recording_stopped(&job, 1.23456)["duration_s"] == 1.235);
    CHECK(events::recording_stopped(&job, std::nullopt)["duration_s"].is_null());

    // A command's original is the selection, which events never carry.
    const auto completed = events::transcription_completed(
        &job, "Fixed.", std::string("the selection"),
        fv::Json{{"app_id", "notepad.exe"}, {"app_name", "Notepad"}, {"language", "en"}},
        CleanupInfo{"openrouter", "model", 1.5, "light"}, 0.0);
    CHECK(completed["raw_text"].is_null());
    CHECK(completed["audio_s"].is_null());
    CHECK(completed["language"] == "en");
    CHECK(completed["app"] ==
          fv::Json{{"id", "notepad.exe"}, {"name", "Notepad"}, {"category", ""}});
    CHECK(completed["cleanup"] ==
          fv::Json{{"provider", "openrouter"}, {"model", "model"}, {"level", "light"}});

    DictationJob dictation;
    const auto dictated = events::transcription_completed(
        &dictation, "Fixed.", std::string("um fixed"), fv::Json::object(), std::nullopt, 2.0);
    CHECK(dictated["raw_text"] == "um fixed");
    CHECK(dictated["app"].is_null());
    CHECK(dictated["audio_s"] == 2.0);

    // Only a refusal keeps its message, cut to 500 code points; any other text may name files.
    const std::string long_message(600, 'x');
    const auto refused = events::transcription_failed(&job, "refused", long_message);
    CHECK(refused["message"].get<std::string>().size() == 500);
    CHECK(events::transcription_failed(&job, "failed", "C:\\Users\\me")["message"].is_null());
    CHECK(events::transcription_failed(&job, "exploded")["error"] == "failed");
    CHECK(events::history_saved("abc", "id-1", "dictation") ==
          fv::Json{{"job_id", "abc"}, {"record_id", "id-1"}, {"kind", "dictation"}});
}
