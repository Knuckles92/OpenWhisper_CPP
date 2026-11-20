#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "base/utf8.h"
#include "command_support.h"
#include "focus/foreground.h"
#include "focus/synthetic_keys.h"

// A transform over a real Notepad selection: the selection is copied with a real Ctrl+C through
// the Windows clipboard and its rewrite pasted back with a real Ctrl+V. It takes over the
// keyboard, the clipboard and the foreground window, so only the owner runs it
// (ctest --preset windows-interactive); the model answers in memory.

using namespace fv::dictation;
using namespace fv::dictation::testing;
using fv::focus::FocusSnapshot;

namespace {

std::optional<std::string> read_clipboard_text() {
    if (!OpenClipboard(nullptr)) {
        return std::nullopt;
    }
    std::optional<std::string> text;
    if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
        if (const auto* wide = static_cast<const wchar_t*>(GlobalLock(data))) {
            text = fv::base_utf8::path_to_utf8(std::filesystem::path(std::wstring(wide)));
            GlobalUnlock(data);
        }
    }
    CloseClipboard();
    return text;
}

bool write_clipboard_text(const std::string& text) {
    const std::wstring wide = fv::base_utf8::path_from_utf8(text).wstring();
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, (wide.size() + 1) * sizeof(wchar_t));
    if (memory == nullptr) {
        return false;
    }
    if (auto* target = static_cast<wchar_t*>(GlobalLock(memory))) {
        std::copy(wide.c_str(), wide.c_str() + wide.size() + 1, target);
        GlobalUnlock(memory);
    }
    if (!OpenClipboard(nullptr)) {
        GlobalFree(memory);
        return false;
    }
    EmptyClipboard();
    const bool written = SetClipboardData(CF_UNICODETEXT, memory) != nullptr;
    CloseClipboard();
    if (!written) {
        GlobalFree(memory);
    }
    return written;
}

// The Windows clipboard, plain: stages without keeping the user's text, and reads a selection
// by watching the clipboard's sequence number after the copy. GUI thread.
class WindowsClipboard final : public fv::focus::ClipboardPort {
public:
    fv::focus::StageResult stage_text(std::string text, std::optional<std::string>, bool) override {
        return fv::focus::StageResult{write_clipboard_text(text), std::nullopt, false};
    }
    bool schedule_restore(const fv::focus::ClipboardLease&, std::chrono::milliseconds) override {
        return false;
    }
    bool commit_text(const fv::focus::ClipboardLease&, std::string text) override {
        return write_clipboard_text(text);
    }
    fv::focus::RestoreOutcome restore_now(const fv::focus::ClipboardLease&) override {
        return fv::focus::RestoreOutcome::Skipped;
    }
    void cleanup() override {}
    void request_prefetch(std::chrono::milliseconds) override {}
    void discard_prefetch() override {}
    void capture_selection(std::function<void()> send_copy,
                           std::function<void(std::optional<std::string>)> callback,
                           std::chrono::milliseconds timeout) override {
        const DWORD before = GetClipboardSequenceNumber();
        try {
            send_copy();
        } catch (const std::exception&) {
            callback(std::string());
            return;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (GetClipboardSequenceNumber() == before &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (GetClipboardSequenceNumber() == before) {
            callback(std::string());
            return;
        }
        callback(read_clipboard_text().value_or(std::string()));
    }
    bool write_text(std::string text, std::string) override { return write_clipboard_text(text); }
    bool write_files(fv::focus::ClipboardFiles) override { return false; }
};

void type_text(const std::wstring& text) {
    std::vector<INPUT> inputs;
    for (const wchar_t ch : text) {
        for (const DWORD flags :
             {DWORD{KEYEVENTF_UNICODE}, DWORD{KEYEVENTF_UNICODE | KEYEVENTF_KEYUP}}) {
            INPUT input{};
            input.type = INPUT_KEYBOARD;
            input.ki.wScan = ch;
            input.ki.dwFlags = flags;
            inputs.push_back(input);
        }
    }
    SendInput(static_cast<UINT>(inputs.size()), inputs.data(), sizeof(INPUT));
}

void select_all() {
    INPUT inputs[4]{};
    for (auto& input : inputs) {
        input.type = INPUT_KEYBOARD;
    }
    inputs[0].ki.wVk = VK_CONTROL;
    inputs[1].ki.wVk = 'A';
    inputs[2].ki.wVk = 'A';
    inputs[2].ki.dwFlags = KEYEVENTF_KEYUP;
    inputs[3].ki.wVk = VK_CONTROL;
    inputs[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, inputs, sizeof(INPUT));
}

} // namespace

// The Command Mode acceptance on the desktop: Notepad's selected text is read with Ctrl+C once
// the shortcut's keys are up, rewritten, and pasted into the same app.
TEST_CASE("a transform rewrites a real Notepad selection in place", "[interactive]") {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    std::wstring command = L"notepad.exe";
    REQUIRE(CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                           &startup, &process));
    // Ends the Notepad this test started on every way out, a failed REQUIRE included.
    struct Started {
        PROCESS_INFORMATION& process;
        ~Started() {
            TerminateProcess(process.hProcess, 0);
            CloseHandle(process.hThread);
            CloseHandle(process.hProcess);
        }
    } started{process};
    WaitForInputIdle(process.hProcess, 10000);
    std::optional<fv::focus::AppIdentity> notepad;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline) {
        notepad = fv::focus::foreground_identity();
        if (notepad && notepad->app_id == "notepad.exe") {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    REQUIRE(notepad);
    REQUIRE(notepad->app_id == "notepad.exe");
    type_text(L"teh quick draft");
    select_all();
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    WindowsClipboard clipboard;
    auto keys = fv::focus::make_synthetic_keys();
    CommandRigOptions options;
    options.real_clipboard = &clipboard;
    options.real_keys = keys.get();
    {
        CommandRig rig(options);
        // UI Automation's answer is left out, so the selection is copied.
        rig.capture.fake(FocusSnapshot{notepad, std::nullopt}, notepad);
        rig.model->answer("The quick draft.");

        rig.commands->run_transform("polish");
        REQUIRE(rig.settle([&] { return rig.jobs.queued() > 0; }, std::chrono::seconds(5)));
        rig.drain();

        const auto completed = rig.completed();
        REQUIRE(completed.size() == 1);
        CHECK(completed[0].raw == std::optional<std::string>("teh quick draft"));
        CHECK(rig.notices() == std::vector<std::string>{"Rewritten (done)"});
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    select_all();
    WindowsClipboard check;
    std::optional<std::string> pasted;
    check.capture_selection([&keys] { keys->send_copy(); },
                            [&pasted](std::optional<std::string> text) { pasted = text; },
                            std::chrono::milliseconds(700));
    CHECK(pasted == std::optional<std::string>("The quick draft."));
}

#endif
