#include <catch2/catch_test_macros.hpp>

// A selection the job did not ask for never hides the text around it
// (tests/test_automation_r2_selection.py). Python drives the macOS reader over a fake
// Accessibility API; the Windows reader is driven over a hidden EDIT of this process, as
// tests/focus does.

#ifdef _WIN32

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <typeinfo>
#include <utility>

#include "focus/snapshot.h"
#include "focus/uia_text_reader.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace std::chrono_literals;
using fv::focus::TextContext;

namespace {

// A hidden popup parent with one child EDIT holding `text` with [start, end) selected, off
// screen and never shown, on its own message loop: UI Automation's Win32 proxy talks to the EDIT
// through window messages.
class HiddenEdit {
public:
    HiddenEdit(std::wstring text, int start, int end)
        : pump_([this, text = std::move(text), start, end] {
              HWND parent =
                  CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"fv-test", WS_POPUP, -32000, -32000,
                                  400, 200, nullptr, nullptr, nullptr, nullptr);
              HWND edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_MULTILINE, 0,
                                          0, 400, 200, parent, nullptr, nullptr, nullptr);
              // The text is longer than an EDIT takes by default.
              SendMessageW(edit, EM_SETLIMITTEXT, 0, 0);
              SetWindowTextW(edit, text.c_str());
              SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(start), static_cast<LPARAM>(end));
              edit_ = reinterpret_cast<std::uintptr_t>(edit);
              ready_.set_value();
              MSG message;
              while (!stop_.load()) {
                  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                      TranslateMessage(&message);
                      DispatchMessageW(&message);
                  }
                  MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
              }
              DestroyWindow(parent);
          }) {}

    ~HiddenEdit() {
        stop_ = true;
        pump_.join();
    }
    HiddenEdit(const HiddenEdit&) = delete;
    HiddenEdit& operator=(const HiddenEdit&) = delete;

    std::uintptr_t window() {
        REQUIRE(ready_.get_future().wait_for(5s) == std::future_status::ready);
        return edit_;
    }

private:
    std::promise<void> ready_;
    std::atomic<bool> stop_{false};
    std::atomic<std::uintptr_t> edit_{0};
    std::thread pump_;
};

} // namespace

TEST_CASE("test_a_long_selection_doesnt_hide_the_text_around_it_when_not_asked",
          "[test_automation_r2_selection][platform]") {
    const std::size_t length = fv::focus::MAX_SELECTION_CHARS + 5;
    const std::wstring text = L"before " + std::wstring(length, L'x') + L" after";
    HiddenEdit edit(text, 7, static_cast<int>(7 + length));
    const auto window = edit.window();
    std::optional<TextContext> context;
    std::string error;

    // On a thread of its own, as the capture service's text thread reads.
    std::thread reading([&] {
        try {
            fv::focus::UiaTextReader reader;
            context = reader.read_window(window, true, false);
        } catch (const std::exception& failure) {
            error = typeid(failure).name();
        }
    });
    reading.join();

    REQUIRE(error.empty());
    REQUIRE(context);
    CHECK(context->before == "before ");
    CHECK(context->after == " after");
    CHECK(context->selected.empty());
}

#endif
