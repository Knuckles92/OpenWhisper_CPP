#include "dictation/hotkey_runtime.h"

#include <algorithm>
#include <exception>
#include <format>
#include <future>
#include <utility>

#include "base/desktop_session.h"
#include "base/log.h"
#include "focus/capture_service.h"
#include "hotkeys/grammar.h"
#include "hotkeys/matcher.h"
#include "pyport/text.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"

namespace fv::dictation {

namespace {

constexpr log::Logger kLog{"services.runtime.hotkeys"};

using Seconds = std::chrono::duration<double>;

// How often a release waiting for the stream looks again without being woken, as Python
// polls.
constexpr std::chrono::milliseconds kStreamPoll{10};
// How often a press waiting for the UI's answer checks that the runtime is still running.
constexpr std::chrono::milliseconds kAnswerPoll{20};

std::string dict_repr(const hotkeys::HotkeyList& hotkeys) {
    std::string text = "{";
    bool first = true;
    for (const auto& [action, hotkey] : hotkeys) {
        if (!first) {
            text += ", ";
        }
        first = false;
        text += py::repr(action);
        text += ": ";
        text += py::repr(hotkey);
    }
    return text + "}";
}

HotkeyRuntime::HotkeyMap to_map(const hotkeys::HotkeyList& hotkeys) {
    HotkeyRuntime::HotkeyMap map;
    for (const auto& [action, hotkey] : hotkeys) {
        map.insert_or_assign(action, hotkey);
    }
    return map;
}

// {**DEFAULT_HOTKEYS, **saved}: the defaults in their order, then saved actions they lack.
hotkeys::HotkeyList merge_hotkeys(const hotkeys::HotkeyList& defaults, const Json& saved) {
    hotkeys::HotkeyList merged = defaults;
    if (!saved.is_object()) {
        return merged;
    }
    for (const auto& [action, value] : saved.items()) {
        if (!value.is_string()) {
            continue; // the manager matches strings only
        }
        const auto found = std::find_if(merged.begin(), merged.end(),
                                        [&](const auto& entry) { return entry.first == action; });
        if (found != merged.end()) {
            found->second = value.get<std::string>();
        } else {
            merged.emplace_back(action, value.get<std::string>());
        }
    }
    return merged;
}

} // namespace

// The machine's view of the recording side. Dispatcher thread.
class HotkeyRuntime::Host final : public hotkeys::HoldMachine::Host {
public:
    explicit Host(HotkeyRuntime& runtime) : runtime_(runtime) {}

    bool is_recording() override {
        // Python's recorder.is_recording stays true until the previous capture lets go of the
        // recorder; here that is is_recording or, while a stream still closes, recorder_busy.
        return runtime_.control_.is_recording() || runtime_.control_.recorder_busy();
    }
    bool capture_canceled() override { return runtime_.control_.capture_canceled(); }
    bool hands_free_latch() override {
        return keys::recording_hands_free_latch.read(*runtime_.settings_.snapshot());
    }
    bool wait_until_recording(double timeout_s) override {
        const bool recording = runtime_.wait_until_recording(timeout_s);
        // Python's count of ended recordings moves the moment one ends, so the release that
        // waited here sees a recording that ended meanwhile.
        runtime_.sync_recordings_ended();
        return recording;
    }

    bool perform(const hotkeys::HoldMachine::Output& output) override {
        using Action = hotkeys::HoldMachine::Action;
        auto& control = runtime_.control_;
        switch (output.action) {
        case Action::Start: {
            const bool accepted =
                runtime_.call_on_ui([&control] { return control.start("hotkey"); }, false);
            runtime_.sync_recordings_ended();
            return accepted;
        }
        case Action::Stop:
            runtime_.call_on_ui(
                [&control] {
                    control.stop();
                    return true;
                },
                false);
            runtime_.sync_recordings_ended();
            return true;
        case Action::Cancel:
            runtime_.call_on_ui([&control] { return control.cancel(); }, false);
            runtime_.sync_recordings_ended();
            return true;
        case Action::Latch:
            runtime_.latch_status();
            runtime_.hands_free_changed.emit(true);
            return true;
        case Action::Unlatch:
            runtime_.hands_free_changed.emit(false);
            return true;
        case Action::ArmDeadline: {
            const std::uint64_t generation = output.generation;
            HotkeyRuntime* runtime = &runtime_;
            runtime_.dispatcher_->call_at(output.at, [runtime, generation] {
                runtime->sync_recordings_ended();
                runtime->machine_.deadline(generation);
            });
            return true;
        }
        case Action::CancelDeadline:
            runtime_.dispatcher_->cancel_deadline();
            return true;
        }
        return true;
    }

private:
    HotkeyRuntime& runtime_;
};

HotkeyRuntime::HotkeyRuntime(hotkeys::HotkeyManager& manager, RecordingControl& control,
                             SettingsStore& settings, Executor& ui, Clock& clock,
                             focus::SyntheticKeys& keys, std::function<bool()> on_wayland)
    : manager_(manager), control_(control), settings_(settings), ui_(ui), clock_(clock),
      keys_(keys),
      on_wayland_(on_wayland ? std::move(on_wayland)
                             : std::function<bool()>([] { return is_wayland_session(); })),
      host_(std::make_unique<Host>(*this)), machine_(*host_),
      expected_watchdog_time_(clock.steady() + HOTKEY_WATCHDOG_INTERVAL),
      dispatcher_(std::make_unique<hotkeys::OrderedDispatcher>("fv-hotkey-dispatch",
                                                               hotkeys::monotonic_clock(clock))) {
    // Python connects this in setup_hotkeys; a recording can only end through a start that
    // setup_hotkeys wired, so connecting at construction changes nothing and never connects
    // twice.
    recording_link_ = control_.recording_state_changed().connect(
        ui_, [this](bool recording) { on_recording_state_changed(recording); });
}

HotkeyRuntime::~HotkeyRuntime() {
    cleanup();
    guard_->close();
    recording_link_.disconnect();
    dispatcher_.reset();
}

void HotkeyRuntime::connect_signals() {
    stt_link_ =
        stt_state_changed.connect(ui_, [this](bool enabled) { on_stt_state_changed(enabled); });
}

void HotkeyRuntime::setup_hotkeys() {
    kLog.info("Setting up hotkeys...");
    // Backfill newly introduced default actions over the saved hotkeys, so existing users get
    // new shortcuts without reconfiguring: load_hotkey_settings returns the saved table
    // unmerged.
    const hotkeys::HotkeyList merged =
        merge_hotkeys(hotkeys::default_hotkeys(), load_hotkey_settings(settings_));
    const HotkeyMap map = to_map(merged);
    manager_.update_hotkeys(map);
    manager_.set_record_mode(
        hotkeys::trigger_mode_from(keys::recording_trigger_mode.read(*settings_.snapshot())));

    // The manager keeps its callbacks after the runtime is gone; the guard ends them, here and
    // in what they post to the UI executor.
    std::weak_ptr<CallbackGuard> weak = guard_;
    const auto guarded = [weak](std::function<void()> fn) {
        return [weak, fn = std::move(fn)] {
            if (auto guard = weak.lock()) {
                guard->run(fn);
            }
        };
    };
    const auto on_ui = [this, guarded](std::function<void()> fn) {
        return guarded([this, task = guarded(std::move(fn))] { ui_.post(task); });
    };
    hotkeys::Callbacks callbacks;
    callbacks.on_record_toggle = on_ui([this] { control_.toggle(); });
    callbacks.on_record_press = [weak, this](double at) {
        if (auto guard = weak.lock()) {
            guard->run([this, at] { queue_record_press(at); });
        }
    };
    callbacks.on_record_release = [weak, this](double at) {
        if (auto guard = weak.lock()) {
            guard->run([this, at] { queue_record_release(at); });
        }
    };
    callbacks.on_command_press = [weak, this](double at) {
        if (auto guard = weak.lock()) {
            guard->run([this, at] { queue_command_press(at); });
        }
    };
    callbacks.on_command_release = [weak, this](double at) {
        if (auto guard = weak.lock()) {
            guard->run([this, at] { queue_command_release(at); });
        }
    };
    callbacks.on_cancel = on_ui([this] { control_.cancel(); });
    callbacks.on_minimize_tray = on_ui([this] { control_.minimize_to_tray(); });
    callbacks.on_meeting_toggle = guarded([this] { meeting_toggle_requested.emit(); });
    callbacks.on_scratchpad_toggle = guarded([this] { scratchpad_toggle_requested.emit(); });
    callbacks.on_cycle_language = guarded([this] { cycle_language_requested.emit(); });
    callbacks.on_paste_last_original = guarded([this] { paste_last_original_after_keys_up(); });
    callbacks.on_route_record = guarded([this] { route_record_requested.emit(); });
    callbacks.on_walkthrough = guarded([this] { walkthrough_requested.emit(); });
    callbacks.on_screenshot = guarded([this] { screenshot_requested.emit(); });
    callbacks.on_paste_walkthrough = guarded([this] { paste_walkthrough_after_keys_up(); });
    const auto status_update = [weak, this](const std::string& text) {
        if (auto guard = weak.lock()) {
            guard->run([this, &text] { update_status_with_auto_hide(text); });
        }
    };
    callbacks.on_status_update = status_update;
    callbacks.on_status_update_auto_hide = status_update;
    manager_.set_callbacks(std::move(callbacks));

    hotkeys_changed.emit(map);
    refresh_profile_hotkeys();
}

void HotkeyRuntime::refresh_profile_hotkeys() {
    const Json settings = *settings_.snapshot();
    text::ShortcutContext context;
    Json standard = Json::object();
    for (const auto& [action, hotkey] : manager_.hotkeys()) {
        standard[action] = hotkey;
    }
    context.standard_hotkeys = std::move(standard);
    std::vector<ActionShortcut> actions;
    if (action_loader_) {
        try {
            actions = action_loader_(settings);
        } catch (const std::exception& error) {
            kLog.error_with(error, "Could not load action shortcuts");
            actions.clear();
        } catch (...) {
            kLog.error("Could not load action shortcuts");
            actions.clear();
        }
    }
    // Every saved action is a binding the others must not collide with, enabled or not.
    for (const ActionShortcut& action : actions) {
        context.actions.push_back({action.id, action.name, action.hotkey});
    }
    using Conflict = std::string (*)(std::string_view, const Json&, std::string_view,
                                     const text::ShortcutContext&);
    const auto free = [&](const std::string& hotkey, Conflict conflict, const std::string& id) {
        return !hotkey.empty() && conflict(hotkey, settings, id, context).empty();
    };

    hotkeys::HotkeyList profiles;
    for (const auto& profile : text::load_cleanup_profiles(settings)) {
        if (free(profile.hotkey, &text::profile_hotkey_conflict, profile.id)) {
            profiles.emplace_back(profile.id, profile.hotkey);
        }
    }
    hotkeys::HotkeyList transforms;
    try {
        const auto loaded =
            transform_loader_ ? transform_loader_(settings) : text::load_transforms(settings);
        for (const auto& transform : loaded) {
            if (free(transform.hotkey, &text::transform_hotkey_conflict, transform.id)) {
                transforms.emplace_back(transform.id, transform.hotkey);
            }
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Could not load transform shortcuts");
        transforms.clear();
    }
    hotkeys::HotkeyList enabled_actions;
    for (const ActionShortcut& action : actions) {
        if (!action.enabled || action.hotkey.empty()) {
            continue;
        }
        // hotkey_conflict(exclude=("action", id)): the action is checked against every binding
        // but itself. fv_text names no action kind, so the action leaves the list instead; a
        // profile's exclusion of the id "" excludes nothing, as profile ids are never blank.
        text::ShortcutContext others = context;
        std::erase_if(others.actions,
                      [&](const hotkeys::Binding& binding) { return binding.id == action.id; });
        if (text::profile_hotkey_conflict(action.hotkey, settings, {}, others).empty()) {
            enabled_actions.emplace_back(action.id, action.hotkey);
        }
    }
    std::weak_ptr<CallbackGuard> weak = guard_;
    manager_.set_dynamic_hotkeys(hotkeys::kProfile, profiles, [weak, this](const std::string& id) {
        if (auto guard = weak.lock()) {
            guard->run([this, &id] { profile_record_requested.emit(id); });
        }
    });
    manager_.set_dynamic_hotkeys(hotkeys::kTransform, transforms,
                                 [weak, this](const std::string& id) {
                                     if (auto guard = weak.lock()) {
                                         guard->run([this, &id] { transform_requested.emit(id); });
                                     }
                                 });
    manager_.set_dynamic_hotkeys(
        hotkeys::kAction, enabled_actions, [weak, this](const std::string& id) {
            if (auto guard = weak.lock()) {
                guard->run([this, &id] { action_record_requested.emit(id); });
            }
        });
}

void HotkeyRuntime::set_recording_trigger_mode(std::string_view mode) {
    std::string applied(mode);
    if (applied != "toggle" && applied != "push_hold") {
        applied = "toggle"; // config.RECORDING_TRIGGER_MODE
    }
    kLog.info("Recording trigger mode set to {}", applied);
    dispatcher_->submit([this] {
        sync_recordings_ended();
        machine_.reset();
    });
    manager_.set_record_mode(hotkeys::trigger_mode_from(applied));
    status.emit(applied == "push_hold" ? std::string("Push-and-hold recording enabled")
                                       : std::string("Toggle recording enabled"));
}

void HotkeyRuntime::paste_last_original_after_keys_up() {
    if (!keys_.wait_for_modifiers_released()) {
        status.emit(std::string(KEYS_STILL_HELD));
        return;
    }
    std::weak_ptr<CallbackGuard> weak = guard_;
    ui_.post([weak, this] {
        if (auto guard = weak.lock()) {
            guard->run([this] { control_.paste_last_original(); });
        }
    });
}

void HotkeyRuntime::paste_walkthrough_after_keys_up() {
    // The app is read before the wait: the walkthrough is pasted where the shortcut was pressed.
    const std::optional<focus::AppIdentity> target = focus::get_service()->current_identity();
    if (!keys_.wait_for_modifiers_released()) {
        status.emit(std::string(WALKTHROUGH_KEYS_STILL_HELD));
        return;
    }
    paste_walkthrough_requested.emit(target);
}

void HotkeyRuntime::queue_record_press(double at) {
    dispatcher_->submit([this, at] { record_key_pressed(at); });
}

void HotkeyRuntime::queue_record_release(double at) {
    dispatcher_->submit([this, at] { record_key_released(at); });
}

void HotkeyRuntime::queue_command_press(double at) {
    dispatcher_->submit([this, at] {
        if (CommandKeys* keys = command_keys_.load()) {
            keys->key_pressed(at);
        }
    });
}

void HotkeyRuntime::queue_command_release(double at) {
    dispatcher_->submit([this, at] {
        if (CommandKeys* keys = command_keys_.load()) {
            keys->key_released(at);
        }
    });
}

void HotkeyRuntime::on_recording_state_changed(bool recording) {
    if (recording) {
        {
            std::lock_guard lock(stream_mutex_);
            stream_woken_ = true;
        }
        stream_changed_.notify_all();
        return;
    }
    recordings_ended_.fetch_add(1);
    dispatcher_->submit([this] {
        sync_recordings_ended();
        machine_.drop_stale_latch();
    });
}

void HotkeyRuntime::record_key_pressed(std::optional<double> at) {
    const double when = at.value_or(Seconds(clock_.steady().time_since_epoch()).count());
    sync_recordings_ended();
    machine_.press(when);
}

void HotkeyRuntime::record_key_released(std::optional<double> at) {
    const double when = at.value_or(Seconds(clock_.steady().time_since_epoch()).count());
    sync_recordings_ended();
    machine_.release(when);
}

bool HotkeyRuntime::sync_recordings_ended() {
    bool stale = false;
    const std::uint64_t ended = recordings_ended_.load();
    while (recordings_seen_ < ended) {
        ++recordings_seen_;
        stale = machine_.recording_ended() || stale;
    }
    return stale;
}

bool HotkeyRuntime::call_on_ui(const std::function<bool()>& fn, bool fallback) {
    if (ui_.is_current()) {
        return fn();
    }
    auto promise = std::make_shared<std::promise<bool>>();
    std::future<bool> answer = promise->get_future();
    ui_.post([promise, fn] {
        try {
            promise->set_value(fn());
        } catch (...) {
            promise->set_exception(std::current_exception());
        }
    });
    while (answer.wait_for(kAnswerPoll) != std::future_status::ready) {
        if (stopping_.load()) {
            return fallback;
        }
    }
    try {
        return answer.get();
    } catch (const std::future_error&) {
        return fallback; // the UI executor dropped the task (shutdown)
    }
}

bool HotkeyRuntime::wait_until_recording(double timeout_s) {
    // The start can return before the stream opens, so a release waits briefly before deciding
    // between stop and cancel. Real time: the wait is for the device, not the hook's clock.
    const auto deadline =
        std::chrono::steady_clock::now() +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(Seconds(timeout_s));
    while (!control_.stream_open() && !stopping_.load()) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            break;
        }
        std::unique_lock lock(stream_mutex_);
        stream_changed_.wait_until(lock, std::min(deadline, now + kStreamPoll),
                                   [this] { return stream_woken_; });
        stream_woken_ = false;
    }
    return control_.stream_open() && control_.is_recording();
}

void HotkeyRuntime::latch_status() {
    const HotkeyMap hotkeys = manager_.hotkeys();
    const auto found = hotkeys.find("record_toggle");
    const std::string hotkey = found != hotkeys.end() ? found->second : std::string();
    status.emit(
        std::format("Hands-free · press {} to stop",
                    hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary())));
}

void HotkeyRuntime::update_hotkeys(const hotkeys::HotkeyList& hotkeys) {
    kLog.info("Updating hotkeys: {}", dict_repr(hotkeys));
    const HotkeyMap map = to_map(hotkeys);
    manager_.update_hotkeys(map);
    Json saved = Json::object();
    for (const auto& [action, hotkey] : hotkeys) {
        saved[action] = hotkey;
    }
    save_hotkey_settings(settings_, saved);
    hotkeys_changed.emit(map);
    status.emit(std::string("Hotkeys updated"));
    refresh_profile_hotkeys();
}

void HotkeyRuntime::set_capture_suspended(bool suspended) {
    manager_.set_capture_suspended(suspended);
}

void HotkeyRuntime::setup_hook_watchdog() {
    if (on_wayland_()) {
        kLog.info("Wayland focused-window shortcuts do not need a hook watchdog");
        return;
    }
    watchdog_interval_ = HOTKEY_WATCHDOG_INTERVAL;
    sleep_gap_threshold_ = HOTKEY_SLEEP_GAP_THRESHOLD;
    expected_watchdog_time_ = now() + watchdog_interval_;
    last_rehook_time_.reset();
    arm_watchdog_timer();
    arm_refresh_timer();
    kLog.info("Hook watchdog started: sleep detection every {}ms, periodic refresh every {}ms",
              HOTKEY_WATCHDOG_INTERVAL.count(), HOTKEY_HOOK_REFRESH_INTERVAL.count());
}

void HotkeyRuntime::arm_watchdog_timer() {
    watchdog_timer_ = ui_.post_after(watchdog_interval_, [this] {
        arm_watchdog_timer();
        on_watchdog_tick();
    });
}

void HotkeyRuntime::arm_refresh_timer() {
    refresh_timer_ = ui_.post_after(HOTKEY_HOOK_REFRESH_INTERVAL, [this] {
        arm_refresh_timer();
        on_periodic_hook_refresh();
    });
}

void HotkeyRuntime::cleanup() {
    stopping_.store(true);
    dispatcher_->stop();
    watchdog_timer_.cancel();
    refresh_timer_.cancel();
}

void HotkeyRuntime::on_watchdog_tick() {
    const auto tick = now();
    const double gap = Seconds(tick - expected_watchdog_time_).count();
    expected_watchdog_time_ = tick + watchdog_interval_;
    if (gap > sleep_gap_threshold_.count()) {
        kLog.warning("Sleep/resume detected: time gap of {:.1f}s. Re-registering keyboard hook.",
                     gap);
        rehook_keyboard();
    } else if (refresh_pending_) {
        on_periodic_hook_refresh();
    }
}

void HotkeyRuntime::on_periodic_hook_refresh() {
    const auto tick = now();
    if (last_rehook_time_ && tick - *last_rehook_time_ < HOTKEY_MIN_REHOOK_GAP) {
        refresh_pending_ = false;
        return;
    }
    // A rehook forgets held keys, so refreshing in the middle of a push-and-hold would drop
    // its release and leave the recording running.
    if (manager_.keyboard_hold_active()) {
        if (!refresh_pending_) {
            kLog.info("Keyboard hook refresh waits for the held shortcut");
        }
        refresh_pending_ = true;
        return;
    }
    refresh_pending_ = false;
    kLog.info("Periodic keyboard hook refresh");
    rehook_keyboard();
}

void HotkeyRuntime::rehook_keyboard() {
    if (on_wayland_()) {
        return;
    }
    try {
        manager_.rehook();
        last_rehook_time_ = now();
        expected_watchdog_time_ = now() + watchdog_interval_;
    } catch (const std::exception& error) {
        kLog.error_with(error,
                        std::format("Failed to re-register keyboard hook: {}", error.what()));
    }
}

void HotkeyRuntime::on_stt_state_changed(bool enabled) {
    overlay_state_update.emit(enabled ? appcore::OverlayState::SttEnabled
                                      : appcore::OverlayState::SttDisabled);
}

void HotkeyRuntime::update_status_with_auto_hide(const std::string& text) {
    status.emit(text);
    if (text == "STT Enabled") {
        stt_state_changed.emit(true);
    } else if (text == "STT Disabled") {
        stt_state_changed.emit(false);
    }
}

} // namespace fv::dictation
