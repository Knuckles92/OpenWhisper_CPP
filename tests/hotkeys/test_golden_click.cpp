// The walkthrough hook's click decisions (services/_mouse_hook_win.py observe_click) against
// OpenWhisper's callback over scripted calls (tools/parity/cases/services._mouse_hook_win.py,
// click_trace). Each call is decided as the Windows hook procedure decides it: side-button
// messages by MouseButtonFilter (with nothing claimed), the left and right buttons by a
// ClickFilter, everything passed on. WindowFromPoint and GetWindowThreadProcessId are the step's
// window and owner, combined as win/mouse_hook.cpp combines them.
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <string>

#include "golden/golden.h"
#include "hotkeys/mouse_hook.h"

using namespace fv::hotkeys;
using fv::Json;
namespace golden = fv::golden;

TEST_CASE("services._mouse_hook_win click traces", "[golden]") {
    golden::for_each("services._mouse_hook_win", "click_trace", [](const golden::Case& c) {
        ClickFilter clicks;
        MouseButtonFilter buttons;
        const MouseButtonFilter::KeyState key_state = [](int) -> short {
            return 0;
        };
        const MouseHookSource::Claims claims_nothing = [](MouseButton, std::uint8_t) {
            return false;
        };
        const MonotonicClock clock = [] {
            return 0.0;
        };
        Json queued = Json::array();
        const MouseButtonFilter::Dispatch dispatch = [&queued](const MouseEvent& event) {
            queued.push_back(
                Json::array({"button", mouse_button_name(event.button), event.pressed}));
        };

        const Json& steps = c.arg<Json>(0);
        const Json expected = c.result<Json>();
        REQUIRE(expected.size() == steps.size());
        for (std::size_t i = 0; i < steps.size(); ++i) {
            const Json& step = steps[i];
            const auto op = step.at("op").get<std::string>();
            INFO("step " << i << " " << step.dump());
            if (op == "restart") {
                clicks = ClickFilter();
                continue;
            }
            REQUIRE(op == "call");
            const int code = step.at("code").get<int>();
            const auto message = step.at("message").get<unsigned>();
            const int x = step.at("x").get<int>();
            const int y = step.at("y").get<int>();
            const auto flags = step.at("flags").get<std::uint32_t>();
            const auto xbutton = step.value("xbutton", 0U);
            const int window = step.at("window").get<int>();
            const std::string owner =
                step.at("owner").is_null() ? std::string() : step.at("owner").get<std::string>();
            Json asked = Json::array();
            const ClickFilter::PressedOutside outside = [&](int at_x, int at_y) {
                asked.push_back(Json::array({at_x, at_y}));
                return window != 0 && owner == "other";
            };
            queued = Json::array();
            bool swallowed = false;
            if (message == kWmXButtonDown || message == kWmXButtonUp) {
                swallowed = buttons.decide(code, message, xbutton << 16, flags, key_state,
                                           claims_nothing, clock, dispatch);
            } else if (code >= 0 && ClickFilter::reads(message)) {
                if (const auto click = clicks.observe(code, message, x, y, flags, outside, clock)) {
                    queued.push_back(Json::array({click->x, click->y}));
                }
            }
            CHECK((swallowed ? 1 : 0) == expected[i][0].get<int>());
            CHECK(golden::equal(expected[i][1], asked));
            CHECK(golden::equal(expected[i][2], queued));
        }
    });
}
