#include "hotkeys/keyboard_hook.h"

#include <stdexcept>

namespace fv::hotkeys {

namespace {

class UnsupportedKeyboardHook final : public KeyboardHook {
public:
    void install(Callback) override {
        throw std::runtime_error("Global hotkeys are not supported on this platform.");
    }
    void uninstall() override {}
    bool installed() const override { return false; }
};

} // namespace

std::unique_ptr<KeyboardHook> make_keyboard_hook(MonotonicClock) {
    return std::make_unique<UnsupportedKeyboardHook>();
}

} // namespace fv::hotkeys
