// Ports the Windows parts of tests/test_personalize_s1_identity.py, the classic-console naming
// tests of tests/test_personalize_fix_f3_console_terminal.py and
// test_ui_automation_marks_a_password_edit_as_refused of
// tests/test_personalize_fix_f2_selection_blocked.py. The identity logic runs on a fake
// Win32Desktop; the UI Automation tests read a hidden window of this process on its own message
// loop, never focused and never another application's.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <atomic>
#include <chrono>
#include <map>
#include <thread>
#include <tuple>
#include <typeinfo>

#include "focus/catalog.h"
#include "focus/focus_capture.h"
#include "focus/foreground.h"
#include "focus/synthetic_keys.h"
#include "focus/testing/fake_platform.h"
#include "focus_support.h"

#ifdef _WIN32
#include <windows.h>

#include "focus/uia_text_reader.h"
#endif

using namespace fv;
using namespace fv::focus;
using namespace std::chrono_literals;

namespace {

constexpr std::uint32_t kOwnPid = 4000;

class FakeWin32 final : public Win32Desktop {
public:
    std::uintptr_t window = 0x100;
    std::uint32_t pid = 500;
    std::string exe = "chrome.exe";
    std::string window_class;
    std::string title_text;
    std::optional<std::pair<std::uintptr_t, std::uint32_t>> uwp;
    std::map<std::uint32_t, std::string> images;
    std::vector<std::uintptr_t> titles;

    std::uintptr_t foreground_window() override { return window; }
    std::uint32_t window_pid(std::uintptr_t) override { return pid; }
    std::string class_name(std::uintptr_t) override { return window_class; }
    std::string title(std::uintptr_t hwnd) override {
        titles.push_back(hwnd);
        return title_text;
    }
    std::string image_name(std::uint32_t of) override {
        const auto found = images.find(of);
        return found != images.end() ? found->second : exe;
    }
    std::optional<std::pair<std::uintptr_t, std::uint32_t>> uwp_app_window(std::uintptr_t,
                                                                           std::uint32_t) override {
        return uwp;
    }
};

} // namespace

// from
// tests/test_personalize_s1_identity.py::test_windows_names_the_foreground_app_and_a_browser_site
TEST_CASE("windows names the foreground app and a browser site", "[test_personalize_s1_identity]") {
    FakeWin32 fake;
    fake.title_text = "Inbox (2) - me@example.com - Gmail - Google Chrome";

    const auto found = foreground_identity(fake, kOwnPid);

    CHECK(found == AppIdentity{.app_id = "chrome.exe",
                               .name = "Chrome",
                               .pid = 500,
                               .window = "0x100",
                               .title_hint = "Gmail",
                               .platform = "windows"});
    CHECK(fake.titles == std::vector<std::uintptr_t>{0x100});
}

// from tests/test_personalize_s1_identity.py::test_windows_reads_no_title_outside_browsers
TEST_CASE("windows reads no title outside browsers", "[test_personalize_s1_identity]") {
    FakeWin32 fake;
    fake.exe = "winword.exe";
    fake.title_text = "Secret plans - Word";

    const auto found = foreground_identity(fake, kOwnPid);

    REQUIRE(found);
    CHECK(std::tuple(found->app_id, found->name, found->title_hint) ==
          std::tuple(std::string("winword.exe"), std::string("Word"), std::string()));
    CHECK(fake.titles.empty());
}

// from tests/test_personalize_s1_identity.py::test_windows_resolves_store_apps_behind_their_frame
TEST_CASE("windows resolves store apps behind their frame", "[test_personalize_s1_identity]") {
    FakeWin32 fake;
    fake.exe = "applicationframehost.exe";
    fake.uwp = std::pair<std::uintptr_t, std::uint32_t>{0x200, 777};
    fake.images = {{777, "whatsapp.root.exe"}};

    const auto found = foreground_identity(fake, kOwnPid);

    REQUIRE(found);
    CHECK(std::tuple(found->app_id, found->name, found->pid) ==
          std::tuple(std::string("whatsapp.root.exe"), std::string("WhatsApp"),
                     std::optional<std::uint32_t>(777)));
}

// from
// tests/test_personalize_s1_identity.py::test_windows_marks_openwhisper_itself_without_reading_more
TEST_CASE("windows marks openwhisper itself without reading more",
          "[test_personalize_s1_identity]") {
    FakeWin32 fake;
    fake.pid = kOwnPid;
    fake.exe = "python.exe";

    const auto found = foreground_identity(fake, kOwnPid);

    REQUIRE(found);
    CHECK(found->is_self);
    CHECK(found->app_id == self_app_id());
    CHECK(found->name == self_app_name());
    CHECK(fake.titles.empty());
}

// from tests/test_personalize_s1_identity.py::test_windows_unknown_foreground_is_none
TEST_CASE("windows unknown foreground is none", "[test_personalize_s1_identity]") {
    const int which = GENERATE(0, 1, 2);
    FakeWin32 fake;
    if (which == 0) {
        fake.window = 0;
    } else if (which == 1) {
        fake.pid = 0;
    } else {
        fake.exe = "";
    }
    CAPTURE(which);
    CHECK_FALSE(foreground_identity(fake, kOwnPid));
}

// from tests/test_personalize_s1_identity.py::test_windows_real_identity_is_quick_and_never_raises
// Reads the real foreground window.
TEST_CASE("windows real identity is quick and never raises",
          "[test_personalize_s1_identity][interactive]") {
    foreground_identity();
    const auto started = std::chrono::steady_clock::now();
    std::optional<AppIdentity> found;
    for (int index = 0; index < 20; ++index) {
        found = foreground_identity();
    }
    CHECK((std::chrono::steady_clock::now() - started) / 20 < 20ms);
    CHECK((!found || !found->app_id.empty()));
}

// --- tests/test_personalize_fix_f3_console_terminal.py ----------------------------------------

namespace {

constexpr std::uintptr_t kConsoleWindow = 0x10876;
// Never this process's own.
constexpr std::uint32_t kClientPid = kOwnPid + 4;

std::optional<AppIdentity> console_identity(const std::string& exe,
                                            const std::string& window_class) {
    FakeWin32 fake;
    fake.window = kConsoleWindow;
    // GetWindowThreadProcessId on a classic console reports its first client process, not the
    // conhost.exe that draws the window.
    fake.pid = kClientPid;
    fake.exe = exe;
    fake.window_class = window_class;
    return foreground_identity(fake, kOwnPid);
}

bool text_allowed(const AppIdentity& identity) {
    auto platform = std::make_shared<FakeFocusPlatform>(identity);
    FocusCaptureOptions options;
    options.metrics = [](const CaptureSample&) {
    };
    FocusCapture service(platform, std::move(options));
    const auto snapshot = service.request(true, false)->wait_for(5000ms);
    return !snapshot || snapshot->text.has_value() || !platform->reads().empty() ||
           service.state().has_worker;
}

} // namespace

// from
// tests/test_personalize_fix_f3_console_terminal.py::test_a_classic_console_gets_every_terminal_protection
TEST_CASE("a classic console gets every terminal protection",
          "[test_personalize_fix_f3_console_terminal]") {
    const std::string client = GENERATE(as<std::string>{}, "ubuntu.exe", "wsl.exe", "ssh.exe",
                                        "python.exe", "claude.exe", "");
    const std::string window_class =
        GENERATE(as<std::string>{}, "ConsoleWindowClass", "PseudoConsoleWindow");
    CAPTURE(client, window_class);

    const auto found = console_identity(client, window_class);

    REQUIRE(found);
    CHECK(found->pid == kClientPid);
    CHECK(found->window == "0x10876");
    CHECK(catalog::is_terminal(found));
    CHECK(is_terminal(found));
    CHECK_FALSE(text_allowed(*found));
    CHECK(catalog_surface(*found) == catalog::TERMINAL);
}

// from
// tests/test_personalize_fix_f3_console_terminal.py::test_a_console_run_by_a_known_shell_keeps_its_name
TEST_CASE("a console run by a known shell keeps its name",
          "[test_personalize_fix_f3_console_terminal]") {
    const auto [client, name] = GENERATE(table<std::string, std::string>({
        {"powershell.exe", "PowerShell"},
        {"pwsh.exe", "PowerShell"},
        {"cmd.exe", "Console"},
    }));
    CAPTURE(client);

    const auto found = console_identity(client, "ConsoleWindowClass");

    REQUIRE(found);
    CHECK(std::tuple(found->app_id, found->name) == std::tuple(client, name));
    CHECK(is_terminal(found));
}

// from tests/test_personalize_fix_f3_console_terminal.py::test_other_windows_keep_their_own_app
TEST_CASE("other windows keep their own app", "[test_personalize_fix_f3_console_terminal]") {
    const auto found = console_identity("python.exe", "TkTopLevel");

    REQUIRE(found);
    CHECK(found->app_id == "python.exe");
    CHECK_FALSE(catalog::is_terminal(found));
}

// --- Windows UI Automation (real, on a hidden window of this process) ------------------------

#ifdef _WIN32

namespace {

const std::wstring kSample = L"Meeting notes. Lunch with Siobhan about the Q3 roadmap tomorrow.";

// A hidden popup parent with one child EDIT, off screen and never shown, on its own message
// loop: UI Automation's Win32 proxy talks to the EDIT through window messages.
class HiddenEdit {
public:
    HiddenEdit(std::wstring text, DWORD extra_style, std::optional<std::pair<int, int>> selection)
        : pump_([this, text = std::move(text), extra_style, selection] {
              HWND parent =
                  CreateWindowExW(WS_EX_TOOLWINDOW, L"STATIC", L"fv-test", WS_POPUP, -32000, -32000,
                                  400, 200, nullptr, nullptr, nullptr, nullptr);
              HWND edit =
                  CreateWindowExW(0, L"EDIT", text.c_str(), WS_CHILD | WS_VISIBLE | extra_style, 0,
                                  0, 400, 200, parent, nullptr, nullptr, nullptr);
              if (selection) {
                  SendMessageW(edit, EM_SETSEL, static_cast<WPARAM>(selection->first),
                               static_cast<LPARAM>(selection->second));
              }
              edit_ = reinterpret_cast<std::uintptr_t>(edit);
              ready_.set();
              MSG message;
              while (!stop_.is_set()) {
                  while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
                      TranslateMessage(&message);
                      DispatchMessageW(&message);
                  }
                  MsgWaitForMultipleObjects(0, nullptr, FALSE, 20, QS_ALLINPUT);
              }
              DestroyWindow(parent);
          }) {}

    ~HiddenEdit() {
        stop_.set();
        pump_.join();
    }

    std::uintptr_t window() {
        REQUIRE(ready_.wait(5000ms));
        return edit_;
    }

private:
    FakeEvent ready_;
    FakeEvent stop_;
    std::atomic<std::uintptr_t> edit_{0};
    std::thread pump_;
};

struct Reads {
    std::optional<TextContext> full;
    std::optional<TextContext> selection;
    std::optional<TextContext> text;
    std::string error;
};

// Reads on a thread of its own, as the capture service's text thread does.
template <class Body> void read_on_own_thread(Body body) {
    FakeEvent done;
    std::thread reading([&] {
        body();
        done.set();
    });
    // UI Automation's own timeouts end a stuck call well inside this.
    const bool answered = done.wait(10000ms);
    reading.join();
    REQUIRE(answered);
}

} // namespace

// from tests/test_personalize_s1_identity.py::test_ui_automation_reads_a_hidden_edit_control
TEST_CASE("ui automation reads a hidden edit control", "[test_personalize_s1_identity][platform]") {
    const auto start = static_cast<int>(kSample.find(L"Siobhan"));
    HiddenEdit edit(kSample, ES_MULTILINE, std::pair<int, int>{start, start + 7});
    const auto window = edit.window();
    Reads results;

    read_on_own_thread([&] {
        try {
            UiaTextReader reader;
            results.full = reader.read_window(window, true, true);
            results.selection = reader.read_window(window, false, true);
            results.text = reader.read_window(window, true, false);
        } catch (const std::exception& error) {
            results.error = typeid(error).name();
        }
    });

    REQUIRE(results.error.empty());
    CHECK(results.full == TextContext{.before = "Meeting notes. Lunch with ",
                                      .selected = "Siobhan",
                                      .after = " about the Q3 roadmap tomorrow.",
                                      .caret_known = true,
                                      .selection_known = true,
                                      .source = "uia"});
    CHECK(results.selection ==
          TextContext{.selected = "Siobhan", .selection_known = true, .source = "uia"});
    // Not asked for, the selection only places the text around it.
    CHECK(results.text == TextContext{.before = "Meeting notes. Lunch with ",
                                      .after = " about the Q3 roadmap tomorrow.",
                                      .caret_known = true,
                                      .selection_known = false,
                                      .source = "uia"});
}

// from
// tests/test_personalize_fix_f2_selection_blocked.py::test_ui_automation_marks_a_password_edit_as_refused
TEST_CASE("ui automation marks a password edit as refused",
          "[test_personalize_fix_f2_selection_blocked][platform]") {
    HiddenEdit edit(L"hunter2", ES_PASSWORD, std::nullopt);
    const auto window = edit.window();
    std::optional<TextContext> context;
    std::string error;

    read_on_own_thread([&] {
        try {
            UiaTextReader reader;
            context = reader.read_window(window, false, true);
        } catch (const std::exception& failure) {
            error = typeid(failure).name();
        }
    });

    REQUIRE(error.empty());
    CHECK(context == TextContext{.source = "uia", .blocked = true});
}

#endif
