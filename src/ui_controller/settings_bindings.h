#pragma once

#include <functional>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"

namespace fv {

class ApplicationController;
class DownloadsPresenter;
class RecordWidgets;
class ScreenRegistry;
class SettingsDialog;
class SettingsPagesSignals;
class SettingsStore;
class SettingsWindow;
class UiController;
class VoiceModelPage;
class WaveformOverlay;

namespace dictation {
class HotkeyRuntime;
class StreamingRuntime;
} // namespace dictation

// What the Settings bindings reach. `signals` and `window` are null when the Settings feature
// is not linked, `streaming` and `downloads` when theirs could not be built; the rest are
// required. Each must outlive the bindings.
struct SettingsBindingsParts {
    SettingsPagesSignals* signals = nullptr;
    SettingsWindow* window = nullptr;
    ApplicationController* app = nullptr;
    UiController* ui = nullptr;
    dictation::HotkeyRuntime* hotkeys = nullptr;
    dictation::StreamingRuntime* streaming = nullptr;
    RecordWidgets* record = nullptr;
    WaveformOverlay* overlay = nullptr;
    ScreenRegistry* screens = nullptr;
    DownloadsPresenter* downloads = nullptr;
    // ui::applyUiTheme for a preference; true when the palette changed.
    std::function<bool(const QString& preference)> applyTheme;
    // ui::applyUiFontScale (percent).
    std::function<void(int percent)> applyFontScale;
};

// UIController._prepare_settings_dialog's callbacks (ui_qt/ui_controller.py:1605-1648): what the
// App pages, Recording, Appearance, API keys and Hotkeys pages say (SettingsPagesSignals) goes to
// the runtimes, the overlay, the theme and the shortcuts; the dialog's settingsChanged goes to
// the controller's on_settings_changed; the Voice model page's engine choice goes through the
// controller (select_transcription_backend) and its engine fields reload the engine.
// While connected, this is the receiver of hotkeysChanged, so it saves the shortcuts.
// Affinity: GUI thread.
class SettingsBindings : public QObject {
    Q_OBJECT
public:
    explicit SettingsBindings(SettingsBindingsParts parts, QObject* parent = nullptr);
    ~SettingsBindings() override;

    // _on_settings_streaming_changed: the tabs show the switch first, then the preview is set
    // up again.
    void onStreamingSettingsChanged();
    // _apply_ui_font_scale and _apply_ui_theme: the Markdown documents bake their colours and
    // sizes in when rendered, so they are rendered again (a theme change only when the palette
    // changed).
    void onUiFontScaleChanged(int percent);
    void onUiThemeChanged(const QString& preference);
    // _on_api_keys_changed: model views redrawn, and the API engine asks for its key again.
    void onApiKeysChanged();
    // _on_settings_hotkeys_changed: saved and applied by the hotkey runtime, whose
    // hotkeys_changed redraws the shortcut texts.
    void onHotkeysChanged(const Json& hotkeys);
    // _redraw_rendered_text: Quick Record, Upload File and its reading window.
    void redrawRenderedText();

    // The Voice model page wired to the controller, null until its page was built.
    VoiceModelPage* voiceModelPage() const;

private:
    void onDialogCreated(SettingsDialog* dialog);
    void wireVoiceModelPage(SettingsDialog* dialog);

    SettingsBindingsParts parts_;
    QPointer<SettingsDialog> dialog_;
    QPointer<VoiceModelPage> voicePage_;
};

} // namespace fv
