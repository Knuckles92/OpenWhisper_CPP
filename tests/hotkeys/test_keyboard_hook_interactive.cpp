// Tests of the real WH_KEYBOARD_LL hook. They install a global hook and send keys through
// SendInput, which takes over the desktop session, so they carry [interactive]: every default
// preset leaves them out, and only `windows-interactive` runs them, with the owner's go-ahead.
#ifdef _WIN32

#include <catch2/catch_test_macros.hpp>

#include <windows.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

#include "base/input_tag.h"
#include "hotkeys/hotkey_manager.h"

using namespace fv::hotkeys;
using namespace std::chrono_literals;

namespace {

// Untagged, the hook takes it for another program's input, which shortcuts match; with
// kSyntheticInputTag, for OpenWhisper_CPP's own, which passes unseen.
void send_numpad_star(ULONG_PTR extra_info = 0) {
    INPUT inputs[2] = {};
    for (int i = 0; i < 2; ++i) {
        inputs[i].type = INPUT_KEYBOARD;
        inputs[i].ki.wVk = VK_MULTIPLY;
        inputs[i].ki.wScan = 0x37;
        inputs[i].ki.dwFlags = i == 1 ? KEYEVENTF_KEYUP : 0;
        inputs[i].ki.dwExtraInfo = extra_info;
    }
    SendInput(2, inputs, sizeof(INPUT));
}

// A foreground window with an edit control, pumped on this thread.
class EditWindow {
public:
    EditWindow() {
        window_ = CreateWindowExW(0, L"EDIT", L"", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 300,
                                  100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        SetForegroundWindow(window_);
        SetFocus(window_);
        pump(200ms);
    }
    ~EditWindow() { DestroyWindow(window_); }

    std::wstring text() const {
        wchar_t buffer[64] = {};
        GetWindowTextW(window_, buffer, 64);
        return buffer;
    }

    static void pump(std::chrono::milliseconds duration) {
        const auto until = std::chrono::steady_clock::now() + duration;
        MSG message;
        while (std::chrono::steady_clock::now() < until) {
            while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            std::this_thread::sleep_for(5ms);
        }
    }

private:
    HWND window_ = nullptr;
};

} // namespace

TEST_CASE("the real hook swallows kp * and fires the toggle, and lets it through while disabled",
          "[interactive]") {
    HotkeyManager manager;
    REQUIRE(manager.backend_available());
    std::atomic<int> toggles{0};
    Callbacks callbacks;
    callbacks.on_record_toggle = [&toggles] {
        ++toggles;
    };
    manager.set_callbacks(callbacks);
    EditWindow edit;

    send_numpad_star();
    EditWindow::pump(300ms);
    CHECK(toggles == 1);
    CHECK(edit.text().empty());

    manager.set_program_enabled(false);
    send_numpad_star();
    EditWindow::pump(300ms);
    CHECK(toggles == 1);
    CHECK(edit.text() == L"*");
    manager.cleanup();
}

TEST_CASE("the real hook lets our own tagged kp * through without firing the toggle",
          "[interactive]") {
    HotkeyManager manager;
    REQUIRE(manager.backend_available());
    std::atomic<int> toggles{0};
    Callbacks callbacks;
    callbacks.on_record_toggle = [&toggles] {
        ++toggles;
    };
    manager.set_callbacks(callbacks);
    EditWindow edit;

    send_numpad_star(fv::kSyntheticInputTag);
    EditWindow::pump(300ms);
    CHECK(toggles == 0);
    CHECK(edit.text() == L"*");

    send_numpad_star();
    EditWindow::pump(300ms);
    CHECK(toggles == 1);
    CHECK(edit.text() == L"*");
    manager.cleanup();
}

#endif
