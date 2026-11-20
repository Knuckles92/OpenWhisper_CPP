#include <catch2/catch_test_macros.hpp>

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "golden/golden.h"
#include "harness.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/grammar.h"
#include "hotkeys/hold_machine.h"

using namespace fv::hotkeys;
using fv::Json;
namespace golden = fv::golden;

namespace {

Vocabulary vocabulary(const std::string& name) {
    return name == "windows" ? Vocabulary::Windows : Vocabulary::Pynput;
}

Json parsed_json(const ParsedHotkey& parsed) {
    Json modifiers = Json::array();
    for (const auto& modifier : parsed.modifiers) {
        modifiers.push_back(modifier);
    }
    return Json::array({modifiers, parsed.main_key ? Json(*parsed.main_key) : Json(nullptr)});
}

std::set<std::string> string_set(const Json& names) {
    std::set<std::string> set;
    for (const auto& name : names) {
        set.insert(name.get<std::string>());
    }
    return set;
}

std::uint8_t modifier_mask(const Json& names) {
    std::uint8_t mask = 0;
    for (const auto& name : names) {
        const auto text = name.get<std::string>();
        mask |= text == "ctrl" ? kCtrl : 0;
        mask |= text == "alt" ? kAlt : 0;
        mask |= text == "shift" ? kShift : 0;
        mask |= (text == "win" || text == "windows") ? kWin : 0;
    }
    return mask;
}

// "record_press@1.0" as Python wrote it: the name, and the time stamp repr() printed, which
// round-trips to the same double.
bool same_entry(const std::string& expected, const std::string& actual_name,
                std::optional<double> actual_at) {
    const auto at = expected.find('@');
    if (at == std::string::npos) {
        return !actual_at && expected == actual_name;
    }
    return actual_at && expected.substr(0, at) == actual_name &&
           std::stod(expected.substr(at + 1)) == *actual_at;
}

struct Entry {
    std::string name;
    std::optional<double> at;
};

bool same_entries(const Json& expected, const std::vector<Entry>& actual) {
    if (expected.size() != actual.size()) {
        return false;
    }
    for (std::size_t i = 0; i < actual.size(); ++i) {
        if (!same_entry(expected[i].get<std::string>(), actual[i].name, actual[i].at)) {
            return false;
        }
    }
    return true;
}

std::string describe(const std::vector<Entry>& entries) {
    std::string out = "[";
    for (const auto& entry : entries) {
        out += entry.name;
        if (entry.at) {
            out += "@" + std::to_string(*entry.at);
        }
        out += ", ";
    }
    return out + "]";
}

} // namespace

TEST_CASE("services._hotkey_common parse_hotkey_string", "[golden]") {
    golden::for_each("services._hotkey_common", "parse_hotkey_string", [](const golden::Case& c) {
        const auto parsed = parse_hotkey(c.arg<std::string>(0), vocabulary(c.arg<std::string>(1)));
        CHECK(golden::equal(c.result<Json>(), parsed_json(parsed)));
    });
}

TEST_CASE("services._hotkey_common format_hotkey_string", "[golden]") {
    golden::for_each("services._hotkey_common", "format_hotkey_string", [](const golden::Case& c) {
        const auto modifiers = string_set(c.arg<Json>(0));
        CHECK(format_hotkey(modifiers, c.arg<std::optional<std::string>>(1),
                            vocabulary(c.arg<std::string>(2))) == c.result<std::string>());
    });
}

TEST_CASE("services._hotkey_common is_mouse_key", "[golden]") {
    golden::for_each("services._hotkey_common", "is_mouse_key", [](const golden::Case& c) {
        const auto key = c.arg<std::optional<std::string>>(0);
        CHECK(is_mouse_key(key ? std::optional<std::string_view>(*key) : std::nullopt) ==
              c.result<bool>());
    });
}

TEST_CASE("services._hotkey_common Debouncer", "[golden]") {
    golden::for_each("services._hotkey_common", "Debouncer", [](const golden::Case& c) {
        Debouncer debouncer(c.arg<int>(0), [] { return 0.0; });
        Json results = Json::array();
        for (const auto& now : c.arg<Json>(1)) {
            if (now.is_null()) {
                debouncer.reset();
                results.push_back(nullptr);
            } else {
                results.push_back(debouncer.should_trigger_at(golden::decode<double>(now)));
            }
        }
        CHECK(golden::equal(c.result<Json>(), results));
    });
}

TEST_CASE("services._hotkey_keyboard grammar and display", "[golden]") {
    golden::for_each("services._hotkey_keyboard", "parse_hotkey", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            parsed_json(parse_hotkey(c.arg<std::string>(0), Vocabulary::Windows))));
    });
    golden::for_each("services._hotkey_keyboard", "format_hotkey", [](const golden::Case& c) {
        CHECK(format_hotkey(string_set(c.arg<Json>(0)), c.arg<std::optional<std::string>>(1),
                            Vocabulary::Windows) == c.result<std::string>());
    });
    golden::for_each("services._hotkey_keyboard", "format_hotkey_display",
                     [](const golden::Case& c) {
                         CHECK(format_hotkey_display(c.arg<std::string>(0), Vocabulary::Windows) ==
                               c.result<std::string>());
                     });
}

TEST_CASE("services._hotkey_pynput grammar and display", "[golden]") {
    golden::for_each("services._hotkey_pynput", "parse_hotkey", [](const golden::Case& c) {
        CHECK(golden::equal(c.result<Json>(),
                            parsed_json(parse_hotkey(c.arg<std::string>(0), Vocabulary::Pynput))));
    });
    golden::for_each("services._hotkey_pynput", "format_hotkey", [](const golden::Case& c) {
        CHECK(format_hotkey(string_set(c.arg<Json>(0)), c.arg<std::optional<std::string>>(1),
                            Vocabulary::Pynput) == c.result<std::string>());
    });
    golden::for_each("services._hotkey_pynput", "format_hotkey_display", [](const golden::Case& c) {
        const bool mac = c.arg<std::string>(1) == "darwin";
        CHECK(format_hotkey_display(c.arg<std::string>(0), Vocabulary::Pynput, mac) ==
              c.result<std::string>());
    });
}

TEST_CASE("services._hotkey_keyboard handle_keyboard_event traces", "[golden]") {
    golden::for_each(
        "services._hotkey_keyboard", "handle_keyboard_event", [](const golden::Case& c) {
            std::map<std::string, std::string> hotkeys;
            for (const auto& [action, hotkey] : default_hotkeys("win32")) {
                hotkeys[action] = hotkey;
            }
            const auto saved_hotkeys = c.arg<Json>(0);
            for (const auto& [action, hotkey] : saved_hotkeys.items()) {
                hotkeys[action] = hotkey.get<std::string>();
            }
            testing::Harness h(hotkeys);
            std::vector<Entry> fired;
            Callbacks callbacks;
            const auto stamped = [&fired](const char* name) {
                return [&fired, name](double at) {
                    fired.push_back({name, at});
                };
            };
            const auto plain = [&fired](const char* name) {
                return [&fired, name] {
                    fired.push_back({name, std::nullopt});
                };
            };
            callbacks.on_record_toggle = plain("record_toggle");
            callbacks.on_record_press = stamped("record_press");
            callbacks.on_record_release = stamped("record_release");
            callbacks.on_command_press = stamped("command_press");
            callbacks.on_command_release = stamped("command_release");
            for (const auto action : kPressActions) {
                *callbacks.press_action(action) = [&fired, action] {
                    fired.push_back({std::string(action), std::nullopt});
                };
            }
            callbacks.on_status_update_auto_hide = [&fired](const std::string& status) {
                fired.push_back({"status:" + status, std::nullopt});
            };
            h.manager->set_callbacks(callbacks);

            const Json& steps = c.arg<Json>(1);
            const Json expected = c.result<Json>();
            REQUIRE(expected.size() == steps.size());
            for (std::size_t i = 0; i < steps.size(); ++i) {
                const Json& step = steps[i];
                const auto op = step.at("op").get<std::string>();
                INFO("step " << i << " " << step.dump());
                if (op == "key") {
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
                    if (!step.at("native").is_null()) {
                        event.os_modifiers = modifier_mask(step.at("native"));
                    }
                    fired.clear();
                    const bool passes = h.hook->send(event);
                    CHECK(passes == expected[i][0].get<bool>());
                    INFO("fired " << describe(fired));
                    CHECK(same_entries(expected[i][1], fired));
                } else if (op == "mode") {
                    h.manager->set_record_mode(
                        trigger_mode_from(step.at("mode").get<std::string>()));
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
                    h.manager->set_dynamic_hotkeys(
                        family, items, [&fired, family](const std::string& id) {
                            fired.push_back({family + ":" + id, std::nullopt});
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

TEST_CASE("services.hotkey_conflicts hotkey_conflict", "[golden]") {
    golden::for_each("services.hotkey_conflicts", "hotkey_conflict", [](const golden::Case& c) {
        const Json result = c.result<Json>();
        ConflictOptions options;
        options.vocabulary = Vocabulary::Windows;
        options.platform = "win32";
        if (c.arg_count() > 2 && !c.args()[2].is_null()) {
            options.exclude = {c.args()[2][0].get<std::string>(),
                               c.args()[2][1].get<std::string>()};
        }
        if (c.arg_count() > 3 && !c.args()[3].is_null()) {
            options.standard_hotkeys = c.args()[3];
        }
        const auto load = [](const Json& rows) {
            std::vector<Binding> bindings;
            for (const auto& row : rows) {
                bindings.push_back({row[0].get<std::string>(), row[1].get<std::string>(),
                                    row[2].get<std::string>()});
            }
            return bindings;
        };
        const Json& bindings = result.at("bindings");
        options.bindings.profiles = load(bindings.at("profiles"));
        options.bindings.transforms = load(bindings.at("transforms"));
        options.bindings.actions = load(bindings.at("actions"));
        CHECK(hotkey_conflict(c.arg<std::string>(0), c.args()[1], options) ==
              result.at("conflict").get<std::string>());
    });
}

namespace {

class TraceHost final : public HoldMachine::Host {
public:
    bool is_recording() override { return recording; }
    bool capture_canceled() override { return canceled; }
    bool hands_free_latch() override { return latch; }
    bool wait_until_recording(double) override { return recording; }
    bool perform(const HoldMachine::Output& output) override {
        using Action = HoldMachine::Action;
        switch (output.action) {
        case Action::Start:
            done.push_back({"start", std::nullopt});
            if (accept) {
                recording = true;
            }
            return accept;
        case Action::Stop:
            done.push_back({"stop", std::nullopt});
            break;
        case Action::Cancel:
            done.push_back({"cancel", std::nullopt});
            break;
        case Action::Latch:
            done.push_back({"latch", std::nullopt});
            break;
        case Action::Unlatch:
            done.push_back({"unlatch", std::nullopt});
            break;
        case Action::ArmDeadline:
            done.push_back({"arm", output.at});
            pending = output.generation;
            break;
        case Action::CancelDeadline:
            done.push_back({"cancel_deadline", std::nullopt});
            pending.reset();
            break;
        }
        return true;
    }

    bool recording = false;
    bool canceled = false;
    bool latch = true;
    bool accept = true;
    std::optional<std::uint64_t> pending;
    std::vector<Entry> done;
};

std::string state_name(HoldMachine::State state) {
    switch (state) {
    case HoldMachine::State::Idle:
        return "idle";
    case HoldMachine::State::Holding:
        return "holding";
    case HoldMachine::State::TapPending:
        return "tap_pending";
    case HoldMachine::State::Latched:
        return "latched";
    }
    return "";
}

} // namespace

TEST_CASE("services.runtime.hotkeys hold traces", "[golden]") {
    golden::for_each("services.runtime.hotkeys", "hold_trace", [](const golden::Case& c) {
        TraceHost host;
        host.latch = c.arg<bool>(0);
        HoldMachine machine(host);
        const Json& steps = c.arg<Json>(1);
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const Json& step = steps[i];
            const auto op = step.at("op").get<std::string>();
            INFO("step " << i << " " << step.dump());
            host.done.clear();
            if (op == "press") {
                machine.press(golden::decode<double>(step.at("at")));
            } else if (op == "release") {
                machine.release(golden::decode<double>(step.at("at")));
            } else if (op == "deadline") {
                if (host.pending) {
                    const auto generation = *host.pending;
                    host.pending.reset();
                    machine.deadline(generation);
                }
            } else if (op == "ended") {
                host.recording = false;
                if (machine.recording_ended()) {
                    machine.drop_stale_latch();
                }
            } else if (op == "reset") {
                machine.reset();
            } else if (op == "recording") {
                host.recording = step.at("value").get<bool>();
            } else if (op == "canceled") {
                host.canceled = step.at("value").get<bool>();
            } else if (op == "accept") {
                host.accept = step.at("value").get<bool>();
            } else {
                FAIL("unknown op " << op);
            }
            INFO("did " << describe(host.done));
            CHECK(same_entries(expected[i][0], host.done));
            CHECK(state_name(machine.state()) == expected[i][1].get<std::string>());
        }
    });
}
