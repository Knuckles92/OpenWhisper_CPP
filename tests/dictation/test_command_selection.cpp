#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <chrono>
#include <format>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/identity.h"
#include "command_support.h"
#include "dictation/command_selection.h"
#include "dictation/rewrite_jobs.h"
#include "focus/focus_capture.h"
#include "focus/foreground.h"
#include "focus/testing/fake_platform.h"
#include "focus/testing/fake_synthetic_keys.h"
#include "golden/golden.h"
#include "pyport/text.h"

// Where Command Mode and transforms find the selection: the golden vectors of
// services/runtime/command.py's pure helpers and of the forced-paste texts of
// services/runtime/transcription.py, and the OpenWhisper tests of selections that must never be
// copied (excluded apps, password fields, keys still held, classic consoles).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;
using fv::focus::AppIdentity;
using fv::focus::FocusSnapshot;
using fv::focus::TextContext;
namespace golden = fv::golden;

namespace {

constexpr std::string_view kCommand = "services.runtime.command";
constexpr std::string_view kTranscription = "services.runtime.transcription";

std::optional<AppIdentity> identity_of(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    AppIdentity identity;
    identity.app_id = fields.value("app_id", "");
    identity.name = fields.value("name", "");
    if (fields.contains("pid") && !fields.at("pid").is_null()) {
        identity.pid = fields.at("pid").get<std::uint32_t>();
    }
    identity.window = fields.value("window", "");
    identity.title_hint = fields.value("title_hint", "");
    return identity;
}

std::optional<TextContext> context_of(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    TextContext context;
    context.before = fields.value("before", "");
    context.selected = fields.value("selected", "");
    context.after = fields.value("after", "");
    context.caret_known = fields.value("caret_known", false);
    context.selection_known = fields.value("selection_known", false);
    context.source = fields.value("source", "");
    context.blocked = fields.value("blocked", false);
    return context;
}

std::optional<FocusSnapshot> snapshot_of(const Json& fields) {
    if (fields.is_null()) {
        return std::nullopt;
    }
    return FocusSnapshot{identity_of(fields.value("identity", Json())),
                         context_of(fields.value("text", Json()))};
}

std::optional<fv::dictation::PasteTarget> target_of(const std::string& name) {
    if (name == "same") {
        return PasteTarget::Same;
    }
    if (name == "changed") {
        return PasteTarget::Changed;
    }
    if (name == "unknown") {
        return PasteTarget::Unknown;
    }
    return std::nullopt;
}

// What _read_selection handed to done(), and the copies it asked for.
struct Reading {
    std::vector<SelectionValue> results;
    int copies = 0;
    std::optional<std::string> clipboard = std::string("would be read");

    CopySelection copy() {
        return [this](std::function<void(std::optional<std::string>)> copied) {
            ++copies;
            copied(clipboard);
        };
    }
    std::function<void(SelectionValue)> collect() {
        return [this](SelectionValue value) {
            results.push_back(std::move(value));
        };
    }
};

// The Win32 calls of a classic console window: its pid is its first client process.
class ConsoleDesktop final : public fv::focus::Win32Desktop {
public:
    explicit ConsoleDesktop(std::string exe) : exe_(std::move(exe)) {}
    std::uintptr_t foreground_window() override { return 0x10876; }
    std::uint32_t window_pid(std::uintptr_t) override { return 4242; }
    std::string class_name(std::uintptr_t) override { return "ConsoleWindowClass"; }
    std::string title(std::uintptr_t) override { return ""; }
    std::string image_name(std::uint32_t) override { return exe_; }
    std::optional<std::pair<std::uintptr_t, std::uint32_t>> uwp_app_window(std::uintptr_t,
                                                                           std::uint32_t) override {
        return std::nullopt;
    }

private:
    std::string exe_;
};

// tests/test_personalize_fix_f2_selection_blocked.py h: KeePassXC has focus and is on "Never
// read from"; the capture service runs over the fake desktop.
struct ExcludedRig {
    ExcludedRig() {
        platform = std::make_shared<fv::focus::FakeFocusPlatform>(
            app("keepassxc.exe", "KeePassXC", 21, "0x21"));
        fv::focus::FocusCaptureOptions options;
        options.deadline = std::chrono::milliseconds(1000);
        options.settings = [] {
            return std::make_shared<const Json>(
                Json{{"app_context_excluded_apps", Json::array({"KeePassXC"})}});
        };
        options.metrics = [](const fv::focus::CaptureSample&) {
        };
        service = std::make_shared<fv::focus::FocusCapture>(platform, std::move(options));
        rig.capture.set(service);
        rig.clipboard.set_selection("Bank PIN 4321");
    }
    ~ExcludedRig() { service->shutdown(); }

    CommandRig rig;
    std::shared_ptr<fv::focus::FakeFocusPlatform> platform;
    std::shared_ptr<fv::focus::FocusCapture> service;
};

std::string excluded_message() {
    return std::format("{} doesn't read text in KeePassXC", fv::identity().product);
}

} // namespace

// ---- golden: services.runtime.command ----

TEST_CASE("golden: _looks_like_copied_line", "[dictation][golden]") {
    golden::for_each(kCommand, "_looks_like_copied_line", [](const golden::Case& c) {
        CHECK(looks_like_copied_line(c.arg<std::string>(0)) == c.result<bool>());
    });
}

TEST_CASE("golden: _refusal", "[dictation][golden]") {
    golden::for_each(kCommand, "_refusal", [](const golden::Case& c) {
        const auto refused = refusal(snapshot_of(c.arg<Json>(0)));
        const Json expected = c.result<Json>();
        if (expected.is_null()) {
            CHECK_FALSE(refused.has_value());
            return;
        }
        REQUIRE(refused.has_value());
        CHECK(golden::equal_rebranded(expected.at("message").get<std::string>(), refused->message));
    });
}

TEST_CASE("golden: _known_selection", "[dictation][golden]") {
    golden::for_each(kCommand, "_known_selection", [](const golden::Case& c) {
        CHECK(known_selection(snapshot_of(c.arg<Json>(0))) ==
              c.result<std::optional<std::string>>());
    });
}

TEST_CASE("golden: command constants", "[dictation][golden]") {
    golden::for_each(kCommand, "constants", [](const golden::Case& c) {
        const Json expected = c.result<Json>();
        CHECK(expected.at("NO_PROVIDER_MESSAGE").get<std::string>() == NO_PROVIDER_MESSAGE);
        CHECK(expected.at("KEYS_HELD_MESSAGE").get<std::string>() == KEYS_HELD_MESSAGE);
        CHECK(golden::equal_rebranded(expected.at("PASSWORD_MESSAGE").get<std::string>(),
                                      password_message()));
        CHECK(golden::equal_rebranded(expected.at("CLIPBOARD_HIDDEN_MESSAGE").get<std::string>(),
                                      clipboard_hidden_message()));
        CHECK(expected.at("SELECTION_WAIT_S").get<double>() * 1000.0 ==
              static_cast<double>(SELECTION_WAIT.count()));
    });
}

// ---- golden: services.runtime.transcription (forced paste) ----

TEST_CASE("golden: _copy_rewrite", "[dictation][golden]") {
    golden::for_each(kTranscription, "_copy_rewrite", [](const golden::Case& c) {
        const bool copied = c.arg<bool>(0);
        const auto target = target_of(c.arg<std::string>(1));
        REQUIRE(target.has_value());
        const std::string status = rewrite_copied_status(copied, *target);
        const Json expected = c.result<Json>();
        CHECK(expected.at("statuses") == Json::array({status}));
        CHECK(expected.at("notices") == Json::array({Json::array({status, false})}));
        CHECK(expected.at("copied") == Json::array({"Rewritten."}));
    });
}

TEST_CASE("golden: _notify_forced_outcome", "[dictation][golden]") {
    golden::for_each(kTranscription, "_notify_forced_outcome", [](const golden::Case& c) {
        const auto selection = c.arg<std::optional<std::string>>(0);
        const bool had_selection = selection && !fv::py::strip(*selection).empty();
        const auto notice =
            forced_outcome_notice(c.arg<bool>(1), had_selection, c.arg<std::string>(2));
        Json shown = Json::array();
        if (notice) {
            shown.push_back(Json::array({notice->text, notice->done}));
        }
        CHECK(shown == c.result<Json>());
    });
}

// ---- tests/test_dictation_runtime_exception_handling.py ----

// from
// tests/test_dictation_runtime_exception_handling.py::test_command_resolve_ignores_a_future_resolved_in_between
TEST_CASE("test_command_resolve_ignores_a_future_resolved_in_between",
          "[test_dictation_runtime_exception_handling]") {
    SelectionSource source;
    CHECK(source.resolve(std::string("first")));
    CHECK_FALSE(source.resolve(std::string("second")));
    const auto value = resolved(source.future(), std::chrono::milliseconds::zero());
    REQUIRE(value.has_value());
    CHECK(std::get<std::string>(*value) == "first");
    CHECK(source.done());
}

// from
// tests/test_dictation_runtime_exception_handling.py::test_command_snapshot_is_none_when_not_ready_cancelled_or_failed
TEST_CASE("test_command_snapshot_is_none_when_not_ready_cancelled_or_failed",
          "[test_dictation_runtime_exception_handling]") {
    // A focus capture always resolves and never fails, so "not ready" and "no capture" are the
    // two cases left of Python's three.
    auto pending = std::make_shared<fv::focus::FocusRequest>();
    CHECK_FALSE(resolved(pending, std::chrono::milliseconds(10)).has_value());
    CHECK_FALSE(resolved(std::shared_ptr<fv::focus::FocusRequest>(), std::chrono::milliseconds(10))
                    .has_value());
}

// ---- tests/test_personalize_fix_f2_selection_blocked.py ----

// from
// tests/test_personalize_fix_f2_selection_blocked.py::test_command_mode_never_copies_from_an_excluded_app
TEST_CASE("test_command_mode_never_copies_from_an_excluded_app",
          "[test_personalize_fix_f2_selection_blocked]") {
    const bool app_context = GENERATE(true, false);
    ExcludedRig excluded;
    auto& rig = excluded.rig;
    rig.settings.update(Json{{"app_context_enabled", app_context}});

    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return resolved(job->selection, {}).has_value(); }));

    CHECK(rig.clipboard.captures().empty());
    CHECK(job->selection_text().empty());
    CHECK_THROWS_MATCHES(rig.commands->complete_recording("make it formal", job), CommandRefused,
                         Catch::Matchers::Message(excluded_message()));
    CHECK(rig.model->requests().empty());
}

// from
// tests/test_personalize_fix_f2_selection_blocked.py::test_a_transform_in_an_excluded_app_is_abandoned_unread
TEST_CASE("test_a_transform_in_an_excluded_app_is_abandoned_unread",
          "[test_personalize_fix_f2_selection_blocked]") {
    ExcludedRig excluded;
    auto& rig = excluded.rig;

    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.last_status() == excluded_message(); }));
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.clipboard.captures().empty());
}

// from
// tests/test_personalize_fix_f2_selection_blocked.py::test_a_password_field_is_not_copied_either
TEST_CASE("test_a_password_field_is_not_copied_either",
          "[test_personalize_fix_f2_selection_blocked]") {
    ExcludedRig excluded;
    excluded.platform->set_current(notepad_app());
    TextContext blocked;
    blocked.blocked = true;
    blocked.source = "uia";
    excluded.platform->set_context("notepad.exe", blocked);
    Reading reading;
    fv::focus::FakeSyntheticKeys keys;

    read_selection(excluded.service->request(false, true), keys, reading.copy(), reading.collect());

    REQUIRE(reading.results.size() == 1);
    CHECK(reading.results[0] == SelectionValue(UnreadSelection{password_message()}));
    CHECK(reading.copies == 0);
}

// ---- tests/test_personalize_fix_f2_selection_keys_held.py ----

namespace {

// h: the shortcut's keys stay down; Notepad has focus and its selection is unknown.
struct KeysHeldRig {
    KeysHeldRig() {
        rig.keys.released = false;
        rig.capture.fake(FocusSnapshot{notepad_app(), std::nullopt});
        rig.clipboard.set_selection("would be read");
    }
    CommandRig rig;
};

} // namespace

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_command_mode_refuses_instead_of_copying_with_keys_held
TEST_CASE("test_command_mode_refuses_instead_of_copying_with_keys_held",
          "[test_personalize_fix_f2_selection_keys_held]") {
    KeysHeldRig held;
    auto& rig = held.rig;

    rig.commands->key_pressed(1.0);
    const auto job = rig.recording_job();
    REQUIRE(job);
    rig.commands->key_released(2.0);
    REQUIRE(rig.settle([&] { return resolved(job->selection, {}).has_value(); }));

    CHECK(rig.clipboard.captures().empty());
    CHECK(job->selection_text().empty());
    CHECK_THROWS_MATCHES(rig.commands->complete_recording("translate this into Spanish", job),
                         CommandRefused, Catch::Matchers::Message(std::string(KEYS_HELD_MESSAGE)));
    CHECK(rig.model->requests().empty());
}

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_a_transform_abandons_instead_of_copying_with_keys_held
TEST_CASE("test_a_transform_abandons_instead_of_copying_with_keys_held",
          "[test_personalize_fix_f2_selection_keys_held]") {
    KeysHeldRig held;
    auto& rig = held.rig;

    rig.commands->run_transform("polish");

    REQUIRE(rig.settle([&] { return rig.last_status() == KEYS_HELD_MESSAGE; }));
    CHECK_FALSE(rig.runtime->has_active_job());
    CHECK(rig.clipboard.captures().empty());
}

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_a_command_with_a_known_selection_needs_no_keys_released
TEST_CASE("test_a_command_with_a_known_selection_needs_no_keys_released",
          "[test_personalize_fix_f2_selection_keys_held]") {
    KeysHeldRig held;
    auto& rig = held.rig;

    const RewriteResult result =
        rig.commands->complete_recording("make it formal", command_job("old words"));

    CHECK(result.raw_text == std::optional<std::string>("old words"));
    CHECK(rig.clipboard.captures().empty());
}

// from
// tests/test_personalize_fix_f2_selection_keys_held.py::test_the_reader_thread_never_copies_with_keys_held
TEST_CASE("test_the_reader_thread_never_copies_with_keys_held",
          "[test_personalize_fix_f2_selection_keys_held]") {
    fv::focus::FakeCaptureService service(FocusSnapshot{notepad_app(), std::nullopt});
    fv::focus::FakeSyntheticKeys keys;
    keys.released_by_default = false;
    Reading reading;

    std::thread([&] {
        read_selection(service.request(false, true), keys, reading.copy(), reading.collect());
    }).join();

    REQUIRE(reading.results.size() == 1);
    CHECK(reading.results[0] == SelectionValue(UnreadSelection{std::string(KEYS_HELD_MESSAGE)}));
    CHECK(reading.copies == 0);
}

// ---- tests/test_personalize_fix_f3_console_terminal.py ----

// from
// tests/test_personalize_fix_f3_console_terminal.py::test_command_mode_never_sends_a_copy_to_a_classic_console
TEST_CASE("test_command_mode_never_sends_a_copy_to_a_classic_console",
          "[test_personalize_fix_f3_console_terminal]") {
    ConsoleDesktop desktop("ubuntu.exe");
    const auto identity = fv::focus::foreground_identity(desktop, 1);
    REQUIRE(identity.has_value());
    fv::focus::FakeSyntheticKeys keys;
    Reading reading;

    read_selection(fv::focus::FocusRequest::resolved(FocusSnapshot{identity, std::nullopt}), keys,
                   reading.copy(), reading.collect());

    REQUIRE(reading.results.size() == 1);
    CHECK(reading.results[0] == SelectionValue(std::string()));
    CHECK(reading.copies == 0);
}
