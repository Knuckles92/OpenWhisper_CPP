#include "ui_controller/settings_bindings.h"

#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

#include <QMetaObject>
#include <QStackedWidget>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/log.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/streaming_runtime.h"
#include "hotkeys/defaults.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/ui_controller.h"
#include "ui_downloads/downloads_presenter.h"
#include "ui_downloads/voice_model_page.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/transcript_viewer_dialog.h"
#include "ui_record/upload_file_tab.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

// The meeting screen's key (ui_shell's screen_key::meeting); its page arrives with M7.
constexpr char kMeetingScreen[] = "meeting";

template <class T> T& required(T* pointer, const char* name) {
    if (pointer == nullptr) {
        throw std::invalid_argument(std::string("SettingsBindings needs ") + name);
    }
    return *pointer;
}

} // namespace

SettingsBindings::SettingsBindings(SettingsBindingsParts parts, QObject* parent)
    : QObject(parent), parts_(std::move(parts)) {
    required(parts_.app, "the application controller");
    required(parts_.ui, "the UI controller");
    required(parts_.hotkeys, "the hotkey runtime");
    required(parts_.record, "the record widgets");
    required(parts_.overlay, "the overlay");
    required(parts_.screens, "the screen registry");

    if (SettingsPagesSignals* signals = parts_.signals) {
        ApplicationController* app = parts_.app;
        WaveformOverlay* overlay = parts_.overlay;
        dictation::HotkeyRuntime* hotkeys = parts_.hotkeys;
        connect(signals, &SettingsPagesSignals::audioDeviceChanged, this,
                [app](const Json&) { app->change_audio_device(); });
        connect(signals, &SettingsPagesSignals::streamingSettingsChanged, this,
                &SettingsBindings::onStreamingSettingsChanged);
        connect(signals, &SettingsPagesSignals::streamingFontChanged, overlay,
                [overlay](int) { overlay->refreshStreamingFontSize(); });
        connect(signals, &SettingsPagesSignals::streamingLayoutChanged, overlay,
                [overlay](const QString&) { overlay->refreshStreamingLayout(); });
        connect(signals, &SettingsPagesSignals::recordingLookChanged, overlay,
                [overlay](const QString&) { overlay->refreshRecordingLook(); });
        connect(signals, &SettingsPagesSignals::uiFontScaleChanged, this,
                &SettingsBindings::onUiFontScaleChanged);
        connect(signals, &SettingsPagesSignals::uiThemeChanged, this,
                &SettingsBindings::onUiThemeChanged);
        connect(signals, &SettingsPagesSignals::apiKeysChanged, this,
                &SettingsBindings::onApiKeysChanged);
        ScreenRegistry* screens = parts_.screens;
        connect(signals, &SettingsPagesSignals::developerModeChanged, this,
                [screens](bool enabled) {
                    // update_screen("meeting", "set_developer_mode", enabled).
                    if (!screens->isDefined(kMeetingScreen)) {
                        return;
                    }
                    screens->update(kMeetingScreen, "set_developer_mode", [enabled](QWidget* page) {
                        if (page->metaObject()->indexOfMethod("setDeveloperMode(bool)") >= 0) {
                            QMetaObject::invokeMethod(page, "setDeveloperMode", enabled);
                        }
                    });
                });
        connect(signals, &SettingsPagesSignals::hotkeysChanged, this,
                &SettingsBindings::onHotkeysChanged);
        connect(
            signals, &SettingsPagesSignals::recordingTriggerModeChanged, this,
            [app](const QString& mode) { app->update_recording_trigger_mode(qt::toUtf8(mode)); });
        // on_profile_hotkey_capture: the global shortcuts pause while a field captures one.
        connect(signals, &SettingsPagesSignals::hotkeyCaptureSuspended, this,
                [hotkeys](bool suspended) { hotkeys->set_capture_suspended(suspended); });
        // The General page's update switches: the update checks re-read them (M3), through
        // the dialog's settingsChanged("updates") below.
    }

    if (SettingsWindow* window = parts_.window) {
        connect(window, &SettingsWindow::dialogCreated, this, &SettingsBindings::onDialogCreated);
        if (SettingsDialog* dialog = window->dialog()) {
            onDialogCreated(dialog);
        }
    }
}

SettingsBindings::~SettingsBindings() = default;

VoiceModelPage* SettingsBindings::voiceModelPage() const {
    return voicePage_;
}

void SettingsBindings::onStreamingSettingsChanged() {
    parts_.record->refreshLivePreviewControls();
    if (parts_.streaming != nullptr) {
        parts_.streaming->reconfigure();
    }
}

void SettingsBindings::onUiFontScaleChanged(int percent) {
    if (parts_.applyFontScale) {
        parts_.applyFontScale(percent);
    }
    redrawRenderedText();
}

void SettingsBindings::onUiThemeChanged(const QString& preference) {
    if (parts_.applyTheme && parts_.applyTheme(preference)) {
        redrawRenderedText();
    }
}

void SettingsBindings::onApiKeysChanged() {
    if (parts_.downloads != nullptr) {
        parts_.downloads->refreshModelViews();
    }
    parts_.app->refresh_api_engine();
}

// The receiver saves (SettingsPagesSignals::hasHotkeysReceiver): update_hotkeys saves the map
// and re-registers it; a save that fails throws back to the page, which refuses the change.
void SettingsBindings::onHotkeysChanged(const Json& hotkeys) {
    if (!hotkeys.is_object()) {
        kLog.warning("The saved shortcuts are not a mapping; nothing was changed");
        return;
    }
    hotkeys::HotkeyList list;
    for (const auto& [action, hotkey] : hotkeys.items()) {
        list.emplace_back(action, hotkey.is_string() ? hotkey.get<std::string>() : std::string());
    }
    parts_.app->update_hotkeys(list);
}

void SettingsBindings::redrawRenderedText() {
    RecordWidgets& record = *parts_.record;
    if (QuickRecordTab* quick = record.existingQuickRecord()) {
        quick->redrawTranscript();
    }
    UploadFileTab* upload = record.existingUpload();
    if (upload == nullptr) {
        return;
    }
    upload->redrawTranscript();
    if (TranscriptViewerDialog* viewer = upload->viewer()) {
        viewer->refreshTypography();
    }
}

void SettingsBindings::onDialogCreated(SettingsDialog* dialog) {
    if (dialog == nullptr || dialog == dialog_) {
        return;
    }
    dialog_ = dialog;
    UiController* ui = parts_.ui;
    // on_settings_changed, read at call time (_on_settings_page_changed).
    connect(dialog, &SettingsDialog::settingsChanged, ui, &UiController::settingsPageChanged);
    // The Voice model page is built on first use; it is wired before anyone can use it.
    QPointer<SettingsDialog> guarded = dialog;
    const auto wire = [this, guarded] {
        if (guarded != nullptr) {
            wireVoiceModelPage(guarded);
        }
    };
    connect(dialog, &SettingsDialog::destinationChanged, this, wire);
    connect(dialog, &SettingsDialog::windowShown, this, wire);
    if (QStackedWidget* stack = dialog->stack()) {
        connect(stack, &QStackedWidget::widgetAdded, this, wire);
    }
    wire();
}

// models.on_backend_changed = select_transcription_backend; models.on_runtime_settings_changed =
// _on_manager_runtime_changed.
void SettingsBindings::wireVoiceModelPage(SettingsDialog* dialog) {
    const std::string key(destinations::voice_model);
    if (!dialog->isRegistered(key) || !dialog->isBuilt(key)) {
        return;
    }
    auto* page = qobject_cast<VoiceModelPage*>(dialog->pageContent(key));
    if (page == nullptr || page == voicePage_) {
        return;
    }
    voicePage_ = page;
    UiController* ui = parts_.ui;
    ApplicationController* app = parts_.app;
    page->setBackendHandler([ui](const QString& displayName) { ui->onModelChanged(displayName); });
    connect(page, &VoiceModelPage::engineSettingsChanged, this, [app] {
        kLog.info("Local engine settings changed via Settings");
        app->on_local_engine_settings_changed();
    });
    // A server or model of the API engine was saved: it is asked for its key again; a switch of
    // engine went through the handler, which reloads.
    connect(page, &VoiceModelPage::backendChanged, this,
            [app](const QString&) { app->refresh_api_engine(); });
    connect(page, &VoiceModelPage::helpRequested, ui, [this](const QString& destination) {
        if (parts_.window != nullptr) {
            parts_.window->openEngineHelp(destination);
        }
    });
}

} // namespace fv
