#include "focus/win/send_input.h"

#include <utility>

#include "base/input_tag.h"

namespace fv::focus {

namespace {

// The OS keyboard. SendInput events carry LLKHF_INJECTED and, from here, kSyntheticInputTag:
// together they tell the hotkey hook the keys are ours, and it lets them through unseen.
class WindowsKeyboardPort final : public KeyboardPort {
public:
    explicit WindowsKeyboardPort(SendInputFn send_input) : send_input_(std::move(send_input)) {}

    std::size_t send(std::span<const KeyStroke> strokes) override {
        auto inputs = keyboard_inputs(strokes);
        return send_tagged(inputs, send_input_);
    }

    std::uint16_t scan_code(std::uint16_t vk) override {
        return static_cast<std::uint16_t>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    }

    bool reads_key_state() const override { return true; }

    bool key_down(std::uint16_t vk) override { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

private:
    SendInputFn send_input_;
};

} // namespace

void tag_input(INPUT& input) {
    switch (input.type) {
    case INPUT_KEYBOARD:
        input.ki.dwExtraInfo = kSyntheticInputTag;
        break;
    case INPUT_MOUSE:
        input.mi.dwExtraInfo = kSyntheticInputTag;
        break;
    default:
        break;
    }
}

std::vector<INPUT> keyboard_inputs(std::span<const KeyStroke> strokes) {
    std::vector<INPUT> inputs;
    inputs.reserve(strokes.size());
    for (const auto& stroke : strokes) {
        INPUT input{};
        input.type = INPUT_KEYBOARD;
        input.ki.wVk = stroke.vk;
        input.ki.wScan = stroke.scan;
        input.ki.dwFlags = stroke.up ? KEYEVENTF_KEYUP : 0;
        tag_input(input);
        inputs.push_back(input);
    }
    return inputs;
}

UINT send_tagged(std::span<INPUT> inputs, const SendInputFn& send) {
    for (auto& input : inputs) {
        tag_input(input);
    }
    return send(inputs);
}

std::shared_ptr<KeyboardPort> make_keyboard_port(SendInputFn send_input) {
    return std::make_shared<WindowsKeyboardPort>(std::move(send_input));
}

std::shared_ptr<KeyboardPort> make_keyboard_port() {
    return make_keyboard_port([](std::span<INPUT> inputs) {
        return SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
    });
}

} // namespace fv::focus
