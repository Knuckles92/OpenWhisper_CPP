// The ClipboardPort contract's fake, which the dictation core's tests use until fv_clipboard's
// TemporaryClipboard implements the port.

#include <catch2/catch_test_macros.hpp>

#include "base/testing/manual_executor.h"
#include "focus/testing/fake_clipboard.h"

using namespace fv;
using namespace fv::focus;
using namespace std::chrono_literals;

TEST_CASE("the fake clipboard stages and restores the user's text", "[clipboard_port]") {
    FakeClipboard clipboard("user text");

    const auto staged = clipboard.stage_text("dictated", std::string("<b>dictated</b>"));
    REQUIRE(staged.written);
    REQUIRE(staged.lease);
    CHECK(staged.lease->html == "<b>dictated</b>");
    CHECK(clipboard.text() == "dictated");
    CHECK(clipboard.schedule_restore(*staged.lease, CLIPBOARD_RESTORE_DELAY));
    CHECK(clipboard.fire_restore() == RestoreOutcome::Restored);
    CHECK(clipboard.text() == "user text");
    CHECK(clipboard.restore_delays == std::vector<std::chrono::milliseconds>{250ms});
}

TEST_CASE("the fake clipboard leaves a copy made after the stage alone", "[clipboard_port]") {
    FakeClipboard clipboard("user text");
    const auto staged = clipboard.stage_text("dictated", std::nullopt);
    REQUIRE(staged.lease);

    clipboard.external_copy("copied later");

    CHECK_FALSE(clipboard.schedule_restore(*staged.lease, CLIPBOARD_RESTORE_DELAY));
    CHECK(clipboard.restore_now(*staged.lease) == RestoreOutcome::Skipped);
    CHECK(clipboard.text() == "copied later");
}

TEST_CASE("the fake clipboard keeps no lease for a blank clipboard or without restore",
          "[clipboard_port]") {
    FakeClipboard blank(" \n");
    const auto first = blank.stage_text("dictated", std::nullopt);
    CHECK(first.written);
    CHECK_FALSE(first.lease);

    FakeClipboard keep("user text");
    const auto second = keep.stage_text("dictated", std::nullopt, false);
    CHECK(second.written);
    CHECK_FALSE(second.lease);
    CHECK(keep.text() == "dictated");

    keep.fail_writes = true;
    CHECK_FALSE(keep.stage_text("again", std::nullopt).written);
    CHECK_FALSE(keep.write_text("plain"));
}

TEST_CASE("the fake clipboard commits, writes, prefetches and captures", "[clipboard_port]") {
    FakeClipboard clipboard("user text");
    const auto staged = clipboard.stage_text("dictated", std::string("<i>d</i>"));
    REQUIRE(staged.lease);
    CHECK(clipboard.commit_text(*staged.lease, "kept"));
    CHECK(clipboard.text() == "kept");
    CHECK(clipboard.html() == "<i>d</i>");
    CHECK_FALSE(clipboard.restore_pending());

    clipboard.request_prefetch();
    clipboard.discard_prefetch();
    CHECK(clipboard.prefetches == std::vector<std::chrono::milliseconds>{CLIPBOARD_PREFETCH_DELAY});
    CHECK(clipboard.discarded_prefetches == 1);

    clipboard.selection_answers = {std::string("selected words"), std::nullopt};
    int copies = 0;
    std::vector<std::optional<std::string>> answers;
    const auto collect = [&](std::optional<std::string> text) {
        answers.push_back(text);
    };
    clipboard.capture_selection([&] { ++copies; }, collect, SELECTION_TIMEOUT);
    clipboard.capture_selection([&] { ++copies; }, collect, SELECTION_TIMEOUT);
    clipboard.capture_selection([&] { ++copies; }, collect, SELECTION_TIMEOUT);
    CHECK(answers == std::vector<std::optional<std::string>>{std::string("selected words"),
                                                             std::nullopt, std::string()});
    CHECK(copies == 2);

    CHECK(clipboard.write_files(ClipboardFiles{.paths = {"a.wav"}, .text = "a.wav"}));
    CHECK(clipboard.text() == "a.wav");
    CHECK(clipboard.file_writes.size() == 1);
}

TEST_CASE("restore failures reach subscribers on their executor", "[clipboard_port]") {
    FakeClipboard clipboard;
    ManualExecutor ui;
    std::vector<std::string> messages;
    const auto subscription = clipboard.restore_failed.connect(
        ui, [&](std::string message) { messages.push_back(message); });

    clipboard.restore_failed.emit("Ready (Pasted; clipboard restore failed)");
    CHECK(messages.empty());
    ui.run_all();
    CHECK(messages == std::vector<std::string>{"Ready (Pasted; clipboard restore failed)"});
}
