#include "walkthrough/paste.h"

#include <catch2/catch_test_macros.hpp>

#include "base/testing/manual_executor.h"
#include "focus/testing/fake_clipboard.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "library_support.h"
#include "support.h"

using namespace fv::walkthrough;
using namespace std::chrono_literals;

TEST_CASE("walkthrough paste sends PDF then instructions", "[paste]") {
    test::TempFolder folder;
    test::write_file(folder / "result.pdf", "PDF");
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [target] { return target; },
        [&clipboard](std::string_view marker) {
            return !clipboard.file_writes.empty() && clipboard.file_writes.back().marker == marker;
        },
        [&report](std::string text) { report = std::move(text); });
    paste.paste(result, target);
    paste.paste(result, target);
    REQUIRE(clipboard.file_writes.size() == 1);
    CHECK(keys.sent().size() == 1);
    CHECK(paste.busy());
    ui.advance(800ms);
    CHECK(keys.sent().size() == 2);
    CHECK(clipboard.text().find("Read the attached walkthrough PDF") != std::string::npos);
    CHECK(report == "PDF and instructions pasted; review them before sending");
    CHECK_FALSE(paste.busy());
}

TEST_CASE("walkthrough paste stops if the clipboard changes", "[paste]") {
    test::TempFolder folder;
    test::write_file(folder / "result.pdf", "PDF");
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [target] { return target; }, [](std::string_view) { return false; },
        [&report](std::string text) { report = std::move(text); });
    paste.paste(result, target);
    ui.advance(800ms);
    CHECK(keys.sent().size() == 1);
    CHECK(report == "PDF paste sent; instructions stopped because the clipboard changed");
    CHECK_FALSE(paste.busy());
}

TEST_CASE("canceling a walkthrough paste prevents the second paste", "[paste]") {
    test::TempFolder folder;
    test::write_file(folder / "result.pdf", "PDF");
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    WalkthroughPaste paste(
        ui, clipboard, keys, [target] { return target; }, [](std::string_view) { return true; },
        [](std::string) {});
    paste.paste(result, target);
    paste.cancel();
    ui.advance(800ms);
    CHECK(keys.sent().size() == 1);
    CHECK(clipboard.writes.empty());
}

TEST_CASE("paste preflight refuses an unavailable target, missing PDF, and held modifiers",
          "[paste]") {
    test::TempFolder folder;
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::optional<fv::focus::AppIdentity> current = target;
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [&current] { return current; }, [](std::string_view) { return true; },
        [&report](std::string text) { report = std::move(text); });
    paste.paste(result, std::nullopt);
    CHECK(report == "Walkthrough not pasted: focus changed or the destination is unavailable");
    paste.paste(result, target);
    CHECK(report == "Walkthrough not pasted: the PDF is missing");
    test::write_file(result.pdf, "PDF");
    keys.held = true;
    paste.paste(result, target);
    CHECK(report == "Walkthrough not pasted: modifier keys are held");
    CHECK(keys.sent().empty());
    CHECK(clipboard.file_writes.empty());
    CHECK_FALSE(paste.busy());
}

TEST_CASE("paste requires accessibility permission", "[paste]") {
    test::TempFolder folder;
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [target] { return target; }, [](std::string_view) { return true; },
        [&report](std::string text) { report = std::move(text); }, [] { return false; });
    paste.paste({}, target);
    CHECK(report == "Walkthrough not pasted: enable Accessibility permission to auto-paste");
    CHECK(keys.sent().empty());
}

TEST_CASE("paste stops if focus or modifiers change during PDF delivery", "[paste]") {
    test::TempFolder folder;
    test::write_file(folder / "result.pdf", "PDF");
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::optional<fv::focus::AppIdentity> current = target;
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [&current] { return current; }, [](std::string_view) { return true; },
        [&report](std::string text) { report = std::move(text); });
    paste.paste(result, target);
    current.reset();
    ui.advance(800ms);
    CHECK(report == "PDF paste sent; instructions stopped because focus changed");
    CHECK(keys.sent().size() == 1);
    current = target;
    paste.paste(result, target);
    keys.held = true;
    ui.advance(800ms);
    CHECK(report == "PDF paste sent; instructions stopped because modifier keys are held");
    CHECK(keys.sent().size() == 2);
    CHECK(clipboard.writes.empty());
}

TEST_CASE("failed instruction write does not repeat the PDF paste", "[paste]") {
    test::TempFolder folder;
    test::write_file(folder / "result.pdf", "PDF");
    WalkthroughResult result;
    result.pdf = folder / "result.pdf";
    fv::ManualExecutor ui;
    fv::focus::FakeClipboard clipboard;
    fv::focus::FakeSyntheticKeys keys;
    const fv::focus::AppIdentity target{.app_id = "editor", .pid = 42, .window = "0x42"};
    std::string report;
    WalkthroughPaste paste(
        ui, clipboard, keys, [target] { return target; }, [](std::string_view) { return true; },
        [&report](std::string text) { report = std::move(text); });
    paste.paste(result, target);
    clipboard.fail_writes = true;
    ui.advance(800ms);
    CHECK(keys.sent().size() == 1);
    CHECK(report == "PDF paste sent; couldn't paste instructions: couldn't copy the instructions");
    CHECK_FALSE(paste.busy());
}

TEST_CASE("latest completed walkthrough skips a newer draft", "[paste]") {
    test::Capture capture;
    const auto draft = capture.root / "2026-10-22_14-32-05";
    std::filesystem::create_directories(draft);
    test::write_file(draft / "draft.json", "{}");
    const auto result = latest_result(capture.root);
    REQUIRE(result);
    CHECK(result->folder == capture.folder);
    CHECK(result->pdf == capture.result.pdf);
}

TEST_CASE("latest completed walkthrough propagates a broken manifest", "[paste]") {
    test::TempFolder folder;
    const auto capture = folder / "2026-10-22_14-32-05";
    std::filesystem::create_directories(capture);
    test::write_file(capture / "walkthrough.json", "broken");
    CHECK_THROWS(latest_result(folder.path()));
}
