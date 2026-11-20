#include "hotkeys/keyboard_hook.h"

#include <windows.h>

#include <atomic>
#include <exception>
#include <format>
#include <future>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include "base/log.h"
#include "hotkeys/detail/thread_name.h"
#include "hotkeys/key_names.h"

namespace fv::hotkeys {

namespace {

constexpr log::Logger kLog{"services._hotkey_keyboard"};

// `keyboard` builds its names once per process from the layout of the thread that first asks;
// a rehook on the same layout reuses them, so a refresh does not spend the time again.
std::shared_ptr<const KeyNameTable> names_for_this_thread() {
    static std::mutex mutex;
    static std::map<HKL, std::shared_ptr<const KeyNameTable>> cache;
    const HKL layout = GetKeyboardLayout(0);
    std::lock_guard lock(mutex);
    auto& table = cache[layout];
    if (!table) {
        table = std::make_shared<const KeyNameTable>(layout_key_names());
    }
    return table;
}

std::uint8_t os_modifiers() {
    const auto down = [](int vk) {
        return (GetAsyncKeyState(vk) & 0x8000) != 0;
    };
    std::uint8_t mask = 0;
    mask |= down(VK_CONTROL) ? kCtrl : 0;
    mask |= down(VK_MENU) ? kAlt : 0;
    mask |= down(VK_SHIFT) ? kShift : 0;
    mask |= (down(VK_LWIN) || down(VK_RWIN)) ? kWin : 0;
    return mask;
}

class WinKeyboardHook final : public KeyboardHook {
public:
    explicit WinKeyboardHook(MonotonicClock clock) : clock_(std::move(clock)) {}
    ~WinKeyboardHook() override { uninstall(); }

    void install(Callback callback) override {
        uninstall();
        callback_ = std::move(callback);
        std::promise<void> ready;
        auto installed = ready.get_future();
        thread_ = std::thread([this, ready = std::move(ready)]() mutable { run(ready); });
        try {
            installed.get();
        } catch (...) {
            thread_.join();
            throw;
        }
    }

    void uninstall() override {
        if (!thread_.joinable()) {
            return;
        }
        // The thread unhooks itself when its message loop ends: a hook is removed on the thread
        // that installed it.
        PostThreadMessageW(thread_id_.load(), WM_QUIT, 0, 0);
        thread_.join();
        installed_ = false;
    }

    bool installed() const override { return installed_; }

private:
    static LRESULT CALLBACK proc(int code, WPARAM wparam, LPARAM lparam) {
        WinKeyboardHook* self = current_;
        if (code == HC_ACTION && self != nullptr) {
            if (!self->deliver(wparam, *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam))) {
                return 1;
            }
        }
        return CallNextHookEx(nullptr, code, wparam, lparam);
    }

    // False when the event is swallowed.
    bool deliver(WPARAM message, const KBDLLHOOKSTRUCT& info) {
        try {
            RawKey raw;
            switch (message) {
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                raw.down = true;
                break;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                raw.down = false;
                break;
            default:
                return true;
            }
            raw.vk = static_cast<int>(info.vkCode);
            raw.scan_code = static_cast<int>(info.scanCode);
            raw.extended = (info.flags & LLKHF_EXTENDED) != 0;
            constexpr DWORD kInjectedAlt = LLKHF_INJECTED | LLKHF_ALTDOWN;
            raw.injected_alt = (info.flags & kInjectedAlt) == kInjectedAlt;
            raw.injected = (info.flags & LLKHF_INJECTED) != 0;
            raw.extra_info = info.dwExtraInfo;
            const LockState locks{(GetKeyState(VK_NUMLOCK) & 1) != 0,
                                  (GetKeyState(VK_CAPITAL) & 1) != 0,
                                  (GetKeyState(VK_SCROLL) & 1) != 0};
            const std::optional<std::uint8_t> os =
                raw.scan_code == 0 ? std::optional(os_modifiers()) : std::nullopt;
            const auto event = translator_->translate(raw, locks, clock_(), os);
            // Our own paste and copy keys pass unseen: no shortcut fires on FioriVoce's Ctrl+V,
            // and the user's held keys stay as they were.
            if (!event || event->own_input()) {
                return true;
            }
            const bool pass = callback_(*event);
            if (pass) {
                translator_->passed(*event);
            }
            return pass;
        } catch (const std::exception& error) {
            kLog.error_with(error, "Error in keyboard hook");
        } catch (...) {
            kLog.error("Error in keyboard hook");
        }
        return true;
    }

    void run(std::promise<void>& ready) {
        detail::name_current_thread("fv-hotkey-hook");
        HHOOK hook = nullptr;
        try {
            // `keyboard`'s record of pressed keys is process-wide and survives a rehook; it
            // starts over only when the layout, and with it the names, changed.
            auto names = names_for_this_thread();
            if (names != names_ || !translator_) {
                names_ = std::move(names);
                // A key the table misses is named the first time it comes, as `keyboard` does.
                translator_ = std::make_unique<KeyEventTranslator>(*names_, &layout_event_name);
            }
            MSG message;
            // Creates this thread's message queue before anyone posts WM_QUIT to it.
            PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
            thread_id_ = GetCurrentThreadId();
            current_ = this;
            hook = SetWindowsHookExW(WH_KEYBOARD_LL, &WinKeyboardHook::proc,
                                     GetModuleHandleW(nullptr), 0);
            if (hook == nullptr) {
                throw std::runtime_error(
                    std::format("Windows refused the keyboard hook (error {}).", GetLastError()));
            }
            installed_ = true;
            ready.set_value();
        } catch (...) {
            current_ = nullptr;
            ready.set_exception(std::current_exception());
            return;
        }
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        UnhookWindowsHookEx(hook);
        current_ = nullptr;
    }

    // The hook procedure has no context argument; it runs on the installing thread only.
    static thread_local WinKeyboardHook* current_;

    MonotonicClock clock_;
    Callback callback_;
    std::thread thread_;
    std::atomic<DWORD> thread_id_{0};
    std::atomic<bool> installed_{false};
    std::shared_ptr<const KeyNameTable> names_;
    std::unique_ptr<KeyEventTranslator> translator_;
};

thread_local WinKeyboardHook* WinKeyboardHook::current_ = nullptr;

} // namespace

std::unique_ptr<KeyboardHook> make_keyboard_hook(MonotonicClock clock) {
    return std::make_unique<WinKeyboardHook>(std::move(clock));
}

} // namespace fv::hotkeys
