#include "hotkeys/hotkey_manager.h"

#include <cstdint>
#include <exception>
#include <thread>
#include <utility>

#include "base/log.h"

namespace fv::hotkeys {

namespace {

constexpr log::Logger kLog{"services._hotkey_keyboard"};
constexpr log::Logger kCommonLog{"services._hotkey_common"};

void run_guarded(const std::function<void()>& call) {
    try {
        call();
    } catch (const std::exception& error) {
        kCommonLog.error_with(error, "Hotkey handler failed");
    } catch (...) {
        kCommonLog.error("Hotkey handler failed");
    }
}

void spawn_thread(std::function<void()> call) {
    std::thread([call = std::move(call)] { run_guarded(call); }).detach();
}

} // namespace

const std::function<void()>* Callbacks::press_action(std::string_view action) const {
    return const_cast<Callbacks*>(this)->press_action(action);
}

std::function<void()>* Callbacks::press_action(std::string_view action) {
    // _hotkey_common.PRESS_ACTIONS: each action and the callback attribute it calls.
    if (action == "meeting_toggle")
        return &on_meeting_toggle;
    if (action == "cancel")
        return &on_cancel;
    if (action == "minimize_tray")
        return &on_minimize_tray;
    if (action == "scratchpad_toggle")
        return &on_scratchpad_toggle;
    if (action == "cycle_language")
        return &on_cycle_language;
    if (action == "paste_last_original")
        return &on_paste_last_original;
    if (action == "route_record")
        return &on_route_record;
    if (action == "walkthrough")
        return &on_walkthrough;
    if (action == "screenshot")
        return &on_screenshot;
    if (action == "paste_walkthrough")
        return &on_paste_walkthrough;
    return nullptr;
}

HotkeyManager::HotkeyManager(std::map<std::string, std::string> hotkeys)
    : HotkeyManager(std::move(hotkeys), make_keyboard_hook(), monotonic_clock(),
                    make_mouse_hook_source()) {}

HotkeyManager::HotkeyManager(std::map<std::string, std::string> hotkeys,
                             std::unique_ptr<KeyboardHook> hook, MonotonicClock clock,
                             std::unique_ptr<MouseHookSource> mouse_hook)
    : matcher_(std::move(hotkeys)), hook_(std::move(hook)), clock_(std::move(clock)),
      spawner_(std::make_shared<const Spawner>(&spawn_thread)), mouse_hook_(std::move(mouse_hook)) {
    try {
        setup_keyboard_hook();
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            backend_available_ = false;
            backend_name_ = "unavailable";
            backend_error_ = error.what();
        }
        kLog.warning("Global hotkeys unavailable; continuing with in-window controls: {}",
                     error.what());
    }
}

HotkeyManager::~HotkeyManager() {
    cleanup();
}

void HotkeyManager::setup_keyboard_hook() {
    {
        // A rehook may miss a key-up while unhooked, so forget keyboard holds. A side button's
        // hold carries over: its hook still reports the release.
        std::lock_guard lock(mutex_);
        matcher_.forget_held_keys(true);
    }
    install_hook();
    sync_mouse_hook();
}

void HotkeyManager::sync_mouse_hook() {
    // The wish is read under mouse_mutex_, so two threads syncing at once cannot leave the hook
    // in the state the earlier one read.
    std::lock_guard mouse(mouse_mutex_);
    if (!mouse_hook_) {
        return;
    }
    bool wanted = false;
    {
        std::lock_guard lock(mutex_);
        wanted = matcher_.wants_mouse_hook();
    }
    try {
        if (wanted) {
            mouse_hook_->start(
                [this](MouseButton button, std::uint8_t modifiers) {
                    return claims_mouse_button(button, modifiers);
                },
                [this](const MouseEvent& event) { on_mouse_button(event); });
        } else {
            mouse_hook_->stop();
        }
    } catch (const std::exception& error) {
        kLog.warning("Mouse button shortcuts unavailable: {}", error.what());
    }
}

void HotkeyManager::set_mouse_hook_source(std::unique_ptr<MouseHookSource> source) {
    std::unique_ptr<MouseHookSource> previous;
    {
        std::lock_guard mouse(mouse_mutex_);
        previous = std::exchange(mouse_hook_, std::move(source));
        if (previous) {
            try {
                previous->stop();
            } catch (const std::exception& error) {
                kLog.error("Error removing the mouse hook: {}", error.what());
            }
        }
    }
    previous.reset();
    sync_mouse_hook();
}

bool HotkeyManager::claims_mouse_button(MouseButton button, std::uint8_t modifiers) const {
    std::lock_guard lock(mutex_);
    return matcher_.claims_mouse_button(button, modifiers);
}

void HotkeyManager::on_mouse_button(const MouseEvent& event) {
    Decision decision;
    std::shared_ptr<const Callbacks> callbacks;
    std::shared_ptr<const std::map<std::string, FamilyCallback>> families;
    {
        std::lock_guard lock(mutex_);
        decision = matcher_.handle_mouse(event);
        callbacks = callbacks_;
        families = families_;
    }
    // A callback that throws ends here; the dispatch thread logs it ("Mouse event dispatch
    // failed") and goes on.
    for (const Effect& effect : decision.effects) {
        run(effect, *callbacks, *families);
    }
}

bool HotkeyManager::mouse_hook_running() const {
    std::lock_guard mouse(mouse_mutex_);
    return mouse_hook_ && mouse_hook_->running();
}

void HotkeyManager::install_hook() {
    try {
        hook_->install([this](const KeyEvent& event) { return on_event(event); });
    } catch (const std::exception& error) {
        std::lock_guard lock(mutex_);
        backend_available_ = false;
        backend_name_ = "unavailable";
        backend_error_ = error.what();
        throw;
    }
    std::lock_guard lock(mutex_);
    backend_available_ = true;
    backend_name_ = "keyboard";
    backend_error_.clear();
}

bool HotkeyManager::on_event(const KeyEvent& event) {
    Decision decision;
    std::shared_ptr<const Callbacks> callbacks;
    std::shared_ptr<const std::map<std::string, FamilyCallback>> families;
    {
        std::lock_guard lock(mutex_);
        decision = matcher_.handle(event);
        callbacks = callbacks_;
        families = families_;
    }
    try {
        for (const Effect& effect : decision.effects) {
            run(effect, *callbacks, *families);
        }
    } catch (const std::exception& error) {
        // `keyboard` lets an event through when its callback raised.
        kLog.error_with(error, "Error in keyboard hook");
        return true;
    } catch (...) {
        kLog.error("Error in keyboard hook");
        return true;
    }
    return !decision.suppress;
}

void HotkeyManager::run(const Effect& effect, const Callbacks& callbacks,
                        const std::map<std::string, FamilyCallback>& families) {
    std::shared_ptr<const Spawner> spawner;
    {
        std::lock_guard lock(mutex_);
        spawner = spawner_;
    }
    switch (effect.kind) {
    case Effect::Kind::RecordToggle:
        if (callbacks.on_record_toggle) {
            (*spawner)(callbacks.on_record_toggle);
        }
        break;
    case Effect::Kind::RecordPress:
        if (callbacks.on_record_press) {
            callbacks.on_record_press(effect.at);
        }
        break;
    case Effect::Kind::RecordRelease:
        if (callbacks.on_record_release) {
            callbacks.on_record_release(effect.at);
        }
        break;
    case Effect::Kind::CommandPress:
        if (callbacks.on_command_press) {
            callbacks.on_command_press(effect.at);
        }
        break;
    case Effect::Kind::CommandRelease:
        if (callbacks.on_command_release) {
            callbacks.on_command_release(effect.at);
        }
        break;
    case Effect::Kind::PressAction:
        if (const auto* callback = callbacks.press_action(effect.action); callback && *callback) {
            (*spawner)(*callback);
        }
        break;
    case Effect::Kind::Dynamic:
        if (const auto found = families.find(effect.family);
            found != families.end() && found->second) {
            (*spawner)([callback = found->second, id = effect.id] { callback(id); });
        }
        break;
    case Effect::Kind::Status:
        announce(effect.enabled, callbacks);
        break;
    }
}

void HotkeyManager::announce(bool enabled, const Callbacks& callbacks) {
    // _hotkey_common.notify_stt_toggle.
    const std::string status = enabled ? "STT Enabled" : "STT Disabled";
    if (callbacks.on_status_update_auto_hide) {
        callbacks.on_status_update_auto_hide(status);
    } else if (callbacks.on_status_update) {
        callbacks.on_status_update(status);
    }
    kCommonLog.info("STT has been {}", enabled ? "enabled" : "disabled");
}

void HotkeyManager::set_callbacks(Callbacks callbacks) {
    auto shared = std::make_shared<const Callbacks>(std::move(callbacks));
    std::lock_guard lock(mutex_);
    callbacks_ = std::move(shared);
}

void HotkeyManager::update_hotkeys(const std::map<std::string, std::string>& hotkeys) {
    {
        std::lock_guard lock(mutex_);
        matcher_.update_hotkeys(hotkeys);
    }
    cleanup();
    try {
        setup_keyboard_hook();
    } catch (const std::exception& error) {
        kLog.warning("Global hotkeys unavailable; continuing with in-window controls: {}",
                     error.what());
        return;
    }
    kLog.info("Hotkeys updated successfully");
}

void HotkeyManager::set_record_mode(TriggerMode mode) {
    std::lock_guard lock(mutex_);
    matcher_.set_record_mode(mode);
}

void HotkeyManager::set_dynamic_hotkeys(std::string_view family, const HotkeyList& hotkeys,
                                        FamilyCallback callback) {
    {
        std::lock_guard lock(mutex_);
        matcher_.set_dynamic_hotkeys(family, hotkeys);
        auto families = std::make_shared<std::map<std::string, FamilyCallback>>(*families_);
        (*families)[std::string(family)] = std::move(callback);
        families_ = std::move(families);
    }
    sync_mouse_hook();
}

void HotkeyManager::set_capture_suspended(bool suspended) {
    {
        std::lock_guard lock(mutex_);
        matcher_.set_capture_suspended(suspended);
    }
    sync_mouse_hook();
}

void HotkeyManager::rehook() {
    kLog.info("Re-registering keyboard hook...");
    try {
        cleanup();
    } catch (const std::exception& error) {
        kLog.warning("Error during rehook cleanup: {}", error.what());
    }
    try {
        setup_keyboard_hook();
        kLog.info("Keyboard hook re-registered successfully");
    } catch (const std::exception& error) {
        kLog.error("Failed to re-register keyboard hook: {}", error.what());
    }
}

void HotkeyManager::cleanup() {
    {
        std::lock_guard mouse(mouse_mutex_);
        try {
            if (mouse_hook_) {
                mouse_hook_->stop();
            }
        } catch (const std::exception& error) {
            kLog.error("Error removing the mouse hook: {}", error.what());
        }
    }
    try {
        hook_->uninstall();
    } catch (const std::exception& error) {
        kLog.error("Error cleaning up keyboard hooks: {}", error.what());
    }
}

bool HotkeyManager::keyboard_hold_active() const {
    return keyboard_hold_active(clock_());
}

bool HotkeyManager::keyboard_hold_active(double now) const {
    std::lock_guard lock(mutex_);
    return matcher_.keyboard_hold_active(now);
}

bool HotkeyManager::backend_available() const {
    std::lock_guard lock(mutex_);
    return backend_available_;
}

std::string HotkeyManager::backend_name() const {
    std::lock_guard lock(mutex_);
    return backend_name_;
}

std::string HotkeyManager::backend_error() const {
    std::lock_guard lock(mutex_);
    return backend_error_;
}

std::map<std::string, std::string> HotkeyManager::hotkeys() const {
    std::lock_guard lock(mutex_);
    return matcher_.hotkeys();
}

std::map<std::string, std::string> HotkeyManager::dynamic_hotkeys() const {
    std::lock_guard lock(mutex_);
    return matcher_.dynamic_hotkeys();
}

bool HotkeyManager::program_enabled() const {
    std::lock_guard lock(mutex_);
    return matcher_.program_enabled();
}

void HotkeyManager::set_program_enabled(bool enabled) {
    std::lock_guard lock(mutex_);
    matcher_.set_program_enabled(enabled);
}

void HotkeyManager::toggle_program_enabled() {
    Effect effect;
    std::shared_ptr<const Callbacks> callbacks;
    {
        std::lock_guard lock(mutex_);
        effect = matcher_.toggle_program_enabled();
        callbacks = callbacks_;
    }
    announce(effect.enabled, *callbacks);
}

bool HotkeyManager::capture_suspended() const {
    std::lock_guard lock(mutex_);
    return matcher_.capture_suspended();
}

void HotkeyManager::set_spawner(Spawner spawner) {
    auto shared = std::make_shared<const Spawner>(std::move(spawner));
    std::lock_guard lock(mutex_);
    spawner_ = std::move(shared);
}

} // namespace fv::hotkeys
