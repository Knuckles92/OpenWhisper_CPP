// a32 SP-04: an action's answer reaches the Scratchpad through the app's own host
// (action_wiring.cpp Host::show_in_scratchpad -> showScratchpadText; scratchpad.py show_text):
// the Scratchpad is shown without taking focus, and the answer is added after the notes already
// there, on a block of its own, never replacing them.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <memory>
#include <string>

#include <QPlainTextEdit>

#include "agents/agent_actions.h"
#include "automation/action_runtime.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "ui_controller/action_wiring.h"
#include "ui_record/scratchpad.h"

TEST_CASE("a32 SP-04: an agent action's answer is added to the shown Scratchpad", "[widget]") {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    std::atomic<int> asks = 0;
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    options.settings["actions"] = fv::Json::array(
        {{{"id", "q1"},
          {"name", "Quick answer"},
          {"kind", "agent"},
          {"enabled", true},
          {"hotkey", "f6"},
          {"params", {{"agent", "claude_code"}, {"mode", "ask"}, {"output", "scratchpad"}}}}});
    options.beforeFeatures = [&](fv::AppContext& context) {
        auto services = std::make_shared<fv::automation::ActionServices>();
        services->ask = [&](std::string_view, std::string_view, std::string_view,
                            const fv::CancelToken&) {
            ++asks;
            fv::agents::AskResult result;
            result.ok = true;
            result.answer = "Forty-two.";
            result.message = "Claude Code answered";
            return result;
        };
        context.provide<fv::automation::ActionServices>(services);
        auto consent = std::make_shared<fv::ActionConsent>();
        consent->confirm = [](const auto&, const auto&) {
            return true;
        };
        context.provide<fv::ActionConsent>(consent);
    };
    options.platform = [](fv::ControllerPlatform& platform) {
        platform.clipboardBackend = [] {
            return std::make_unique<fv::clipboard::FakeClipboardBackend>();
        };
    };
    fvtest::ControllerRig rig(options);
    REQUIRE(rig.startEngine());

    // Notes the user already has, in a Scratchpad that is not shown.
    auto& service = rig.context().service<fv::ScratchpadService>();
    fv::ScratchpadWindow& pad = service.ensureWindow();
    pad.ensureLoaded();
    pad.editor()->setPlainText(QStringLiteral("Earlier note."));
    pad.hide();
    REQUIRE_FALSE(pad.isVisible());

    double fakeTime = fv::hotkeys::monotonic_clock()();
    auto press = [&] {
        fv::hotkeys::KeyEvent event;
        event.name = "f6";
        event.down = true;
        event.t = fakeTime;
        fakeTime += 1.0;
        rig.hook().send(event);
        event.down = false;
        rig.hook().send(event);
    };
    press();
    REQUIRE(fvtest::waitUntil([&] { return rig.runtime().is_recording(); }));
    REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    press();

    REQUIRE(fvtest::waitUntil([&] { return pad.text().contains(QStringLiteral("Forty-two.")); },
                              std::chrono::seconds(30)));
    CHECK(asks == 1);
    CHECK(service.window() == &pad);
    CHECK(pad.isVisible());
    CHECK(pad.text() == QStringLiteral("Earlier note.\n\nForty-two."));
    // Nothing was pasted into the user's app.
    CHECK(rig.keys().sent().empty());
    pad.hide();
}
