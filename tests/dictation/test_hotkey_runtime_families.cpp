#include <catch2/catch_test_macros.hpp>

#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dictation_support.h"
#include "hotkeys/key_names.h"
#include "text/transforms.h"

// The Command Mode keys and the profile and transform shortcut families of the hotkey runtime
// (tests/test_personalize_s6_runtime.py, tests/test_cleanup_profiles.py).

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

// _Controller's command_key_pressed and command_key_released: what reached it, and on which
// thread.
class RecordingCommandKeys final : public CommandKeys {
public:
    void key_pressed(double at) override { record("command_press", at); }
    void key_released(double at) override { record("command_release", at); }

    std::vector<std::pair<std::string, double>> calls() const {
        std::lock_guard lock(mutex_);
        return calls_;
    }
    std::vector<std::thread::id> threads() const {
        std::lock_guard lock(mutex_);
        return threads_;
    }

private:
    void record(std::string what, double at) {
        std::lock_guard lock(mutex_);
        calls_.emplace_back(std::move(what), at);
        threads_.push_back(std::this_thread::get_id());
    }
    mutable std::mutex mutex_;
    std::vector<std::pair<std::string, double>> calls_;
    std::vector<std::thread::id> threads_;
};

Json profile(std::string id, std::string name, std::string hotkey) {
    return Json{{"id", std::move(id)},
                {"name", std::move(name)},
                {"instructions", "Ticket."},
                {"hotkey", std::move(hotkey)}};
}

Json transform(std::string id, std::string name, std::string hotkey) {
    return Json{{"id", std::move(id)},
                {"name", std::move(name)},
                {"instruction", name + " it."},
                {"hotkey", std::move(hotkey)}};
}

// The items of one family as the manager registered them.
std::map<std::string, std::string> family(const fv::hotkeys::HotkeyManager& manager,
                                          const std::string& name) {
    std::map<std::string, std::string> items;
    for (const auto& [key, hotkey] : manager.dynamic_hotkeys()) {
        if (key.starts_with(name + ":")) {
            items.emplace(key.substr(name.size() + 1), hotkey);
        }
    }
    return items;
}

// Runs the manager's per-press callbacks inline, as the Python tests call them directly.
void inline_presses(fv::hotkeys::HotkeyManager& manager) {
    manager.set_spawner([](std::function<void()> call) { call(); });
}

} // namespace

// from
// tests/test_personalize_s6_runtime.py::test_command_keys_reach_the_controller_on_the_dispatcher_thread
TEST_CASE("test_command_keys_reach_the_controller_on_the_dispatcher_thread",
          "[test_personalize_s6_runtime]") {
    Rig rig;
    RecordingCommandKeys keys;
    rig.runtime->set_command_keys(&keys);

    rig.runtime->queue_command_press(10.0);
    rig.runtime->queue_command_release(10.4);
    rig.drain();
    rig.runtime->set_command_keys(nullptr);

    CHECK(keys.calls() == std::vector<std::pair<std::string, double>>{{"command_press", 10.0},
                                                                      {"command_release", 10.4}});
    const auto threads = keys.threads();
    REQUIRE(threads.size() == 2);
    CHECK(threads[0] == threads[1]);
    CHECK(threads[0] != std::this_thread::get_id());
}

// from
// tests/test_personalize_s6_runtime.py::test_transform_shortcuts_register_beside_profiles_and_skip_conflicts
TEST_CASE("test_transform_shortcuts_register_beside_profiles_and_skip_conflicts",
          "[test_personalize_s6_runtime]") {
    Rig rig({{"record_toggle", "ctrl+alt+r"}, {"cancel", "esc"}});
    inline_presses(*rig.manager);
    rig.settings.update(Json{
        {"transcript_cleanup_profiles", Json::array({profile("ticket", "Ticket", "ctrl+alt+t")})},
        {"text_transforms", Json::array({transform("polish", "Polish", "ctrl+alt+p"),
                                         transform("clash", "Clash", "alt+ctrl+R"),
                                         transform("plain", "Plain", "")})}});
    Emitted<std::string> transforms(rig.runtime->transform_requested, rig.inline_ui);

    rig.runtime->refresh_profile_hotkeys();

    CHECK(family(*rig.manager, "profile") ==
          std::map<std::string, std::string>{{"ticket", "ctrl+alt+t"}});
    CHECK(family(*rig.manager, "transform") ==
          std::map<std::string, std::string>{{"polish", "ctrl+alt+p"}});
    CHECK_FALSE(
        rig.hook->send(key_event(true, "p", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt)));
    rig.hook->send(key_event(false, "p", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt));
    CHECK(transforms.values() == std::vector<std::string>{"polish"});
}

// from
// tests/test_personalize_s6_runtime.py::test_a_broken_transform_library_keeps_profile_shortcuts
TEST_CASE("test_a_broken_transform_library_keeps_profile_shortcuts",
          "[test_personalize_s6_runtime]") {
    Rig rig({});
    rig.settings.update(Json{
        {"transcript_cleanup_profiles", Json::array({profile("ticket", "Ticket", "ctrl+alt+t")})}});
    rig.runtime->set_transform_loader(
        [](const Json&) -> std::vector<fv::text::Transform> { throw std::runtime_error("1 / 0"); });

    rig.runtime->refresh_profile_hotkeys();

    CHECK(family(*rig.manager, "transform").empty());
    CHECK(family(*rig.manager, "profile") ==
          std::map<std::string, std::string>{{"ticket", "ctrl+alt+t"}});
}

// from
// tests/test_personalize_s6_runtime.py::test_transform_shortcut_dispatches_end_to_end_on_windows
TEST_CASE("test_transform_shortcut_dispatches_end_to_end_on_windows",
          "[test_personalize_s6_runtime]") {
    Rig rig;
    rig.settings.update(
        Json{{"text_transforms", Json::array({transform("polish", "Polish", "ctrl+alt+p")})}});
    Emitted<std::string> transforms(rig.runtime->transform_requested, rig.inline_ui);
    rig.runtime->refresh_profile_hotkeys();

    // The press runs on a thread of its own, as on Windows.
    rig.hook->send(key_event(true, "p", false, fv::hotkeys::kCtrl | fv::hotkeys::kAlt));

    CHECK(wait_until([&] { return transforms.values() == std::vector<std::string>{"polish"}; }));
}

// ---- tests/test_cleanup_profiles.py ----

// from
// tests/test_cleanup_profiles.py::test_windows_profile_shortcuts_toggle_once_per_press_and_unregister
TEST_CASE("test_windows_profile_shortcuts_toggle_once_per_press_and_unregister",
          "[test_cleanup_profiles]") {
    Rig rig;
    inline_presses(*rig.manager);
    rig.settings.update(Json{
        {"transcript_cleanup_profiles", Json::array({profile("ticket", "Ticket", "ctrl+alt+t")})}});
    Emitted<std::string> profiles(rig.runtime->profile_record_requested, rig.inline_ui);
    rig.runtime->refresh_profile_hotkeys();
    const auto chord = fv::hotkeys::kCtrl | fv::hotkeys::kAlt;

    CHECK_FALSE(rig.hook->send(key_event(true, "t", false, chord)));
    CHECK(profiles.values() == std::vector<std::string>{"ticket"});
    // Auto-repeat of the held key fires nothing more.
    rig.hook->send(key_event(true, "t", false, chord));
    CHECK_FALSE(rig.hook->send(key_event(false, "t", false, chord)));
    CHECK(profiles.values() == std::vector<std::string>{"ticket"});

    rig.manager->set_program_enabled(false);
    CHECK(rig.hook->send(key_event(true, "t", false, chord)));
    rig.hook->send(key_event(false, "t", false, chord));
    rig.manager->set_program_enabled(true);
    rig.manager->set_capture_suspended(true);
    CHECK(rig.hook->send(key_event(true, "t", false, chord)));
    rig.hook->send(key_event(false, "t", false, chord));
    rig.manager->set_capture_suspended(false);

    rig.settings.update(Json{{"transcript_cleanup_profiles", Json::array()}});
    rig.runtime->refresh_profile_hotkeys();
    CHECK(rig.hook->send(key_event(true, "t", false, chord)));
    CHECK(profiles.values() == std::vector<std::string>{"ticket"});
}
