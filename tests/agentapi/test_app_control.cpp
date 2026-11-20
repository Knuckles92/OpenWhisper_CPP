// The desktop app's AppControl (tests/test_automation_api_app_control.py): calls hop onto the UI
// executor, refusals say why, and the control never outlives the controller's use. The UI thread
// is the test's own: it runs the UI executor's tasks while the call waits on another thread.
#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "agentapi/app_control.h"
#include "automation/control.h"
#include "automation/permissions.h"
#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "dictation/automation_port.h"
#include "live_support.h"
#include "pyport/json.h"
#include "settings/testing/memory_store.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::test;
using namespace std::chrono_literals;

std::string product() {
    return std::string(fv::identity().product);
}

struct Outcome {
    std::optional<std::string> value;
    std::exception_ptr error;

    fv::automation::ControlError control_error() const {
        REQUIRE(error);
        try {
            std::rethrow_exception(error);
        } catch (const fv::automation::ControlError& refused) {
            return refused;
        } catch (...) {
            // Not a ControlError: an empty code fails every check on it.
        }
        return fv::automation::ControlError("", "");
    }
};

struct Fixture {
    explicit Fixture(Json values = Json::object(), const fv::Clock* time = nullptr)
        : settings(std::move(values)),
          control(host, ui, settings, time != nullptr ? *time : fv::system_clock()) {}

    // fn() on a worker thread while this (the UI) thread runs the UI executor's tasks (unless
    // `pump` is false); `between` runs on this thread between the rounds.
    Outcome in_worker(const std::function<std::string()>& fn, bool pump = true,
                      const std::function<void()>& between = {}) {
        Outcome outcome;
        std::atomic<bool> done{false};
        std::thread worker([&] {
            try {
                outcome.value = fn();
            } catch (...) {
                outcome.error = std::current_exception();
            }
            done = true;
        });
        const auto deadline = std::chrono::steady_clock::now() + 10s;
        while (!done && std::chrono::steady_clock::now() < deadline) {
            if (pump) {
                ui.run_all();
            }
            if (between) {
                between();
            }
            std::this_thread::sleep_for(2ms);
        }
        worker.join();
        return outcome;
    }
    Outcome in_worker_void(const std::function<void()>& fn, bool pump = true,
                           const std::function<void()>& between = {}) {
        return in_worker(
            [&] {
                fn();
                return std::string("None");
            },
            pump, between);
    }

    fv::MemorySettingsStore settings;
    fv::ManualExecutor ui;
    FakeHost host;
    UiAppControl control;
    const std::thread::id ui_thread = std::this_thread::get_id();
};

Json actions(const char* json) {
    return Json{{"actions", Json::parse(json)}};
}

} // namespace

TEST_CASE("test_calls_from_another_thread_run_on_the_qt_thread",
          "[test_automation_api_app_control]") {
    Fixture f;
    const Outcome outcome = f.in_worker([&] { return f.control.run_action("a1", "hello", "api"); });
    CHECK(outcome.value == "job-run");
    const auto calls = f.host.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "run");
    CHECK(calls[0].args == std::vector<std::string>{"a1", "hello", "api"});
    CHECK(calls[0].thread == f.ui_thread);
}

TEST_CASE("test_errors_raised_on_the_qt_thread_reach_the_caller",
          "[test_automation_api_app_control]") {
    Fixture f;
    const Outcome outcome =
        f.in_worker([&] { return f.control.run_action("gone", "hello", "api"); });
    CHECK(outcome.control_error().code() == "not_found");
}

TEST_CASE("the pipeline's refusals reach the caller as the API's",
          "[test_automation_api_app_control]") {
    Fixture f;
    f.host.on_run = [] {
        throw fv::dictation::RunRefused("busy", "Busy now.");
    };
    const Outcome busy = f.in_worker([&] { return f.control.run_action("a1", "hi", "api"); });
    CHECK(busy.control_error().code() == "busy");
    CHECK(busy.control_error().message() == "Busy now.");
    f.host.on_run = [] {
        throw fv::dictation::RunPermissionDenied(
            std::string(fv::automation::permissions::AGENT_PROMPTS));
    };
    const Outcome denied = f.in_worker([&] { return f.control.run_action("a1", "hi", "api"); });
    REQUIRE(denied.error);
    CHECK_THROWS_AS(std::rethrow_exception(denied.error),
                    fv::automation::permissions::PermissionDenied);
}

TEST_CASE("test_a_call_made_on_the_qt_thread_runs_at_once", "[test_automation_api_app_control]") {
    Fixture f;
    f.ui.post([&] { f.control.notify("Done", "Build"); });
    f.ui.run_all();
    const auto calls = f.host.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].args == std::vector<std::string>{"Build: Done"});
}

TEST_CASE("test_an_unresponsive_qt_thread_is_unavailable_and_the_call_never_runs",
          "[test_automation_api_app_control]") {
    fv::FakeClock clock;
    Fixture f(Json::object(), &clock);
    f.host.recording = true;
    const Outcome outcome =
        f.in_worker_void([&] { f.control.stop_recording(); }, false, [&] { clock.advance(100ms); });
    const fv::automation::ControlError error = outcome.control_error();
    CHECK(error.code() == "unavailable");
    CHECK(error.message() == product() + " didn't respond in time. Try again.");
    for (int i = 0; i < 5; ++i) {
        f.ui.run_all();
    }
    CHECK(f.host.calls().empty());
}

TEST_CASE("test_a_call_already_running_is_waited_for_so_the_answer_is_true",
          "[test_automation_api_app_control]") {
    fv::FakeClock clock;
    Fixture f(Json::object(), &clock);
    // The call takes longer than the caller waits for it to be taken.
    f.host.on_run = [&] {
        clock.advance(CALL_TIMEOUT + 400ms);
        std::this_thread::sleep_for(50ms);
    };
    const Outcome outcome = f.in_worker([&] { return f.control.run_action("a1", "hello", "api"); });
    CHECK(outcome.value == "job-run");
}

TEST_CASE("test_a_call_still_running_after_the_grace_is_unavailable",
          "[test_automation_api_app_control]") {
    fv::FakeClock clock;
    Fixture f(Json::object(), &clock);
    std::atomic<bool> answered{false};
    // The call never finishes in time: the clock runs on while it works.
    f.host.on_run = [&] {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (!answered && std::chrono::steady_clock::now() < deadline) {
            clock.advance(100ms);
            std::this_thread::sleep_for(5ms);
        }
    };
    const Outcome outcome = f.in_worker([&] {
        try {
            const std::string job = f.control.run_action("a1", "hello", "api");
            answered = true;
            return job;
        } catch (...) {
            answered = true;
            throw;
        }
    });
    const fv::automation::ControlError error = outcome.control_error();
    CHECK(error.code() == "unavailable");
    CHECK(error.message() == product() + " is still working on that. Check its state in a moment.");
}

TEST_CASE("test_start_returns_the_new_job", "[test_automation_api_app_control]") {
    Fixture f;
    const Outcome outcome =
        f.in_worker([&] { return f.control.start_recording(std::nullopt, "api"); });
    CHECK(outcome.value == "job-new");
    const auto calls = f.host.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].name == "start");
    CHECK(calls[0].args == std::vector<std::string>{"api"});
    CHECK(calls[0].thread == f.ui_thread);
}

TEST_CASE("test_start_refusals_say_why", "[test_automation_api_app_control]") {
    struct Case {
        std::string id;
        std::function<void(FakeHost&)> setup;
        std::string action_id, code, message;
    };
    for (const Case& c :
         std::vector<Case>{{"recording", [](FakeHost& h) { h.recording = true; }, "", "busy",
                            product() + " is already recording."},
                           {"transcribing", [](FakeHost& h) { h.busy = true; }, "", "busy",
                            product() + " is busy with another recording."},
                           {"meeting", [](FakeHost& h) { h.meeting = true; }, "", "busy",
                            "Meeting Mode is active; end the meeting first."},
                           {"missing-action", [](FakeHost&) {}, "missing", "not_found",
                            "No enabled action has that id."},
                           {"disabled-action", [](FakeHost&) {}, "off", "not_found",
                            "No enabled action has that id."}}) {
        DYNAMIC_SECTION(c.id) {
            Fixture f(actions(
                R"([{"id": "off", "name": "Off", "kind": "clipboard", "enabled": false}])"));
            c.setup(f.host);
            const Outcome outcome = f.in_worker([&] {
                return f.control.start_recording(
                    c.action_id.empty() ? std::nullopt : std::optional(c.action_id), "api");
            });
            CHECK(outcome.control_error().code() == c.code);
            CHECK(outcome.control_error().message() == c.message);
            CHECK(f.host.calls().empty());
        }
    }
}

TEST_CASE("test_a_start_the_app_refuses_reports_what_it_told_the_user",
          "[test_automation_api_app_control]") {
    Fixture f;
    f.host.refusal = "Speech engine is still loading";
    const Outcome outcome =
        f.in_worker([&] { return f.control.start_recording(std::nullopt, "api"); });
    CHECK(outcome.control_error().code() == "refused");
    CHECK(outcome.control_error().message() == "Speech engine is still loading");
    // Nothing stays connected to the controller once the start is over.
    CHECK(f.host.status_update.subscriber_count() == 0);
}

TEST_CASE("a start refused without a word says it could not start",
          "[test_automation_api_app_control]") {
    Fixture f;
    // A host that refuses without saying anything.
    class Quiet final : public ControlHost {
    public:
        fv::automation::LiveState state() override { return {}; }
        std::optional<std::string> start_recording(std::optional<std::string>,
                                                   std::string) override {
            return std::nullopt;
        }
        bool stop_recording() override { return false; }
        bool cancel() override { return false; }
        std::string run_action_text(std::string, std::string, std::string) override { return ""; }
        void notify(std::string) override {}
        fv::Signal<std::string>& status_updates() override { return updates; }
        fv::Signal<std::string> updates;
    } quiet;
    UiAppControl control(quiet, f.ui, f.settings);
    const Outcome outcome =
        f.in_worker([&] { return control.start_recording(std::nullopt, "api"); });
    CHECK(outcome.control_error().code() == "refused");
    CHECK(outcome.control_error().message() == product() + " couldn't start recording.");
}

TEST_CASE("test_stop_and_cancel_need_something_to_act_on", "[test_automation_api_app_control]") {
    Fixture f;
    CHECK(f.in_worker_void([&] { f.control.stop_recording(); }).control_error().code() ==
          "refused");
    CHECK(f.in_worker_void([&] { f.control.cancel(); }).control_error().code() == "refused");
    CHECK(f.host.calls().empty());

    f.host.recording = true;
    CHECK(f.in_worker_void([&] { f.control.stop_recording(); }).value == "None");
    CHECK(f.in_worker_void([&] { f.control.cancel(); }).value == "None");
    f.host.recording = false;
    f.host.busy = true;
    CHECK(f.in_worker_void([&] { f.control.cancel(); }).value == "None");
    // Only the recording or job: never the user's Cancel, which stops their actions.
    CHECK(f.host.call_names() == std::vector<std::string>{"stop", "cancel", "cancel"});
    for (const auto& call : f.host.calls()) {
        CHECK(call.thread == f.ui_thread);
    }

    // A job already delivered is only being saved: nothing is left to cancel, and the actions it
    // ran are the user's.
    f.host.saving = true;
    const Outcome refused = f.in_worker_void([&] { f.control.cancel(); });
    CHECK(refused.control_error().code() == "refused");
    CHECK(refused.control_error().message() == "Nothing is recording or transcribing.");
    CHECK(f.host.calls().size() == 3);
}

TEST_CASE("test_notices_are_one_line", "[test_automation_api_app_control]") {
    Fixture f;
    CHECK(f.in_worker_void([&] { f.control.notify("Tests\n passed", " CI "); }).value == "None");
    CHECK(f.in_worker_void([&] { f.control.notify("Plain"); }).value == "None");
    const auto calls = f.host.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[0].args == std::vector<std::string>{"CI: Tests passed"});
    CHECK(calls[1].args == std::vector<std::string>{"Plain"});
    const Outcome blank = f.in_worker_void([&] { f.control.notify(" \t "); });
    CHECK(blank.control_error().code() == "refused");
    CHECK(blank.control_error().message() == "There's nothing to show.");
}

TEST_CASE("test_state_is_read_without_waiting_for_the_qt_thread",
          "[test_automation_api_app_control]") {
    Fixture f;
    const auto state = [&] {
        fv::automation::LiveState out;
        std::thread reader([&] { out = f.control.state(); });
        reader.join();
        return out;
    };
    CHECK(state() == fv::automation::LiveState{false, false, false, "", "", "", "9.9.9"});

    f.host.job = FakeHost::Job{"job-9", "action", "a1"};
    f.host.recording = true;
    f.host.meeting = true;
    CHECK(state() ==
          fv::automation::LiveState{true, false, true, "action", "a1", "job-9", "9.9.9"});

    // After the stop, the worker slot holds the job as it is now: a routed dictation is its
    // action's job, while the recording's job is unchanged.
    f.host.recording = false;
    f.host.job = FakeHost::Job{"job-10", "dictation", ""};
    f.host.active_job = FakeHost::Job{"job-10", "action", "a1"};
    f.host.busy = true;
    const fv::automation::LiveState busy = state();
    CHECK_FALSE(busy.recording);
    CHECK(busy.busy);
    CHECK(busy.mode == "action");
    CHECK(busy.action_id == "a1");
    CHECK(busy.job_id == "job-10");
}

TEST_CASE("test_listed_actions_are_the_enabled_ones_without_private_settings",
          "[test_automation_api_app_control]") {
    Fixture f(actions(R"([
        {"id": "a1", "name": "Hook", "kind": "webhook", "params": {"url": "https://secret.example/hook"}},
        {"id": "a2", "name": "Off", "kind": "clipboard", "enabled": false}])"));
    Json listed;
    std::thread reader([&] { listed = f.control.list_actions(); });
    reader.join();
    REQUIRE(listed.size() == 1);
    CHECK(listed[0].at("id") == "a1");
    CHECK(fv::py::dumps(listed).find("secret") == std::string::npos);
}

TEST_CASE("test_the_control_never_keeps_the_controller_alive",
          "[test_automation_api_app_control]") {
    Fixture f;
    CHECK(f.in_worker([&] { return f.control.run_action("a1", "hi", "api"); }).value == "job-run");
    f.host.refusal = "No";
    CHECK(f.in_worker([&] { return f.control.start_recording(std::nullopt, "api"); })
              .control_error()
              .code() == "refused");
    // The controller goes away: the control forgets it, and every call is unavailable.
    f.control.release();
    const std::size_t before = f.host.calls().size();
    const std::vector<std::function<std::string()>> calls = {
        [&] {
            f.control.state();
            return std::string();
        },
        [&] { return f.control.run_action("a1", "hi", "api"); },
        [&] { return f.control.start_recording(std::nullopt, "api"); },
        [&] {
            f.control.stop_recording();
            return std::string();
        },
        [&] {
            f.control.cancel();
            return std::string();
        },
        [&] {
            f.control.notify("hi");
            return std::string();
        }};
    for (const auto& call : calls) {
        const fv::automation::ControlError error = f.in_worker(call).control_error();
        CHECK(error.code() == "unavailable");
        CHECK(error.message() == product() + " isn't ready for this yet.");
    }
    CHECK(f.host.calls().size() == before);
}

TEST_CASE("calls from any thread run on the UI executor", "[test_automation_api_app_control]") {
    // test_a_relay_first_needed_off_the_qt_thread_moves_onto_it and
    // test_one_relay_serves_every_call_from_the_qt_thread: there is no relay object to create or
    // move; every call made off the UI thread is a task of the UI executor.
    Fixture f;
    for (int i = 0; i < 3; ++i) {
        CHECK(f.in_worker([&] { return f.control.run_action("a1", "hi", "api"); }).value ==
              "job-run");
    }
    CHECK(f.in_worker_void([&] { f.control.notify("hi"); }).value == "None");
    const auto calls = f.host.calls();
    REQUIRE(calls.size() == 4);
    for (const auto& call : calls) {
        CHECK(call.thread == f.ui_thread);
    }
    CHECK(f.ui.queued() == 0);
}
