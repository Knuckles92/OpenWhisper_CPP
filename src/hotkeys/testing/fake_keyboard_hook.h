#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include "hotkeys/key_event.h"
#include "hotkeys/key_names.h"
#include "hotkeys/keyboard_hook.h"

namespace fv::hotkeys {

// A keyboard hook driven by the test: send() hands an event to the installed callback on the
// calling thread, as the hook thread would, and says whether it reached the focused app.
// send_raw() first runs the hook data through `keyboard`'s bookkeeping (KeyEventTranslator over
// the generated names), so modifiers, names and numpad flags come out as on Windows. Either way
// an event OpenWhisper_CPP sent itself (own_input()) passes without reaching the callback and
// without being recorded as pressed, as in the Windows hook. Affinity: thread-safe.
class FakeKeyboardHook final : public KeyboardHook {
public:
    FakeKeyboardHook() : translator_(KeyNameTable::generated()) {}

    void install(Callback callback) override {
        std::lock_guard lock(mutex_);
        if (fail_next_) {
            std::string message = std::move(*fail_next_);
            fail_next_.reset();
            throw std::runtime_error(message);
        }
        // `keyboard`'s record of pressed keys is process-wide: it survives a rehook.
        callback_ = std::move(callback);
        ++installs_;
    }
    void uninstall() override {
        std::lock_guard lock(mutex_);
        if (callback_) {
            ++uninstalls_;
        }
        callback_ = nullptr;
    }
    bool installed() const override {
        std::lock_guard lock(mutex_);
        return static_cast<bool>(callback_);
    }

    // True when the event passes; with no hook installed every event passes.
    bool send(const KeyEvent& event) {
        if (event.own_input()) {
            return true;
        }
        Callback callback;
        {
            std::lock_guard lock(mutex_);
            callback = callback_;
        }
        return !callback || callback(event);
    }

    bool send_raw(const RawKey& raw, double t = 0, LockState locks = {},
                  std::optional<std::uint8_t> os_modifiers = std::nullopt) {
        std::optional<KeyEvent> event;
        {
            std::lock_guard lock(mutex_);
            event = translator_.translate(raw, locks, t, os_modifiers);
        }
        if (!event || event->own_input()) {
            return true;
        }
        const bool pass = send(*event);
        if (pass) {
            std::lock_guard lock(mutex_);
            translator_.passed(*event);
        }
        return pass;
    }

    // keyboard.is_pressed over what send_raw() let through.
    bool is_pressed(int scan_code) const {
        std::lock_guard lock(mutex_);
        return translator_.is_pressed(scan_code);
    }
    std::uint8_t pressed_modifiers() const {
        std::lock_guard lock(mutex_);
        return translator_.pressed_modifiers();
    }

    // The next install() throws std::runtime_error(message).
    void fail_next_install(std::string message) {
        std::lock_guard lock(mutex_);
        fail_next_ = std::move(message);
    }
    int installs() const {
        std::lock_guard lock(mutex_);
        return installs_;
    }
    int uninstalls() const {
        std::lock_guard lock(mutex_);
        return uninstalls_;
    }

private:
    mutable std::mutex mutex_;
    Callback callback_;
    KeyEventTranslator translator_;
    std::optional<std::string> fail_next_;
    int installs_ = 0;
    int uninstalls_ = 0;
};

} // namespace fv::hotkeys
