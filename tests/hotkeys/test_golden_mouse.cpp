#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "base/testing/manual_executor.h"
#include "golden/golden.h"
#include "harness.h"
#include "hotkeys/capture.h"
#include "hotkeys/grammar.h"
#include "hotkeys/mouse_shortcuts.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;
using fv::Json;
namespace golden = fv::golden;

namespace {

std::uint8_t modifier_mask(const Json& names) {
    std::uint8_t mask = 0;
    for (const auto& name : names) {
        const auto text = name.get<std::string>();
        mask |= text == "ctrl" ? kCtrl : 0;
        mask |= text == "alt" ? kAlt : 0;
        mask |= text == "shift" ? kShift : 0;
        mask |= text == "win" ? kWin : 0;
    }
    return mask;
}

// The frozenset Python sorts, as a JSON list.
Json modifier_names(std::uint8_t mask) {
    Json names = Json::array();
    for (const auto& [bit, name] : {std::pair{kAlt, "alt"}, std::pair{kCtrl, "ctrl"},
                                    std::pair{kShift, "shift"}, std::pair{kWin, "win"}}) {
        if ((mask & bit) != 0) {
            names.push_back(name);
        }
    }
    return names;
}

MouseButton button_named(const Json& name) {
    const auto button = mouse_button_from_name(name.get<std::string>());
    if (!button) {
        throw std::invalid_argument("not a side button: " + name.dump());
    }
    return *button;
}

// "record_press@1.0": the name, and the time stamp repr() printed, which round-trips.
bool same_entries(const Json& expected, const std::vector<std::string>& names,
                  const std::vector<std::optional<double>>& stamps) {
    if (expected.size() != names.size()) {
        return false;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
        const auto text = expected[i].get<std::string>();
        const auto at = text.find('@');
        if (at == std::string::npos) {
            if (stamps[i] || text != names[i]) {
                return false;
            }
        } else if (!stamps[i] || text.substr(0, at) != names[i] ||
                   std::stod(text.substr(at + 1)) != *stamps[i]) {
            return false;
        }
    }
    return true;
}

} // namespace

TEST_CASE("services._mouse_hook_win callback traces", "[golden]") {
    golden::for_each("services._mouse_hook_win", "callback_trace", [](const golden::Case& c) {
        std::set<MouseButton> claimed;
        for (const auto& name : c.arg<Json>(0)) {
            claimed.insert(button_named(name));
        }
        std::set<int> pressed;
        bool raising = false;
        Json asked = Json::array();
        Json queued = Json::array();
        MouseButtonFilter filter;
        const MouseButtonFilter::KeyState key_state = [&pressed](int vk) -> short {
            return pressed.contains(vk) ? short{-32768} : short{0};
        };
        const MouseHookSource::Claims claims = [&](MouseButton button, std::uint8_t modifiers) {
            asked.push_back(Json::array({mouse_button_name(button), modifier_names(modifiers)}));
            if (raising) {
                throw std::runtime_error("claims failed");
            }
            return claimed.contains(button);
        };
        const MonotonicClock clock = [] {
            return 0.0;
        };
        const MouseButtonFilter::Dispatch dispatch = [&queued](const MouseEvent& event) {
            queued.push_back(Json::array(
                {mouse_button_name(event.button), event.pressed, modifier_names(event.modifiers)}));
        };

        const Json& steps = c.arg<Json>(1);
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const Json& step = steps[i];
            const auto op = step.at("op").get<std::string>();
            INFO("step " << i << " " << step.dump());
            if (op == "call") {
                pressed.clear();
                for (const auto& vk : step.at("pressed")) {
                    pressed.insert(vk.get<int>());
                }
                asked = Json::array();
                queued = Json::array();
                const bool swallowed = filter.decide(
                    step.at("code").get<int>(), step.at("message").get<unsigned>(),
                    step.at("xbutton").get<std::uint32_t>() << 16,
                    step.at("flags").get<std::uint32_t>(), key_state, claims, clock, dispatch);
                CHECK((swallowed ? 1 : 0) == expected[i][0].get<int>());
                CHECK(golden::equal(expected[i][1], asked));
                CHECK(golden::equal(expected[i][2], queued));
            } else if (op == "claimed") {
                claimed.clear();
                for (const auto& name : step.at("buttons")) {
                    claimed.insert(button_named(name));
                }
            } else if (op == "raise") {
                raising = step.at("value").get<bool>();
            } else if (op == "restart") {
                // A new installation keeps the filter, and with it the swallowed presses.
            } else {
                FAIL("unknown op " << op);
            }
        }
    });
}

TEST_CASE("services._hotkey_keyboard mouse_button traces", "[golden]") {
    golden::for_each("services._hotkey_keyboard", "mouse_button", [](const golden::Case& c) {
        std::map<std::string, std::string> hotkeys;
        for (const auto& [action, hotkey] : default_hotkeys("win32")) {
            hotkeys[action] = hotkey;
        }
        const auto saved_hotkeys = c.arg<Json>(0);
        for (const auto& [action, hotkey] : saved_hotkeys.items()) {
            hotkeys[action] = hotkey.get<std::string>();
        }
        testing::Harness h(hotkeys);
        h.attach_mouse();
        std::vector<std::string> fired;
        std::vector<std::optional<double>> stamps;
        const auto record = [&fired, &stamps](std::string name, std::optional<double> at) {
            fired.push_back(std::move(name));
            stamps.push_back(at);
        };
        Callbacks callbacks;
        const auto stamped = [&record](const char* name) {
            return [&record, name](double at) {
                record(name, at);
            };
        };
        const auto plain = [&record](const char* name) {
            return [&record, name] {
                record(name, std::nullopt);
            };
        };
        callbacks.on_record_toggle = plain("record_toggle");
        callbacks.on_record_press = stamped("record_press");
        callbacks.on_record_release = stamped("record_release");
        callbacks.on_command_press = stamped("command_press");
        callbacks.on_command_release = stamped("command_release");
        for (const auto action : kPressActions) {
            *callbacks.press_action(action) = [&record, action] {
                record(std::string(action), std::nullopt);
            };
        }
        callbacks.on_status_update_auto_hide = [&record](const std::string& status) {
            record("status:" + status, std::nullopt);
        };
        h.manager->set_callbacks(callbacks);

        const Json& steps = c.arg<Json>(1);
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const Json& step = steps[i];
            const auto op = step.at("op").get<std::string>();
            INFO("step " << i << " " << step.dump());
            fired.clear();
            stamps.clear();
            if (op == "claims") {
                CHECK(h.manager->claims_mouse_button(button_named(step.at("button")),
                                                     modifier_mask(step.at("modifiers"))) ==
                      expected[i].get<bool>());
            } else if (op == "mouse") {
                h.manager->on_mouse_button(
                    {button_named(step.at("button")), step.at("pressed").get<bool>(),
                     modifier_mask(step.at("modifiers")), golden::decode<double>(step.at("t"))});
                CHECK(same_entries(expected[i], fired, stamps));
            } else if (op == "hook") {
                CHECK(h.manager->mouse_hook_running() == expected[i].get<bool>());
            } else if (op == "held") {
                h.manager->with_matcher([&](HotkeyMatcher& m) {
                    auto holds = m.press_holds();
                    std::sort(holds.begin(), holds.end());
                    CHECK(m.record_key_held() == expected[i][0].get<bool>());
                    CHECK(m.command_key_held() == expected[i][1].get<bool>());
                    CHECK(golden::equal(expected[i][2], Json(holds)));
                });
            } else if (op == "cleanup") {
                h.manager->cleanup();
            } else if (op == "key") {
                const std::string name =
                    step.at("name").is_null() ? "" : step.at("name").get<std::string>();
                KeyEvent event;
                event.down = step.at("type") == "down";
                event.name = name;
                if (!step.at("scan").is_null()) {
                    event.scan_code = step.at("scan").get<int>();
                }
                event.keypad = step.at("keypad").get<bool>();
                event.t = golden::decode<double>(step.at("t"));
                event.modifiers = modifier_mask(step.at("held"));
                const bool passes = h.hook->send(event);
                CHECK(passes == expected[i][0].get<bool>());
                CHECK(same_entries(expected[i][1], fired, stamps));
            } else if (op == "mode") {
                h.manager->set_record_mode(trigger_mode_from(step.at("mode").get<std::string>()));
            } else if (op == "enabled") {
                h.manager->set_program_enabled(step.at("value").get<bool>());
            } else if (op == "suspend") {
                h.manager->set_capture_suspended(step.at("value").get<bool>());
            } else if (op == "dynamic") {
                const auto family = step.at("family").get<std::string>();
                HotkeyList items;
                for (const auto& item : step.at("items")) {
                    items.emplace_back(item.at(0).get<std::string>(),
                                       item.at(1).get<std::string>());
                }
                h.manager->set_dynamic_hotkeys(family, items,
                                               [&record, family](const std::string& id) {
                                                   record(family + ":" + id, std::nullopt);
                                               });
            } else if (op == "update") {
                std::map<std::string, std::string> update;
                for (const auto& [action, hotkey] : step.at("hotkeys").items()) {
                    update[action] = hotkey.get<std::string>();
                }
                h.manager->update_hotkeys(update);
            } else if (op == "rehook") {
                h.manager->rehook();
            } else if (op == "hold_active") {
                CHECK(h.manager->keyboard_hold_active(golden::decode<double>(step.at("now"))) ==
                      expected[i].get<bool>());
            } else {
                FAIL("unknown op " << op);
            }
        }
    });
}

TEST_CASE("ui_qt.widgets.hotkey_capture mouse_shortcut_note and is_escape_key", "[golden]") {
    golden::for_each(
        "ui_qt.widgets.hotkey_capture", "mouse_shortcut_note", [](const golden::Case& c) {
            const auto vocabulary =
                c.arg<std::string>(1) == "windows" ? Vocabulary::Windows : Vocabulary::Pynput;
            CHECK(mouse_shortcut_note(c.arg<std::string>(0), vocabulary) ==
                  c.result<std::string>());
        });
    golden::for_each("ui_qt.widgets.hotkey_capture", "is_escape_key", [](const golden::Case& c) {
        const auto name = c.arg<std::optional<std::string>>(0);
        CHECK(is_escape_key(name ? std::optional<std::string_view>(*name) : std::nullopt) ==
              c.result<bool>());
    });
}

TEST_CASE("ui_qt.widgets.hotkey_capture capture runs", "[golden]") {
    golden::for_each("ui_qt.widgets.hotkey_capture", "capture_run", [](const golden::Case& c) {
        auto fake = std::make_unique<FakeKeyboardHook>();
        FakeKeyboardHook* hook = fake.get();
        auto capture = make_hotkey_capture(std::move(fake));
        fv::ManualExecutor ui;
        Json result = {{"captured", Json::array()}, {"cancelled", 0}, {"failed", Json::array()}};
        auto s1 = capture->captured.connect(
            ui, [&result](const std::string& hotkey) { result["captured"].push_back(hotkey); });
        auto s2 = capture->cancelled.connect(
            ui, [&result] { result["cancelled"] = result["cancelled"].get<int>() + 1; });
        auto s3 = capture->failed.connect(
            ui, [&result](const std::string& message) { result["failed"].push_back(message); });

        capture->start();
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (!hook->installed() && std::chrono::steady_clock::now() < until) {
            std::this_thread::sleep_for(1ms);
        }
        REQUIRE(hook->installed());
        const Json& events = c.arg<Json>(0);
        // Names live as long as the events that point at them.
        std::vector<std::string> names;
        names.reserve(events.size());
        bool released = false;
        for (const auto& event : events) {
            names.push_back(event[1].is_null() ? "" : event[1].get<std::string>());
            KeyEvent key;
            key.down = event[0] == "down";
            key.name = names.back();
            key.keypad = event[2].get<bool>();
            released = released || !key.down;
            CHECK(hook->send(key));
        }
        // Python's run ends at the queued None when no key-up came; nothing is emitted then.
        if (!released) {
            capture->stop();
        }
        REQUIRE(capture->wait(2000ms));
        ui.run_all();
        CHECK(golden::equal(c.result<Json>(), result));
    });
}
