#include "hotkeys/mouse_hook.h"

#include <utility>

#include "base/metrics.h"
#include "pyport/number.h"

namespace fv::hotkeys {

namespace {

std::size_t slot(MouseButton button) {
    return button == MouseButton::Mouse4 ? 0 : 1;
}

} // namespace

std::string_view mouse_button_name(MouseButton button) {
    return button == MouseButton::Mouse4 ? "mouse4" : "mouse5";
}

std::optional<MouseButton> mouse_button_from_name(std::string_view name) {
    if (name == "mouse4") {
        return MouseButton::Mouse4;
    }
    if (name == "mouse5") {
        return MouseButton::Mouse5;
    }
    return std::nullopt;
}

std::optional<MouseButton> mouse_button_from_xbutton(unsigned xbutton) {
    if (xbutton == 1) {
        return MouseButton::Mouse4;
    }
    if (xbutton == 2) {
        return MouseButton::Mouse5;
    }
    return std::nullopt;
}

MouseButtonFilter::MouseButtonFilter(MonotonicClock timer) : timer_(std::move(timer)) {}

MonotonicClock MouseButtonFilter::steady_seconds() {
    return [] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch())
            .count();
    };
}

std::optional<MouseButton> MouseButtonFilter::real_side_button(std::uint32_t mouse_data,
                                                               std::uint32_t flags) {
    if ((flags & kLlmhfInjected) != 0) {
        return std::nullopt;
    }
    return mouse_button_from_xbutton((mouse_data >> 16) & 0xFFFFU);
}

bool MouseButtonFilter::decide(int code, unsigned message, std::uint32_t mouse_data,
                               std::uint32_t flags, const KeyState& key_state,
                               const MouseHookSource::Claims& claims, const MonotonicClock& clock,
                               const Dispatch& dispatch) {
    if (message != kWmXButtonDown && message != kWmXButtonUp) {
        return false;
    }
    const double started = timer_();
    bool swallow = false;
    try {
        const auto button = code < 0 ? std::nullopt : real_side_button(mouse_data, flags);
        if (button && message == kWmXButtonDown) {
            const std::uint8_t held = held_modifiers(key_state);
            if (claims(*button, held)) {
                suppressed_[slot(*button)] = true;
                dispatch(MouseEvent{*button, true, held, clock()});
                swallow = true;
            }
        } else if (button && suppressed_[slot(*button)].exchange(false)) {
            dispatch(MouseEvent{*button, false, 0, clock()});
            swallow = true;
        }
    } catch (...) {
        // Windows must always get the click back when the decision fails.
        swallow = false;
    }
    const double elapsed_ms = (timer_() - started) * 1000;
    double worst = worst_ms_.load();
    while (elapsed_ms > worst && !worst_ms_.compare_exchange_weak(worst, elapsed_ms)) {
    }
    return swallow;
}

void MouseButtonFilter::report_latency() {
    const double worst = worst_ms_.load();
    {
        std::lock_guard lock(report_mutex_);
        if (worst <= reported_ms_) {
            return;
        }
        reported_ms_ = worst;
    }
    metrics::record("mouse_hook_callback_ms", py::round_ndigits(worst, 3));
}

bool MouseButtonFilter::suppressed(MouseButton button) const {
    return suppressed_[slot(button)].load();
}

bool ClickFilter::reads(unsigned message) {
    return message == kWmLButtonDown || message == kWmLButtonUp || message == kWmRButtonDown ||
           message == kWmRButtonUp;
}

bool ClickFilter::pressed_outside(unsigned down_message) const {
    return down_message == kWmLButtonDown ? left_ : down_message == kWmRButtonDown && right_;
}

std::optional<Click> ClickFilter::observe(int code, unsigned message, int x, int y,
                                          std::uint32_t flags, const PressedOutside& outside,
                                          const MonotonicClock& clock) {
    if (code < 0 || !reads(message) || (flags & kLlmhfInjected) != 0) {
        return std::nullopt;
    }
    const bool left = message == kWmLButtonDown || message == kWmLButtonUp;
    bool& pressed = left ? left_ : right_;
    try {
        if (message == kWmLButtonDown || message == kWmRButtonDown) {
            // Forgotten first, so a press whose owner cannot be read records nothing.
            pressed = false;
            pressed = outside(x, y);
            return std::nullopt;
        }
        if (!pressed) {
            return std::nullopt;
        }
        pressed = false;
        return Click{x, y, clock()};
    } catch (...) {
        // Windows must always get the click back; this one is simply not observed.
        return std::nullopt;
    }
}

} // namespace fv::hotkeys
