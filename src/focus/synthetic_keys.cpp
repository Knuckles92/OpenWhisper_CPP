#include "focus/synthetic_keys.h"

#include <algorithm>
#include <array>
#include <thread>

#include "base/executor.h"
#include "base/log.h"
#include "focus/catalog.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::focus {

namespace {

constexpr log::Logger kLog("services.synthetic_keys");

constexpr std::uint16_t kKeyC = 0x43;
constexpr std::uint16_t kKeyV = 0x56;

struct ModifierKeys {
    std::string_view name;
    std::array<std::uint16_t, 2> keys;
    std::size_t count;
};

// VK_CONTROL, VK_MENU, VK_SHIFT, VK_LWIN/VK_RWIN, in the keyboard library's modifier order.
constexpr std::array<ModifierKeys, 4> kWindowsModifiers{{
    {"ctrl", {VK_CONTROL_KEY, 0}, 1},
    {"alt", {VK_MENU_KEY, 0}, 1},
    {"shift", {VK_SHIFT_KEY, 0}, 1},
    {"windows", {VK_LWIN_KEY, VK_RWIN_KEY}, 2},
}};

// Editors whose Ctrl+C with nothing selected copies the caret's whole line, matched against an
// app's id and name, lowercased, with any path, ".exe" or ".app" removed.
constexpr std::string_view kCopyLineEditors[] = {
    "code",
    "code - insiders",
    "code-oss",
    "visual studio code",
    "com.microsoft.vscode",
    "cursor",
    "com.todesktop.230313mzl4w4u92",
    "windsurf",
    "com.exafunction.windsurf",
    "devenv",
    "visual studio",
    "sublime_text",
    "sublime text",
    "com.sublimetext.4",
    "com.sublimetext.3",
    "zed",
    "dev.zed.zed",
    "zeditor",
};

const py::Regex& jetbrains() {
    static const py::Regex pattern(R"(^(?:jetbrains-|com\.jetbrains\.))"
                                   R"(|^(?:idea|pycharm|webstorm|phpstorm|clion|rider|goland)"
                                   R"(|rubymine|datagrip|dataspell|rustrover|aqua|studio|fleet))"
                                   R"((?:64)?$|^(?:intellij idea|android studio)\b)");
    return pattern;
}

bool ends_with(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

std::set<std::string> names_of(const AppIdentity& identity) {
    std::set<std::string> names;
    for (const std::string* value : {&identity.app_id, &identity.name}) {
        if (py::strip(*value).empty()) {
            continue;
        }
        std::string name = py::lower(py::strip(*value));
        std::ranges::replace(name, '\\', '/');
        name = name.substr(name.rfind('/') == std::string::npos ? 0 : name.rfind('/') + 1);
        for (std::string_view suffix : {".exe", ".app"}) {
            if (ends_with(name, suffix)) {
                name.resize(name.size() - suffix.size());
            }
        }
        names.insert(std::move(name));
    }
    return names;
}

std::string surface_of(const AppIdentity& identity, const SurfaceLookup& surface) {
    try {
        return surface ? surface(identity) : catalog_surface(identity);
    } catch (const std::exception&) {
        kLog.warning("App catalogue unavailable");
        return "";
    }
}

} // namespace

std::vector<KeyStroke> chord_strokes(KeyboardPort& port, std::uint16_t modifier_vk,
                                     std::uint16_t key_vk) {
    const std::uint16_t modifier_scan = port.scan_code(modifier_vk);
    const std::uint16_t key_scan = port.scan_code(key_vk);
    return {
        {modifier_vk, modifier_scan, false},
        {key_vk, key_scan, false},
        {key_vk, key_scan, true},
        {modifier_vk, modifier_scan, true},
    };
}

std::optional<std::set<std::string>>
native_windows_modifiers(const std::function<bool(std::uint16_t vk)>& physical) {
    if (!physical) {
        return std::nullopt;
    }
    std::set<std::string> held;
    for (const auto& modifier : kWindowsModifiers) {
        for (std::size_t index = 0; index < modifier.count; ++index) {
            if (physical(modifier.keys[index])) {
                held.emplace(modifier.name);
                break;
            }
        }
    }
    return held;
}

bool windows_modifiers_held(const std::function<bool(std::uint16_t vk)>& physical,
                            const HookKeyState& hook) {
    if (const auto native = native_windows_modifiers(physical)) {
        return !native->empty();
    }
    if (!hook) {
        return false;
    }
    return std::ranges::any_of(kWindowsModifiers,
                               [&](const ModifierKeys& modifier) { return hook(modifier.name); });
}

bool wait_until_released(const std::function<bool()>& probe, std::chrono::milliseconds timeout,
                         const Clock& clock, const CancellableSleep& sleep) {
    if (!probe) {
        sleep(std::max(std::chrono::milliseconds(0), std::min(BLIND_WAIT, timeout)));
        return true;
    }
    const auto deadline = clock.steady() + std::max(std::chrono::milliseconds(0), timeout);
    for (;;) {
        try {
            if (!probe()) {
                return true;
            }
        } catch (const std::exception&) {
            kLog.debug("Couldn't read the modifier keys");
            return true;
        }
        if (clock.steady() >= deadline) {
            return false;
        }
        if (sleep(MODIFIER_POLL)) {
            return false; // Cancelled: the keys may still be down.
        }
    }
}

NativeSyntheticKeys::NativeSyntheticKeys(std::shared_ptr<KeyboardPort> port, HookKeyState hook,
                                         const Clock* clock, CancellableSleep sleep)
    : port_(std::move(port)), hook_(std::move(hook)),
      clock_(clock != nullptr ? clock : &system_clock()), sleep_(std::move(sleep)) {}

void NativeSyntheticKeys::send_chord(std::uint16_t key_vk) {
    const auto strokes = chord_strokes(*port_, VK_CONTROL_KEY, key_vk);
    const std::size_t sent = port_->send(strokes);
    if (sent != strokes.size()) {
        // Windows drops input aimed at a more privileged window without an error.
        kLog.debug("{} of {} synthetic keystrokes were taken", sent, strokes.size());
    }
}

void NativeSyntheticKeys::send_paste() {
    send_chord(kKeyV);
}

void NativeSyntheticKeys::send_copy() {
    send_chord(kKeyC);
}

std::function<bool()> NativeSyntheticKeys::probe() const {
    std::function<bool(std::uint16_t)> physical;
    if (port_->reads_key_state()) {
        physical = [port = port_](std::uint16_t vk) {
            return port->key_down(vk);
        };
    } else if (!hook_) {
        return {};
    }
    return [physical, hook = hook_] {
        return windows_modifiers_held(physical, hook);
    };
}

bool NativeSyntheticKeys::modifiers_held() {
    const auto check = probe();
    return check && check();
}

bool NativeSyntheticKeys::wait_for_modifiers_released(std::chrono::milliseconds timeout,
                                                      const CancelToken& cancel) {
    assert_not_gui_thread();
    const CancellableSleep sleep = sleep_ ? sleep_ : [&cancel](std::chrono::milliseconds pause) {
        return cancel.wait_for(pause);
    };
    return wait_until_released(probe(), timeout, *clock_, sleep);
}

std::unique_ptr<SyntheticKeys> make_synthetic_keys() {
    return std::make_unique<NativeSyntheticKeys>(make_keyboard_port());
}

std::string catalog_surface(const AppIdentity& identity) {
    if (identity.is_self) {
        return "";
    }
    const auto kind = catalog::classify(identity);
    return kind ? kind->surface : std::string();
}

bool is_terminal(const std::optional<AppIdentity>& identity, const SurfaceLookup& surface) {
    if (!identity) {
        return false;
    }
    return surface_of(*identity, surface) == catalog::TERMINAL;
}

bool copies_line_without_selection(const std::optional<AppIdentity>& identity,
                                   const SurfaceLookup& surface) {
    if (!identity) {
        return false;
    }
    try {
        const auto names = names_of(*identity);
        for (const auto& name : names) {
            if (std::ranges::find(kCopyLineEditors, std::string_view(name)) !=
                    std::end(kCopyLineEditors) ||
                jetbrains().search(name)) {
                return true;
            }
        }
    } catch (const std::exception&) {
        kLog.warning("Editor check failed");
        return false;
    }
    return surface_of(*identity, surface) == catalog::CODE;
}

} // namespace fv::focus
