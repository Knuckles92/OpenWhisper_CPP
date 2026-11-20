#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "harness.h"

using namespace fv::hotkeys;
using fv::hotkeys::testing::Harness;
using fv::hotkeys::testing::windows_hotkeys;

namespace {

constexpr int kScCtrl = 29;
constexpr int kScAlt = 56;
constexpr int kScStar = 55;
constexpr int kScH = 35;

using Events = std::vector<std::string>;

// tests/test_personalize_fix3_h3_enable_disable_hold.py::_manager
std::unique_ptr<Harness> manager(TriggerMode mode = TriggerMode::Toggle,
                                 const std::map<std::string, std::string>& hotkeys = {}) {
    auto h = std::make_unique<Harness>(windows_hotkeys(hotkeys));
    h->attach_mouse();
    h->manager->set_record_mode(mode);
    Callbacks callbacks;
    Harness* raw = h.get();
    callbacks.on_record_toggle = [raw] {
        raw->events.push_back("record_toggle");
    };
    callbacks.on_record_press = [raw](double) {
        raw->events.push_back("record_press");
    };
    callbacks.on_record_release = [raw](double) {
        raw->events.push_back("record_release");
    };
    callbacks.on_status_update_auto_hide = [raw](const std::string& status) {
        raw->events.push_back(status);
    };
    h->manager->set_callbacks(std::move(callbacks));
    return h;
}

void modifiers(Harness& h, bool down) {
    for (const auto& [name, code] : {std::pair{"ctrl", kScCtrl}, std::pair{"alt", kScAlt}}) {
        if (down) {
            h.held.insert(name);
        } else {
            h.held.erase(name);
        }
        CHECK(h.passes(h.event(down, name, code)));
    }
}

KeyEvent star(Harness& h, bool down) {
    return h.event(down, "*", kScStar, true);
}

} // namespace

// from
// tests/test_personalize_fix3_h3_enable_disable_hold.py::test_holding_the_shortcut_disables_once_and_never_records
TEST_CASE("test_holding_the_shortcut_disables_once_and_never_records",
          "[test_personalize_fix3_h3_enable_disable_hold]") {
    const TriggerMode mode = GENERATE(TriggerMode::Toggle, TriggerMode::PushHold);
    auto h = manager(mode);

    modifiers(*h, true);
    for (int i = 0; i < 4; ++i) {
        CHECK_FALSE(h->passes(star(*h, true)));
    }
    modifiers(*h, false);
    for (int i = 0; i < 3; ++i) {
        CHECK_FALSE(h->passes(star(*h, true)));
    }
    CHECK_FALSE(h->passes(star(*h, false)));

    CHECK(h->events == Events{"STT Disabled"});
    CHECK_FALSE(h->manager->program_enabled());
    h->manager->with_matcher([](HotkeyMatcher& m) {
        CHECK_FALSE(m.record_key_held());
        CHECK(m.press_holds().empty());
    });

    modifiers(*h, true);
    CHECK_FALSE(h->passes(star(*h, true)));
    CHECK(h->events == Events{"STT Disabled", "STT Enabled"});
}

// from
// tests/test_personalize_fix3_h3_enable_disable_hold.py::test_separate_presses_still_toggle_and_num_star_records_again
TEST_CASE("test_separate_presses_still_toggle_and_num_star_records_again",
          "[test_personalize_fix3_h3_enable_disable_hold]") {
    auto h = manager();

    modifiers(*h, true);
    for (int i = 0; i < 2; ++i) {
        CHECK_FALSE(h->passes(star(*h, true)));
        CHECK_FALSE(h->passes(star(*h, false)));
    }
    modifiers(*h, false);
    CHECK(h->passes(h->down("h", kScH)));
    CHECK(h->passes(h->up("h", kScH)));
    CHECK_FALSE(h->passes(star(*h, true)));

    CHECK(h->events == Events{"STT Disabled", "STT Enabled", "record_toggle"});
}

// from
// tests/test_personalize_fix3_h3_enable_disable_hold.py::test_num_star_reaches_the_app_while_dictation_is_off
TEST_CASE("test_num_star_reaches_the_app_while_dictation_is_off",
          "[test_personalize_fix3_h3_enable_disable_hold]") {
    auto h = manager();
    modifiers(*h, true);
    CHECK_FALSE(h->passes(star(*h, true)));
    CHECK_FALSE(h->passes(star(*h, false)));
    modifiers(*h, false);

    CHECK(h->passes(h->down("h", kScH)));
    CHECK(h->passes(star(*h, true)));
    CHECK(h->passes(star(*h, false)));
    CHECK(h->events == Events{"STT Disabled"});
}

// from
// tests/test_personalize_fix3_h3_enable_disable_hold.py::test_a_side_button_shortcut_toggles_once_per_click
TEST_CASE("test_a_side_button_shortcut_toggles_once_per_click",
          "[test_personalize_fix3_h3_enable_disable_hold]") {
    auto h = manager(TriggerMode::Toggle, {{"enable_disable", "mouse4"}});

    for (int i = 0; i < 2; ++i) {
        h->manager->on_mouse_button({MouseButton::Mouse4, true, 0, 0.0});
        h->manager->on_mouse_button({MouseButton::Mouse4, false, 0, 0.0});
    }

    CHECK(h->events == Events{"STT Disabled", "STT Enabled"});
    h->manager->with_matcher([](HotkeyMatcher& m) { CHECK(m.press_holds().empty()); });
}
