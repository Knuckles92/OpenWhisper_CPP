#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "dictation_support.h"
#include "hotkeys/key_names.h"

// The saved actions' shortcut family and the Route by voice shortcut of the hotkey runtime
// (services/runtime/hotkeys.py refresh_profile_hotkeys and setup_hotkeys' on_route_record).
// OpenWhisper has no test of its own for either; these follow the profile and transform family
// tests of test_hotkey_runtime_families.cpp.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::Json;

namespace {

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

ActionShortcut action(std::string id, std::string name, std::string hotkey, bool enabled = true) {
    return ActionShortcut{std::move(id), std::move(name), std::move(hotkey), enabled};
}

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

void inline_presses(fv::hotkeys::HotkeyManager& manager) {
    manager.set_spawner([](std::function<void()> call) { call(); });
}

constexpr auto kChord = fv::hotkeys::kCtrl | fv::hotkeys::kAlt;

} // namespace

TEST_CASE("action shortcuts register beside profiles and transforms and skip conflicts",
          "[hotkey_runtime_actions]") {
    Rig rig({{"record_toggle", "ctrl+alt+r"}, {"cancel", "esc"}});
    inline_presses(*rig.manager);
    rig.settings.update(
        Json{{"transcript_cleanup_profiles", Json::array({profile("ticket", "Ticket", "ctrl+alt+t"),
                                                          profile("memo", "Memo", "ctrl+alt+o")})},
             {"text_transforms", Json::array({transform("polish", "Polish", "ctrl+alt+p")})}});
    rig.runtime->set_action_hotkeys([](const Json&) {
        return std::vector<ActionShortcut>{
            action("a1", "Ask Claude", "ctrl+alt+a"),
            // Disabled: it registers nothing, but the memo profile still may not take its keys.
            action("a2", "Old one", "ctrl+alt+o", false),
            action("a3", "Clash", "alt+ctrl+R"),
            action("a4", "Ticket twin", "ctrl+alt+t"),
            action("a5", "Plain", ""),
        };
    });
    Emitted<std::string> actions(rig.runtime->action_record_requested, rig.inline_ui);

    rig.runtime->refresh_profile_hotkeys();

    CHECK(family(*rig.manager, "action") ==
          std::map<std::string, std::string>{{"a1", "ctrl+alt+a"}});
    // Each side of a collision is skipped rather than letting whichever matches first win.
    CHECK(family(*rig.manager, "profile").empty());
    CHECK(family(*rig.manager, "transform") ==
          std::map<std::string, std::string>{{"polish", "ctrl+alt+p"}});
    CHECK_FALSE(rig.hook->send(key_event(true, "a", false, kChord)));
    rig.hook->send(key_event(false, "a", false, kChord));
    CHECK(actions.values() == std::vector<std::string>{"a1"});
}

TEST_CASE("an action list that cannot be read keeps the other shortcuts",
          "[hotkey_runtime_actions]") {
    Rig rig({});
    rig.settings.update(Json{
        {"transcript_cleanup_profiles", Json::array({profile("ticket", "Ticket", "ctrl+alt+t")})}});
    rig.runtime->set_action_hotkeys(
        [](const Json&) -> std::vector<ActionShortcut> { throw std::runtime_error("1 / 0"); });

    rig.runtime->refresh_profile_hotkeys();

    CHECK(family(*rig.manager, "action").empty());
    CHECK(family(*rig.manager, "profile") ==
          std::map<std::string, std::string>{{"ticket", "ctrl+alt+t"}});
}

TEST_CASE("without saved actions no action shortcut is registered", "[hotkey_runtime_actions]") {
    Rig rig({});
    rig.runtime->refresh_profile_hotkeys();
    CHECK(family(*rig.manager, "action").empty());
}

TEST_CASE("the route by voice shortcut asks for a routed recording", "[hotkey_runtime_actions]") {
    Rig rig({});
    rig.settings.update(Json{{"hotkeys", Json{{"route_record", "ctrl+alt+v"}}}});
    std::atomic<int> asked{0};
    const auto link = rig.runtime->route_record_requested.connect(rig.inline_ui, [&] { ++asked; });
    rig.runtime->setup_hotkeys();
    inline_presses(*rig.manager);

    CHECK_FALSE(rig.hook->send(key_event(true, "v", false, kChord)));
    rig.hook->send(key_event(false, "v", false, kChord));

    CHECK(asked.load() == 1);
}
