#include "focus/foreground.h"

#include <format>

#include "base/identity.h"
#include "focus/catalog.h"
#include "pyport/text.h"

namespace fv::focus {

namespace {

constexpr std::string_view kUwpFrameHost = "applicationframehost.exe";
constexpr std::string_view kConsoleHost = "conhost.exe";

bool is_console_class(std::string_view name) {
    return name == "ConsoleWindowClass" || name == "PseudoConsoleWindow";
}

// Python's hex(): "0x1a2b".
std::string hex(std::uintptr_t value) {
    return std::format("{:#x}", value);
}

} // namespace

std::string self_app_id() {
    return py::lower(identity().product);
}

std::string self_app_name() {
    return std::string(identity().product);
}

std::optional<AppIdentity> foreground_identity(Win32Desktop& desktop, std::uint32_t own_pid) {
    const std::uintptr_t window = desktop.foreground_window();
    if (window == 0) {
        return std::nullopt;
    }
    std::uint32_t pid = desktop.window_pid(window);
    if (pid == 0) {
        return std::nullopt;
    }
    if (pid == own_pid) {
        return AppIdentity{.app_id = self_app_id(),
                           .name = self_app_name(),
                           .pid = pid,
                           .window = hex(window),
                           .is_self = true,
                           .platform = "windows"};
    }
    std::string exe = desktop.image_name(pid);
    if (exe == kUwpFrameHost) {
        // Store apps draw inside a frame owned by ApplicationFrameHost; the app itself is the
        // process behind the frame's CoreWindow child.
        if (const auto child = desktop.uwp_app_window(window, pid)) {
            std::string child_exe = desktop.image_name(child->second);
            if (!child_exe.empty()) {
                pid = child->second;
                exe = std::move(child_exe);
            }
        }
    }
    if (is_console_class(desktop.class_name(window))) {
        // conhost draws classic consoles, but the window reports the console's first client
        // (wsl.exe, ssh.exe, python.exe...). Naming conhost keeps every terminal rule on it: no
        // text read, no line breaks, no copy.
        const catalog::KnownApp* known = catalog::lookup(exe);
        if (known == nullptr || known->surface != catalog::TERMINAL) {
            exe = std::string(kConsoleHost);
        }
    }
    if (exe.empty()) {
        return std::nullopt;
    }
    std::string hint =
        catalog::is_browser(exe) ? catalog::title_hint(exe, desktop.title(window)) : "";
    return AppIdentity{.app_id = exe,
                       .name = catalog::app_name(exe),
                       .pid = pid,
                       .window = hex(window),
                       .title_hint = std::move(hint),
                       .platform = "windows"};
}

} // namespace fv::focus
