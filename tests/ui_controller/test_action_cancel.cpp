// The user's Cancel through the controller's graph (application_controller.py cancel and
// _cancel_actions): it cancels the recording or transcription in progress, and only that; with
// neither, it stops the actions still running, which then say what they canceled; with nothing at
// all, the status says so. And History's deletes drop the live event replay buffer.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "agents/agent_actions.h"
#include "appcore/cancel_router.h"
#include "automation/action_runtime.h"
#include "automation/catalog.h"
#include "automation/event_bus.h"
#include "clipboard/testing/fake_backend.h"
#include "controller_support.h"
#include "credentials/testing/memory_backend.h"
#include "history/manager.h"
#include "http/testing/fake_http_client.h"
#include "ui_controller/action_wiring.h"

using namespace std::chrono_literals;
using fvtest::waitUntil;

namespace {

// A fresh process-wide bus for one test: the app's events go to it, and the next test gets its own.
struct FreshBus {
    std::shared_ptr<fv::automation::EventBus> bus = std::make_shared<fv::automation::EventBus>();
    std::shared_ptr<fv::automation::EventBus> previous = fv::automation::set_bus(bus);
    ~FreshBus() {
        fv::automation::set_bus(previous);
        bus->close();
    }
    std::vector<fv::automation::Event> events(std::string_view type) const {
        return bus->recent(1000, std::vector<std::string>{std::string(type)});
    }
};

// A Quick answer that thinks until it is canceled.
struct BlockedAsk {
    std::mutex mutex;
    std::vector<std::string> texts;
    std::atomic<bool> canceled = false;

    fv::agents::AskResult operator()(std::string_view, std::string_view text, std::string_view,
                                     const fv::CancelToken& cancel) {
        {
            std::lock_guard lock(mutex);
            texts.emplace_back(text);
        }
        while (!cancel.cancelled()) {
            std::this_thread::sleep_for(5ms);
        }
        canceled = true;
        return {.ok = false, .message = "Canceled", .stopped = true};
    }
    std::size_t calls() {
        std::lock_guard lock(mutex);
        return texts.size();
    }
};

// The controller's graph with the actions wired: a Quick answer action on F6, every action
// approved, and the user's statuses and notices recorded off the UI controller's signal.
struct CancelRig {
    FreshBus bus;
    fv::http::FakeHttpClient http;
    fv::CredentialStore credentials{std::make_shared<fv::MemoryCredentialBackend>(),
                                    fv::identity()};
    std::shared_ptr<BlockedAsk> ask = std::make_shared<BlockedAsk>();
    std::vector<std::string> nothing;
    std::unique_ptr<fvtest::ControllerRig> rig;
    fv::Subscription link;

    CancelRig() {
        fvtest::RigOptions options;
        options.http = &http;
        options.credentials = &credentials;
        options.settings["actions"] =
            fv::Json::array({{{"id", "qa"},
                              {"name", "Quick answer"},
                              {"kind", "agent"},
                              {"enabled", true},
                              {"hotkey", "f6"},
                              {"shaping", "raw"},
                              {"params", {{"agent", "codex"}, {"mode", "ask"}}}}});
        options.beforeFeatures = [this](fv::AppContext& context) {
            auto services = std::make_shared<fv::automation::ActionServices>();
            services->ask = [held = ask](std::string_view agent, std::string_view text,
                                         std::string_view model, const fv::CancelToken& cancel) {
                return (*held)(agent, text, model, cancel);
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
        rig = std::make_unique<fvtest::ControllerRig>(options);
        link = rig->app().nothing_to_cancel.connect(
            rig->uiExecutor(), [this](std::string text) { nothing.push_back(std::move(text)); });
    }
    ~CancelRig() {
        // A Quick answer still thinking ends with the test.
        rig->app().cancel();
        waitUntil([this] { return ask->calls() == 0 || ask->canceled.load(); }, 5s);
    }

    // Records a Quick answer and lets it reach the agent, which keeps thinking.
    void quickAnswerDelivered() {
        REQUIRE(rig->startEngine());
        REQUIRE(rig->app().toggle_action_recording("qa"));
        REQUIRE(waitUntil([&] { return !rig->audio().streams().empty(); }));
        REQUIRE(rig->speak(1.0) > 0);
        REQUIRE(rig->app().toggle_action_recording("qa"));
        REQUIRE(waitUntil([&] { return ask->calls() == 1; }, 30s));
    }
    bool ranCanceled() const {
        for (const auto& event : bus.events(fv::automation::catalog::ACTION_RAN)) {
            if (event.data.value("ok", true) == false &&
                event.data.value("message", std::string()) ==
                    fv::automation::runtime::CANCELED_MESSAGE) {
                return true;
            }
        }
        return false;
    }
};

} // namespace

TEST_CASE("test_cancel_with_nothing_at_all_says_so", "[widget]") {
    CancelRig made;
    auto& rig = *made.rig;
    REQUIRE(rig.startEngine());
    CHECK_FALSE(rig.app().cancel());
    CHECK(waitUntil([&] { return !made.nothing.empty(); }));
    CHECK(made.nothing == std::vector<std::string>{"Nothing to cancel"});
}

TEST_CASE("test_cancel_stops_running_actions_only_with_no_job_to_cancel", "[widget]") {
    const bool recording = GENERATE(true, false);
    INFO((recording ? "a recording to cancel" : "only a Quick answer thinking"));
    CancelRig made;
    auto& rig = *made.rig;
    made.quickAnswerDelivered();
    REQUIRE(waitUntil([&] { return !rig.runtime().has_active_job(); }, 30s));
    if (recording) {
        REQUIRE(rig.app().start_recording());
        REQUIRE(waitUntil([&] { return rig.runtime().is_recording(); }));
    }
    CHECK(rig.app().cancel());
    if (recording) {
        // The recording was canceled; the Quick answer from before keeps going.
        CHECK(waitUntil([&] { return !rig.runtime().is_recording(); }));
        waitUntil([] { return false; }, 200ms);
        CHECK_FALSE(made.ask->canceled.load());
        CHECK_FALSE(made.ranCanceled());
    } else {
        CHECK(waitUntil([&] { return made.ask->canceled.load(); }, 10s));
        CHECK(waitUntil([&] { return made.ranCanceled(); }, 10s));
    }
    CHECK(made.nothing.empty());
}

TEST_CASE("test_the_users_cancel_while_a_delivered_job_is_saved_stops_its_quick_answer",
          "[widget]") {
    CancelRig made;
    auto& rig = *made.rig;
    made.quickAnswerDelivered();
    // Nothing is recording or transcribing: the Cancel reaches the actions.
    CHECK(rig.app().cancel());
    CHECK(waitUntil([&] { return made.ask->canceled.load(); }, 10s));
    CHECK(waitUntil([&] { return made.ranCanceled(); }, 10s));
    CHECK(waitUntil([&] { return !rig.runtime().has_active_job(); }, 30s));
    CHECK(made.bus.events(fv::automation::catalog::RECORDING_CANCELED).empty());
    CHECK(made.bus.events(fv::automation::catalog::TRANSCRIPTION_FAILED).empty());
    CHECK(waitUntil(
        [&] { return !made.bus.events(fv::automation::catalog::HISTORY_SAVED).empty(); }, 10s));
    // The words were saved in History all the same.
    const auto entries =
        fvtest::offGui([&] { return rig.context().service<fv::HistoryManager>().get_history(); });
    REQUIRE(entries.size() == 1);
    CHECK(entries.front().entry_kind == std::optional<std::string>("action"));
}

TEST_CASE("test_cancel_that_reached_actions_leaves_them_to_say_what_it_did", "[widget]") {
    CancelRig made;
    auto& rig = *made.rig;
    made.quickAnswerDelivered();
    CHECK(rig.app().cancel());
    CHECK(waitUntil([&] { return made.ranCanceled(); }, 10s));
    // The router says nothing: the actions report what they canceled themselves.
    CHECK(made.nothing.empty());
}

TEST_CASE("test_a_cancel_from_the_hotkey_thread_reaches_actions_on_the_qt_thread", "[widget]") {
    CancelRig made;
    auto& rig = *made.rig;
    made.quickAnswerDelivered();
    // A step ahead of the actions' that only notes the thread the router runs on.
    // Shared: the router outlives this scope and runs the step again at the rig's end.
    auto threads = std::make_shared<std::vector<std::thread::id>>();
    rig.context().cancel().add(25, [threads] {
        threads->push_back(std::this_thread::get_id());
        return false;
    });
    const auto gui = std::this_thread::get_id();
    // The cancel shortcut ("kp -") pressed on the keyboard hook's own thread.
    std::thread hotkey([&] { rig.pressKeypad("-"); });
    hotkey.join();
    CHECK(waitUntil([&] { return made.ask->canceled.load(); }, 10s));
    REQUIRE(threads->size() == 1);
    CHECK(threads->front() == gui);
}

TEST_CASE("test_deleting_history_drops_the_replay_buffer", "[widget]") {
    const int how = GENERATE(0, 1, 2);
    FreshBus bus;
    fvtest::ControllerRig rig;
    auto& history = rig.context().service<fv::HistoryManager>();
    const auto saved = fvtest::offGui([&] {
        fv::AddEntry entry;
        entry.text = "dictated words";
        entry.model = "parakeet";
        return history.add_entry(entry);
    });
    REQUIRE(saved.has_value());
    bus.bus->publish_event("transcription.completed", fv::Json{{"text", "dictated words"}});
    REQUIRE_FALSE(bus.bus->recent().empty());
    fvtest::offGui([&] {
        if (how == 0) {
            return history.delete_entry(saved->id);
        }
        if (how == 1) {
            history.clear_history();
        } else {
            history.clear_history_and_recordings();
        }
        return true;
    });
    CHECK(bus.bus->recent().empty());
}
