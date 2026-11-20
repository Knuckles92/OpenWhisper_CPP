#include "automation/action_runtime.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "llm/testing/fake_judge.h"
#include "ui_controller/action_wiring.h"
#include <atomic>
#include <catch2/catch_test_macros.hpp>

TEST_CASE("an action shortcut asks before sending its dictated words", "[widget]") {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    std::atomic<int> sends = 0;
    int confirmations = 0;
    std::string disclosed;
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    options.settings["actions"] =
        fv::Json::array({{{"id", "a1"},
                          {"name", "Private site"},
                          {"kind", "open_link"},
                          {"enabled", true},
                          {"hotkey", "f6"},
                          {"params", {{"template", "https://example.test/?q={text}"}}}}});
    options.beforeFeatures = [&](fv::AppContext& context) {
        auto services = std::make_shared<fv::automation::ActionServices>();
        services->open_uri = [&](std::string_view) {
            ++sends;
        };
        context.provide<fv::automation::ActionServices>(services);
        auto consent = std::make_shared<fv::ActionConsent>();
        consent->confirm = [&](const auto& prompt, const auto&) {
            ++confirmations;
            disclosed = prompt.destination + "\n" + prompt.material;
            return false;
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
    double fake_time = fv::hotkeys::monotonic_clock()();
    auto press = [&] {
        fv::hotkeys::KeyEvent event;
        event.name = "f6";
        event.down = true;
        event.t = fake_time;
        fake_time += 1.0;
        rig.hook().send(event);
        event.down = false;
        rig.hook().send(event);
    };
    press();
    REQUIRE(fvtest::waitUntil([&] { return rig.runtime().is_recording(); }));
    REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    press();
    REQUIRE(fvtest::waitUntil([&] { return confirmations == 1; }, std::chrono::seconds(30)));
    CHECK(disclosed.find("example.test") != std::string::npos);
    CHECK(disclosed.find("Hello from FioriVoce") != std::string::npos);
    CHECK(sends == 0);
}

TEST_CASE("declining route by voice never asks the judge or pastes", "[widget]") {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    auto provider = std::make_shared<fv::llm::FakeJudgeProvider>();
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    provider->set_judge(judge);
    std::atomic<int> confirmations = 0;
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    options.settings["voice_routing_enabled"] = true;
    options.settings["typesafe_enabled"] = true; // Route by voice must be ready to record
    options.settings["hotkeys"] = {{"route_record", "f7"}};
    options.settings["actions"] = fv::Json::array(
        {{{"id", "a1"}, {"name", "Home"}, {"kind", "clipboard"}, {"enabled", true}}});
    options.beforeFeatures = [&](fv::AppContext& context) {
        context.provide<fv::llm::JudgeProvider>(provider);
        auto consent = std::make_shared<fv::ActionConsent>();
        consent->confirm = [&](const auto&, const auto&) {
            ++confirmations;
            return false;
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
    auto press = [&] {
        fv::hotkeys::KeyEvent event;
        event.name = "f7";
        event.down = true;
        event.t = fv::hotkeys::monotonic_clock()();
        rig.hook().send(event);
        event.down = false;
        rig.hook().send(event);
    };
    press();
    REQUIRE(fvtest::waitUntil([&] { return rig.runtime().is_recording(); }));
    REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    press();
    REQUIRE(fvtest::waitUntil([&] { return confirmations.load() == 1; }, std::chrono::seconds(30)));
    REQUIRE(fvtest::waitUntil([&] { return !rig.runtime().has_active_job(); },
                              std::chrono::seconds(30)));
    CHECK(judge->calls().empty());
    CHECK(rig.keys().sent().empty());
    CHECK(http.requests().empty());
}

TEST_CASE("a canceled route outlives the controller without touching its UI", "[widget]") {
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials(std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity());
    auto provider = std::make_shared<fv::llm::FakeJudgeProvider>();
    auto judge = std::make_shared<fv::llm::FakeJudge>();
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    judge->set_handler([gate](const auto&, const auto&) -> std::optional<fv::Json> {
        gate.wait();
        return std::nullopt;
    });
    provider->set_judge(judge);
    fvtest::RigOptions options;
    options.http = &http;
    options.credentials = &credentials;
    options.settings["voice_routing_enabled"] = true;
    options.settings["typesafe_enabled"] = true; // Route by voice must be ready to record
    options.settings["hotkeys"] = {{"route_record", "f7"}};
    options.settings["actions"] = fv::Json::array(
        {{{"id", "a1"}, {"name", "Home"}, {"kind", "clipboard"}, {"enabled", true}}});
    options.beforeFeatures = [&](fv::AppContext& context) {
        context.provide<fv::llm::JudgeProvider>(provider);
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
    {
        fvtest::ControllerRig rig(options);
        REQUIRE(rig.startEngine());
        auto press = [&] {
            fv::hotkeys::KeyEvent event;
            event.name = "f7";
            event.down = true;
            event.t = fv::hotkeys::monotonic_clock()();
            rig.hook().send(event);
            event.down = false;
            rig.hook().send(event);
        };
        press();
        REQUIRE(fvtest::waitUntil([&] { return rig.runtime().is_recording(); }));
        REQUIRE(fvtest::waitUntil([&] { return !rig.audio().streams().empty(); }));
        REQUIRE(rig.speak(1.0) > 0);
        press();
        REQUIRE(
            fvtest::waitUntil([&] { return !judge->calls().empty(); }, std::chrono::seconds(30)));
        rig.runtime().begin_shutdown();
        REQUIRE(fvtest::waitUntil([&] { return !rig.runtime().has_active_job(); },
                                  std::chrono::seconds(5)));
        CHECK(rig.keys().sent().empty());
    }
    release->set_value();
}
