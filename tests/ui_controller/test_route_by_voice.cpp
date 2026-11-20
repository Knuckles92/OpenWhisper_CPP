// Route by voice in the running app (application_controller.py toggle_route_recording and
// services/actions/routing.py decide), with the confirmation FioriVoce asks first: the shortcut
// records only when Route by voice is ready and says what it still needs otherwise; a judge that
// does not answer in time is published as route.decided's fallback and the paste says why.
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "automation/event_bus.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "http/testing/fake_http_client.h"
#include "llm/testing/fake_judge.h"
#include "ui_controller/action_wiring.h"

using namespace std::chrono_literals;
using fvtest::waitUntil;

namespace {

struct RouteRig {
    std::shared_ptr<fv::automation::EventBus> bus = std::make_shared<fv::automation::EventBus>();
    std::shared_ptr<fv::automation::EventBus> previous = fv::automation::set_bus(bus);
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    std::shared_ptr<fv::llm::FakeJudgeProvider> provider =
        std::make_shared<fv::llm::FakeJudgeProvider>();
    std::shared_ptr<fv::llm::FakeJudge> judge = std::make_shared<fv::llm::FakeJudge>();
    std::mutex mutex;
    std::vector<std::string> notices;
    std::unique_ptr<fvtest::ControllerRig> rig;
    fv::Subscription link;

    explicit RouteRig(bool fastJudgments) {
        provider->set_judge(judge);
        fvtest::RigOptions options;
        options.http = &http;
        options.credentials = &credentials;
        options.settings["voice_routing_enabled"] = true;
        options.settings["typesafe_enabled"] = fastJudgments;
        options.settings["hotkeys"] = {{"route_record", "f7"}};
        options.settings["actions"] = fv::Json::array(
            {{{"id", "a1"}, {"name", "Home"}, {"kind", "clipboard"}, {"enabled", true}}});
        options.beforeFeatures = [this](fv::AppContext& context) {
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
        rig = std::make_unique<fvtest::ControllerRig>(options);
        link = rig->runtime().notice_requested.connect(rig->uiExecutor(),
                                                       [this](std::string text, bool) {
                                                           std::lock_guard lock(mutex);
                                                           notices.push_back(std::move(text));
                                                       });
    }
    ~RouteRig() {
        rig.reset();
        fv::automation::set_bus(previous);
        bus->close();
    }
    void press() {
        fv::hotkeys::KeyEvent event;
        event.name = "f7";
        event.down = true;
        event.t = fv::hotkeys::monotonic_clock()();
        rig->hook().send(event);
        event.down = false;
        rig->hook().send(event);
    }
    bool noticed(const std::string& text) {
        std::lock_guard lock(mutex);
        for (const auto& notice : notices) {
            if (notice.starts_with(text)) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("the Route by voice shortcut says what it needs instead of recording", "[widget]") {
    RouteRig made(false);
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    made.press();
    REQUIRE(waitUntil([&] {
        for (const auto& status : rig.statuses()) {
            if (status.starts_with("Route by voice needs Fast judgments. Turn them on in ")) {
                return true;
            }
        }
        return false;
    }));
    waitUntil([] { return false; }, 200ms);
    CHECK_FALSE(rig.runtime().is_recording());
    CHECK(rig.audio().streams().empty());
}

TEST_CASE("a judge that does not answer in time is published and the paste says why", "[widget]") {
    RouteRig made(true);
    auto release = std::make_shared<std::promise<void>>();
    auto gate = release->get_future().share();
    made.judge->set_handler([gate](const auto&, const auto&) -> std::optional<fv::Json> {
        gate.wait();
        return std::nullopt;
    });
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    made.press();
    REQUIRE(waitUntil([&] { return rig.runtime().is_recording(); }));
    REQUIRE(waitUntil([&] { return !rig.audio().streams().empty(); }));
    REQUIRE(rig.speak(1.0) > 0);
    made.press();
    REQUIRE(waitUntil(
        [&] { return !made.bus->recent(10, std::vector<std::string>{"route.decided"}).empty(); },
        30s));
    const auto decided = made.bus->recent(10, std::vector<std::string>{"route.decided"});
    CHECK(decided.back().data.at("fallback") == true);
    CHECK(decided.back().data.at("action_id").is_null());
    CHECK(waitUntil([&] { return made.noticed("Couldn't reach Fast judgments"); }, 30s));
    release->set_value();
}
