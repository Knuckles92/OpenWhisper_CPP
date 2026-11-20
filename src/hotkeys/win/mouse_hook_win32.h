#pragma once

#include <windows.h>

#include <functional>
#include <memory>

#include "hotkeys/mouse_hook.h"

// The Win32 calls of the side-button hook's threads, replaceable so tests drive the real hook and
// dispatch threads without a global hook (test_personalize_s6_backends' _FakeUser32 and
// _FakeKernel32).
namespace fv::hotkeys {

struct MouseHookWin32 {
    // SetWindowsHookExW(WH_MOUSE_LL, proc, GetModuleHandleW(nullptr), 0); null when refused.
    std::function<HHOOK(HOOKPROC proc)> set_hook;
    std::function<BOOL(HHOOK hook)> unhook;
    // GetMessageW(message, nullptr, 0, 0). Windows calls a low-level hook's procedure from inside
    // it, on the installing thread.
    std::function<BOOL(MSG* message)> get_message;
    // PostThreadMessageW(thread_id, message, 0, 0), from stop()'s caller.
    std::function<BOOL(DWORD thread_id, UINT message)> post_thread_message;
    // CallNextHookEx(nullptr, code, wparam, lparam), inside the hook. Must not allocate.
    std::function<LRESULT(int code, WPARAM wparam, LPARAM lparam)> call_next;
    // GetAsyncKeyState, inside the hook. Must not allocate.
    std::function<SHORT(int vk)> get_async_key_state;
    std::function<DWORD()> current_thread_id;
    // Only a source that observes clicks calls these. WindowFromPoint and
    // GetWindowThreadProcessId run inside the hook and must not allocate; GetCurrentProcessId
    // runs once per start (empty: the real one).
    std::function<HWND(POINT point)> window_from_point;
    std::function<DWORD(HWND window, DWORD* process_id)> window_thread_process_id;
    std::function<DWORD()> current_process_id;
};

// The real user32 and kernel32 calls.
MouseHookWin32 real_mouse_hook_win32();

// make_mouse_hook_source over `win32`.
std::unique_ptr<MouseHookSource> make_mouse_hook_source(MonotonicClock clock, MouseHookWin32 win32);
// The click-observing source over `win32`.
std::unique_ptr<MouseHookSource> make_mouse_hook_source(ClickObserver on_click,
                                                        MonotonicClock clock, MouseHookWin32 win32);

} // namespace fv::hotkeys
