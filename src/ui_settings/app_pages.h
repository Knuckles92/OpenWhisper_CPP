#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>
#include <QWidget>

#include "base/json.h"
#include "ui_settings/json_metatype.h"

namespace fv {

class AppContext;
class Clock;
class CredentialStore;
class EnvReader;
class DotEnv;
class Executor;
class Recorder;
class SettingsDialog;
struct RecordingInfo;

namespace audio {
struct InputDevice;
}
namespace speech {
struct SpeechEndpoint;
}

class ApiKeysState;
class HotkeysPage;

// What the App pages and Recording tell the app (settings_dialog.py's on_* callbacks the
// controller sets), provided with ctx.provide<SettingsPagesSignals>(); the controller connects
// them. Each is emitted only after its setting was saved.
// Affinity: GUI thread.
class SettingsPagesSignals : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    // on_hotkeys_changed: the full standard shortcut map. While this signal has a receiver, the
    // receiver saves the `hotkeys` setting (and may throw, refusing it); without one the Hotkeys
    // page's host saves it itself (settings_manager.save_hotkey_settings).
    bool hasHotkeysReceiver() const;

Q_SIGNALS:
    // on_audio_device_changed: the saved microphone order (audio_input_priority's list).
    void audioDeviceChanged(const fv::Json& priority);
    // on_streaming_settings_changed: the live-preview switch.
    void streamingSettingsChanged();
    // on_streaming_font_changed: the live preview's text size (pt).
    void streamingFontChanged(int pointSize);
    // on_streaming_layout_changed: "header" or "footer".
    void streamingLayoutChanged(const QString& layout);
    // on_recording_look_changed: the overlay look or one of its label switches; the look is the
    // saved one.
    void recordingLookChanged(const QString& look);
    // on_ui_font_scale_changed: the percent.
    void uiFontScaleChanged(int percent);
    // on_ui_theme_changed: "dark", "light", "system" or "omarchy".
    void uiThemeChanged(const QString& theme);
    // on_api_keys_changed: a key was saved or removed.
    void apiKeysChanged();
    // on_developer_mode_changed.
    void developerModeChanged(bool enabled);
    // on_hotkeys_changed (see hasHotkeysReceiver).
    void hotkeysChanged(const fv::Json& hotkeys);
    // on_recording_trigger_mode_changed: "toggle" or "push_hold".
    void recordingTriggerModeChanged(const QString& mode);
    // on_profile_hotkey_capture: the Settings window's hotkeyCaptureSuspended, forwarded.
    void hotkeyCaptureSuspended(bool suspended);
    // The General page's update switches changed; emitted with the window's
    // settingsChanged("updates").
    void updatesChanged();
};

// The services the App pages and Recording reach, each a seam a test replaces. AppPages fills the
// empty ones from the context when a page is built.
struct AppPagesHooks {
    // QSystemTrayIcon::isSystemTrayAvailable.
    std::function<bool()> trayAvailable;
    // get_meeting_active: a meeting is running (Refresh devices is refused then).
    std::function<bool()> meetingActive;
    // audio_player.player().is_playing: playback is playing (Refresh is refused then).
    std::function<bool()> playbackPlaying;
    // AudioRecorder.get_input_devices: lists the inputs. Blocking; runs on `worker`.
    std::function<std::vector<audio::InputDevice>()> listDevices;
    // audio_devices.refresh_portaudio: false when refused because a stream is open; throws when
    // the audio system does not restart. Blocking; runs on `worker`.
    std::function<bool()> refreshDevices;
    // The throwaway recorder of the Appearance preview, on the saved microphone order, recording
    // to RecordingLookPreview::previewPath() (_recording_look_recorder).
    std::function<std::unique_ptr<Recorder>(const Json& settings)> previewRecorder;
    // The clock that preview animates on: ctx.clock() unless set; the system clock when the
    // context has none. Must outlive the pages.
    const Clock* clock = nullptr;

    // History's saved recordings (HistoryManager): (count, bytes); what limits would delete;
    // applying limits. Blocking; run on `worker`.
    std::function<std::pair<std::int64_t, std::int64_t>()> recordingsUsage;
    std::function<std::vector<RecordingInfo>(std::optional<std::int64_t>,
                                             std::optional<std::int64_t>)>
        recordingsOverLimit;
    std::function<void(std::optional<std::int64_t>, std::optional<std::int64_t>)> setRetention;

    // The credentials the API keys page reads and writes. All must outlive the pages.
    CredentialStore* credentials = nullptr;
    const EnvReader* env = nullptr;
    const DotEnv* dotenv = nullptr;
    // A key test against a speech server: (works, detail). speech::verify_endpoint_key over
    // ctx.http() unless set. Blocking; runs on `worker`.
    std::function<std::pair<bool, std::string>(const speech::SpeechEndpoint&, const std::string&)>
        verifyKey;

    // A Yes/No question ("Delete N saved recording(s)?", "Remove saved key"): true for Yes. A
    // QMessageBox, whose default is the safe answer, unless set.
    std::function<bool(QWidget* parent, const QString& title, const QString& text)> confirm;

    // The Settings window whose rail a background credential read redraws:
    // SettingsWindow::dialog() unless set.
    std::function<SettingsDialog*()> dialog;

    // Where the pages' blocking work runs (device lists, recordings usage, credential reads and
    // key tests): ctx.io() unless set.
    Executor* worker = nullptr;
};

// Registers Settings' App pages (General, Appearance, Hotkeys, API keys, Developer options) and
// Recording, through ctx.settingsPages(), with their rail values, search fields and control
// names; owns the hooks those pages use and the API keys rail's credential state.
// Affinity: GUI thread.
class AppPages {
public:
    explicit AppPages(AppContext& context);
    ~AppPages();
    AppPages(const AppPages&) = delete;
    AppPages& operator=(const AppPages&) = delete;

    // Adds the six pages to the context's SettingsPageRegistry. Throws std::invalid_argument
    // when one of their destinations is registered already.
    void registerPages();

    // Read when a page is built; set before Settings first opens.
    AppPagesHooks& hooks() { return hooks_; }
    SettingsPagesSignals& signals() const { return *signals_; }
    // The SettingsPagesSignals the pages emit on; also provided to the context.
    std::shared_ptr<SettingsPagesSignals> sharedSignals() const { return signals_; }

    // The hooks with the empty entries filled from the context.
    AppPagesHooks resolvedHooks();
    // The API keys rail's credential state; built on first use from the resolved hooks.
    std::shared_ptr<ApiKeysState> apiKeysState();

private:
    QString hotkeysRailValue(const Json& settings) const;
    QString apiKeysRailValue(const Json& settings);

    AppContext& context_;
    AppPagesHooks hooks_;
    std::shared_ptr<SettingsPagesSignals> signals_;
    std::shared_ptr<ApiKeysState> apiKeys_;
    std::unique_ptr<DotEnv> dotenv_;
    QPointer<HotkeysPage> hotkeysPage_;
    // The window whose openings re-read the API keys rail's credentials.
    QPointer<SettingsDialog> watchedDialog_;
};

} // namespace fv
