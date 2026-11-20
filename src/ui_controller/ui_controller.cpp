#include "ui_controller/ui_controller.h"
#include "ui_downloads/downloads_presenter.h"

#include <chrono>
#include <exception>
#include <format>
#include <future>
#include <stdexcept>
#include <utility>

#include <QMenu>
#include <QMetaObject>
#include <QPushButton>
#include <QStackedWidget>

#include "appcore/activity.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/executor.h"
#include "base/log.h"
#include "base/utf8.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/streaming_runtime.h"
#include "hotkeys/defaults.h"
#include "hotkeys/grammar.h"
#include "models/component_runtime.h"
#include "models/model_runtime.h"
#include "models/speech_catalog.h"
#include "recorder/recorder.h"
#include "settings/store.h"
#include "ui_controller/about_box.h"
#include "ui_controller/application_controller.h"
#include "ui_controller/clipboard_service.h"
#include "ui_controller/engine_setup.h"
#include "ui_controller/overlay_router.h"
#include "ui_history/history_actions.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_store.h"
#include "ui_overlay/language_menu.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_record/compact_record_controller.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/scratchpad.h"
#include "ui_record/stats_display.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_setup/guided_setup_offer.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/tabbed_content.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

// How long a History action on io waits for the GUI thread's answer.
constexpr std::chrono::seconds kUiAnswerTimeout{10};

template <class T> T& required(T* pointer, const char* name) {
    if (pointer == nullptr) {
        throw std::invalid_argument(std::string("UiController needs ") + name);
    }
    return *pointer;
}

// config.DEFAULT_HOTKEYS for an action, "" when it has none.
std::string defaultHotkey(std::string_view action) {
    for (const auto& [name, hotkey] : hotkeys::default_hotkeys()) {
        if (name == action) {
            return hotkey;
        }
    }
    return {};
}

QString displayOf(const std::map<std::string, std::string>& hotkeys, const std::string& action,
                  bool withDefault) {
    const auto it = hotkeys.find(action);
    std::string hotkey = it != hotkeys.end() ? it->second : std::string();
    if (it == hotkeys.end() && withDefault) {
        hotkey = defaultHotkey(action);
    }
    return qt::toQString(hotkeys::format_hotkey_display(hotkey, hotkeys::platform_vocabulary()));
}

// The engine picker's display name back to the selected_model value it saves.
std::string backendValue(const QString& displayName) {
    for (const auto& choice : models::backend_choices()) {
        if (qt::toQString(choice.label) == displayName) {
            return std::string(choice.value);
        }
    }
    return qt::toUtf8(displayName);
}

} // namespace

// The History actions' `ui` (history_actions.py), called on io: what it shows goes to the GUI
// thread, and what it asks waits there for the answer.
struct UiController::HistoryBridge final : HistoryActionsUi {
    QPointer<UiController> owner;
    Executor* ui = nullptr;

    template <class R, class Fn> R ask(Fn fn, R fallback) {
        auto answer = std::make_shared<std::promise<R>>();
        std::future<R> result = answer->get_future();
        QPointer<UiController> target = owner;
        ui->post([target, answer, fn = std::move(fn), fallback] {
            try {
                answer->set_value(target != nullptr ? fn(*target) : fallback);
            } catch (...) {
                answer->set_value(fallback);
            }
        });
        if (result.wait_for(kUiAnswerTimeout) != std::future_status::ready) {
            return fallback;
        }
        return result.get();
    }

    void setStatus(const QString& status) override {
        QPointer<UiController> target = owner;
        ui->post([target, status] {
            if (target != nullptr) {
                target->setStatus(status);
            }
        });
    }
    void showNotice(const QString& text, bool done) override {
        QPointer<UiController> target = owner;
        ui->post([target, text, done] {
            if (target != nullptr) {
                target->showNotice(text, done);
            }
        });
    }
    bool pasteTextNow(const QString& text) override {
        return ask<bool>(
            [text](UiController& controller) {
                return controller.parts_.transcription->paste_text_now(qt::toUtf8(text));
            },
            false);
    }
    void refreshHistory() override {
        QPointer<UiController> target = owner;
        ui->post([target] {
            if (target != nullptr) {
                target->refreshHistory();
            }
        });
    }
    bool copyToClipboard(const QString& text) override {
        return ask<bool>(
            [text](UiController& controller) {
                return controller.parts_.clipboard->copyToClipboard(text);
            },
            false);
    }
};

// The language menu's `ui` (language_menu.py): the overlay, the status line, Settings and the
// settings store.
struct UiController::LanguageUi final : LanguageMenuUi {
    explicit LanguageUi(UiController& owner) : owner(owner) {}

    WaveformOverlay& overlay() override { return *owner.parts_.overlay; }
    void setStatus(const QString& text) override { owner.setStatus(text); }
    void openVoiceModelSettings() override {
        if (owner.parts_.settingsWindow != nullptr) {
            owner.parts_.settingsWindow->open(qt::toQString(destinations::voice_model));
        }
    }
    SettingsStore& settings() override {
        if (owner.parts_.settings == nullptr) {
            throw std::runtime_error("The settings are not available");
        }
        return *owner.parts_.settings;
    }

    UiController& owner;
};

UiController::UiController(UiControllerParts parts, QObject* parent)
    : QObject(parent), parts_(std::move(parts)),
      router_(new OverlayRouter(required(parts_.overlay, "the overlay"), this)) {
    required(parts_.window, "the main window");
    required(parts_.screens, "the screen registry");
    required(parts_.record, "the record widgets");
    required(parts_.app, "the application controller");
    required(parts_.transcription, "the transcription runtime");
    required(parts_.hotkeys, "the hotkey runtime");
    required(parts_.clipboard, "the clipboard");
    required(parts_.activity, "the activity state");
    required(parts_.ui, "the UI executor");
    required(parts_.io, "the io executor");
    historyBridge_ = std::make_shared<HistoryBridge>();
    historyBridge_->owner = this;
    historyBridge_->ui = parts_.ui;
    languageUi_ = std::make_unique<LanguageUi>(*this);

    router_->activityState = [this](appcore::OverlayState state) {
        applyToQuickRecord("set_activity_state",
                           [state](QuickRecordTab* tab) { tab->setActivityState(state); });
    };
    router_->setUploadPresenter(parts_.upload);
    connect(parts_.clipboard, &ClipboardService::statusRequested, this, &UiController::setStatus);
    if (parts_.upload != nullptr) {
        connect(parts_.upload, &UploadPresenter::copiedAnimationRequested, this,
                &UiController::showCopiedAnimation);
    }
    onSettingsChanged = [app = parts_.app](std::string_view kind) {
        app->on_settings_changed(kind);
    };

    wireWindow();
    wireCompact();
    wireRuntimes();
    wireM2Runtimes();
    wireHistory();

    // The tabs are built when they first show, which may be after this.
    if (QWidget* existing = parts_.screens->existing(RecordFeature::kQuickRecordScreen)) {
        wireQuickRecord(qobject_cast<QuickRecordTab*>(existing));
    }
    if (parts_.screens->isDefined(RecordFeature::kUploadScreen)) {
        if (QWidget* existing = parts_.screens->existing(RecordFeature::kUploadScreen)) {
            wireUpload(qobject_cast<UploadFileTab*>(existing));
        }
    }
    connect(parts_.window, &MainWindow::screenCreated, this,
            [this](const QString& key, QWidget* screen) {
                if (key == QLatin1String(RecordFeature::kQuickRecordScreen)) {
                    wireQuickRecord(qobject_cast<QuickRecordTab*>(screen));
                } else if (key == QLatin1String(RecordFeature::kUploadScreen)) {
                    wireUpload(qobject_cast<UploadFileTab*>(screen));
                }
            });
}

UiController::~UiController() {
    alive_.reset();
    links_.clear();
    // The window outlives the features; its Quit must not ask a controller that is gone.
    parts_.window->confirmQuit = [] {
        return true;
    };
    if (historyBridge_) {
        historyBridge_->owner = nullptr;
    }
}

template <class Fn> void UiController::onUi(Fn&& fn) {
    QPointer<UiController> self = this;
    parts_.ui->post([self, fn = std::forward<Fn>(fn)]() mutable {
        if (self != nullptr) {
            fn(*self);
        }
    });
}

QuickRecordTab* UiController::quickRecord() const {
    return qobject_cast<QuickRecordTab*>(
        parts_.screens->existing(RecordFeature::kQuickRecordScreen));
}

CompactRecordController* UiController::compactController() const {
    return parts_.record->compactController();
}

// The tab now when it exists, or when it is built (MainWindow.update_screen).
void UiController::applyToQuickRecord(std::string method,
                                      std::function<void(QuickRecordTab*)> apply) {
    parts_.screens->update(RecordFeature::kQuickRecordScreen, std::move(method),
                           [apply = std::move(apply)](QWidget* screen) {
                               if (auto* tab = qobject_cast<QuickRecordTab*>(screen)) {
                                   apply(tab);
                               }
                           });
}

// ---- wiring --------------------------------------------------------------------------------

// UIController._setup_connections: the window's signals M1 answers.
void UiController::wireWindow() {
    MainWindow& window = *parts_.window;
    connect(&window, &MainWindow::recordToggled, this, &UiController::onRecordToggled);
    connect(&window, &MainWindow::recordCanceled, this, &UiController::cancelRecording);
    connect(&window, &MainWindow::modelChanged, this, &UiController::onModelChanged);
    connect(&window, &MainWindow::whisperEngineChanged, this, [this] {
        kLog.info("Local engine settings changed via main GUI");
        parts_.app->on_local_engine_settings_changed();
    });
    connect(&window, &MainWindow::retranscribeRequested, this, [this](const QString& path) {
        kLog.info("Re-transcribe requested: {}", qt::toUtf8(path));
        parts_.app->retranscribe_audio(base_utf8::path_from_utf8(qt::toUtf8(path)));
    });
    connect(&window, &MainWindow::quickRecordCopyRequested, this, &UiController::onQuickRecordCopy);
    connect(&window, &MainWindow::aboutRequested, this, &UiController::showAbout);
    // View > Scratchpad; the service ignores a second connection of the same window.
    if (parts_.scratchpad != nullptr) {
        parts_.scratchpad->connectWindow(window);
    }
    // The overlay's language chip.
    connect(parts_.overlay, &WaveformOverlay::languageCycleRequested, this,
            &UiController::cycleDictationLanguage);
    window.confirmQuit = [this] {
        return confirmQuitWhileBusy(parts_.window, parts_.activity->snapshot(), quitPrompt);
    };
}

// MainWindow's own wiring of Quick Record (main_window.py:741-761) and the controller's.
void UiController::wireQuickRecord(QuickRecordTab* tab) {
    if (tab == nullptr || tab == wiredQuickRecord_) {
        return;
    }
    wiredQuickRecord_ = tab;
    MainWindow& window = *parts_.window;
    connect(tab, &QuickRecordTab::recordToggled, this, &UiController::onRecordToggled);
    connect(tab, &QuickRecordTab::recordCanceled, this, [this] {
        // _on_quick_record_canceled: the compact controller is told at once.
        if (CompactRecordController* compact = compactController()) {
            compact->setRecordingState(false);
            compact->setStatus(QString::fromUtf8(MainWindow::kReadyStatus));
        }
        cancelRecording();
    });
    connect(tab, &QuickRecordTab::copyRequested, this, &UiController::onQuickRecordCopy);
    // open_settings_destination (ui_controller.py:423).
    connect(tab, &QuickRecordTab::settingsRequested, &window,
            &MainWindow::settingsDestinationRequested);
    // The profile card's Manage (ui_controller.py:422).
    connect(tab, &QuickRecordTab::profilesRequested, this, &UiController::openCleanupProfiles);
    wireTranscriptionTab(tab);
    // _on_stats_visibility_changed: Quick Record has no relay of its stats card's signal.
    if (TranscriptionStatsWidget* stats = tab->statsWidget()) {
        connect(
            stats, &TranscriptionStatsWidget::visibilityChanged, &window,
            [&window, stats](bool visible) { window.onStatsVisibilityChanged(visible, stats); });
    }
}

// MainWindow._connect_transcription_tab (main_window.py:666-681) for both tabs.
void UiController::wireTranscriptionTab(TranscriptionTabBase* tab) {
    MainWindow& window = *parts_.window;
    connect(tab, &TranscriptionTabBase::modelChanged, this, &UiController::onModelChanged);
    connect(tab, &TranscriptionTabBase::engineSettingsChanged, this, [this] {
        kLog.info("Local engine settings changed via main GUI");
        parts_.app->on_local_engine_settings_changed();
    });
    // Straight to the window: the sender is the tab the others follow.
    connect(tab, &TranscriptionTabBase::transcriptionCollapsed, &window,
            &MainWindow::onTranscriptionCollapsed);
    connect(tab, &TranscriptionTabBase::helpRequested, &window, &MainWindow::engineHelpRequested);
    connect(tab, &TranscriptionTabBase::engineDownloadsRequested, &window, [&window] {
        Q_EMIT window.settingsDestinationRequested(QStringLiteral("engine_downloads"));
    });
    connect(tab, &TranscriptionTabBase::livePreviewChanged, this,
            &UiController::onLivePreviewChanged);
    // A footer's AI cleanup switch (main_window.py:675); the window's signal is what the
    // cleanup agent offer (M8) listens to.
    connect(tab, &TranscriptionTabBase::cleanupToggled, &window, &MainWindow::cleanupToggled);
    // The Remote engine's card (main_window.py:671-673, ui_controller.py:427-430).
    connect(tab, &TranscriptionTabBase::remoteModelSelected, this,
            [this](const QString& family, const QString& model) {
                kLog.info("Remote model chosen: {}/{}", family.toStdString(), model.toStdString());
                if (onRemoteModelSelected) {
                    onRemoteModelSelected(family.toStdString(), model.toStdString());
                }
            });
    connect(tab, &TranscriptionTabBase::remoteRuntimeSelected, this,
            [this](const QString& family, const QString& model, const QVariantMap& changes) {
                if (onRemoteRuntimeSelected) {
                    Json patch = Json::object();
                    for (auto it = changes.begin(); it != changes.end(); ++it) {
                        patch[it.key().toStdString()] = it.value().toString().toStdString();
                    }
                    onRemoteRuntimeSelected(family.toStdString(), model.toStdString(), patch);
                }
            });
    connect(tab, &TranscriptionTabBase::remoteRetryRequested, this, [this] {
        if (onRemoteRetry) {
            onRemoteRetry();
        }
    });
}

// The Upload File tab's engine card and result card; its upload requests are UploadPresenter's.
void UiController::wireUpload(UploadFileTab* tab) {
    if (tab == nullptr || tab == wiredUpload_) {
        return;
    }
    wiredUpload_ = tab;
    if (parts_.upload != nullptr) {
        parts_.upload->attach(tab);
    }
    wireTranscriptionTab(tab);
    MainWindow* window = parts_.window;
    QPointer<UploadFileTab> upload = tab;
    connect(tab, &UploadFileTab::statsVisibilityChanged, window, [window, upload](bool visible) {
        if (upload != nullptr) {
            window->onStatsVisibilityChanged(visible, upload->statsWidget());
        }
    });
}

// main_window.py:606-614: the compact buttons click Quick Record's. OpenWhisper's tab always
// exists; here it is built on first show, and Compact Mode can be restored over another saved
// tab, so the first compact click builds it (as opening its tab would).
void UiController::wireCompact() {
    CompactRecordController* compact = compactController();
    const auto click = [this](auto button) {
        return [this, button] {
            QuickRecordTab* tab = quickRecord();
            if (tab == nullptr) {
                try {
                    tab = qobject_cast<QuickRecordTab*>(
                        parts_.screens->ensure(RecordFeature::kQuickRecordScreen,
                                               parts_.window->tabbedContent()->stack()));
                } catch (const std::exception& error) {
                    kLog.error_with(error, "Could not open Quick Record");
                }
            }
            if (tab != nullptr) {
                (tab->*button)()->click();
            }
        };
    };
    connect(compact, &CompactRecordController::recordRequested, this,
            click(&QuickRecordTab::recordButton));
    connect(compact, &CompactRecordController::stopRequested, this,
            click(&QuickRecordTab::stopButton));
    connect(compact, &CompactRecordController::cancelRequested, this,
            click(&QuickRecordTab::cancelButton));
}

// ApplicationController._connect_signals and the runtimes' signals the UI shows.
void UiController::wireRuntimes() {
    Executor& ui = *parts_.ui;
    dictation::TranscriptionRuntime& runtime = *parts_.transcription;
    ApplicationController& app = *parts_.app;
    dictation::HotkeyRuntime& hotkeys = *parts_.hotkeys;
    const auto status = [this](std::string text) {
        setStatus(qt::toQString(text));
    };
    const auto overlay = [this](appcore::OverlayState state) {
        router_->setOverlayState(state);
    };

    links_.push_back(runtime.status_update.connect(ui, status));
    links_.push_back(runtime.overlay_state_update.connect(ui, overlay));
    // RemoteRuntime.connect_signals: the Remote engine's link, Model field and runtime fields on
    // the record tabs, and on a host the computers using its engine.
    getRemoteModels = [&app] {
        return app.remote_models();
    };
    onRemoteModelSelected = [&app](const std::string& family, const std::string& model) {
        app.select_remote_model(family, model);
    };
    onRemoteRuntimeSelected = [&app](const std::string& family, const std::string& model,
                                     const Json& changes) {
        app.select_remote_runtime(family, model, changes);
    };
    onRemoteRetry = [&app] {
        app.retry_remote();
    };
    links_.push_back(
        app.remote_link_changed.connect(ui, [this, &app] { setRemoteLink(app.remote_link()); }));
    links_.push_back(app.remote_models_stale.connect(ui, [this] { refreshRemoteModels(); }));
    links_.push_back(app.remote_clients_changed.connect(
        ui, [this](std::vector<remote::ConnectedClient> clients) {
            std::vector<ServedClient> served;
            for (const auto& client : clients) {
                served.push_back(ServedClient{qt::toQString(client.name), client.busy});
            }
            setRemoteClients(served);
        }));
    // transcription_retry_progress -> show_transcription_retry: the paired host's reconnects.
    links_.push_back(runtime.transcription_retry_progress.connect(
        ui, [this](std::string label) { router_->showTranscriptionRetry(qt::toQString(label)); }));
    links_.push_back(runtime.transcription_retry_failed.connect(ui, [this](std::string label) {
        router_->showTranscriptionRetryFailure(qt::toQString(label));
    }));
    links_.push_back(runtime.recording_destination_changed.connect(
        ui, [this](std::string text) { router_->setRecordingDestination(qt::toQString(text)); }));
    links_.push_back(runtime.transcription_completed.connect(
        ui, [this](std::string text, std::optional<std::string> raw,
                   std::optional<dictation::CleanupInfo>) {
            displayTranscript(qt::toQString(text),
                              raw ? std::optional<QString>(qt::toQString(*raw)) : std::nullopt);
        }));
    links_.push_back(runtime.transcription_failed.connect(
        ui, [this](std::string message) { displayTranscriptionError(qt::toQString(message)); }));
    links_.push_back(runtime.transcription_stats.connect(
        ui, [this](dictation::TranscriptionStats stats) { setTranscriptionStats(stats); }));
    links_.push_back(runtime.clear_stats.connect(ui, [this] { clearTranscriptionStats(); }));
    links_.push_back(runtime.notice_requested.connect(
        ui, [this](std::string text, bool done) { showNotice(qt::toQString(text), done); }));
    links_.push_back(runtime.refresh_history.connect(ui, [this] { refreshHistory(); }));
    links_.push_back(runtime.device_info_changed.connect(
        ui, [this](std::string info, std::optional<bool> ready) {
            setDeviceInfo(qt::toQString(info), ready);
        }));
    links_.push_back(runtime.recording_state_changed().connect(
        ui, [this](bool recording) { onRecordingStateChanged(recording); }));

    links_.push_back(app.status_update.connect(ui, status));
    links_.push_back(
        app.device_info_update.connect(ui, [this](std::string info, std::optional<bool> ready) {
            setDeviceInfo(qt::toQString(info), ready);
        }));
    links_.push_back(
        app.engine_busy_changed.connect(ui, [this](bool busy) { setEngineBusy(busy); }));
    links_.push_back(app.minimize_to_tray_requested.connect(
        ui, [this] { parts_.window->toggleTrayVisibility(); }));
    links_.push_back(app.paste_last_original_requested.connect(
        *parts_.io, [bridge = historyBridge_, store = parts_.historyStore] {
            if (store != nullptr) {
                paste_last_original(*bridge, *store);
            }
        }));
    links_.push_back(
        app.recording_device_switched.connect(ui, [this](std::string oldName, std::string newName) {
            onRecordingDeviceSwitched(qt::toQString(oldName), qt::toQString(newName));
        }));
    links_.push_back(app.nothing_to_cancel.connect(ui, [this](std::string text) {
        setStatus(qt::toQString(text));
        showNotice(qt::toQString(text), false);
    }));
    // ensure_local_model_available: the download consent flow of Downloads; without the model
    // runtime (no network client), the runtime setup that adopts or picks a model.
    links_.push_back(app.model_setup_requested.connect(ui, [this] {
        if (parts_.models != nullptr) {
            parts_.models->ensure_selected_model_available();
            return;
        }
        if (parts_.engineSetup != nullptr && parts_.engineSetup->started() &&
            parts_.engineSetup->ensureEngine()) {
            parts_.app->reload_whisper_model();
        }
    }));
    // A load left no engine for want of its runtime: _prompt_for_model_runtime.
    links_.push_back(app.runtime_consent_requested.connect(ui, [this](std::string model) {
        if (parts_.models != nullptr) {
            parts_.models->prompt_for_runtime(model);
        } else {
            kLog.info("The speech runtime for {} is not installed", model);
        }
    }));

    links_.push_back(hotkeys.status.connect(ui, status));
    links_.push_back(hotkeys.overlay_state_update.connect(ui, overlay));
    links_.push_back(hotkeys.hands_free_changed.connect(ui, [this](bool on) { setHandsFree(on); }));
    links_.push_back(hotkeys.hotkeys_changed.connect(
        ui, [this](dictation::HotkeyRuntime::HotkeyMap map) { updateHotkeyDisplay(map); }));

    if (Recorder* recorder = parts_.recorder) {
        links_.push_back(
            recorder->level.connect(ui, [this](float level) { router_->setLevel(level); }));
    }
}

// What M2 adds to _connect_signals: uploads and batches, the live preview, the model and
// component runtimes' status lines, language cycling and the guided setup's try box.
void UiController::wireM2Runtimes() {
    Executor& ui = *parts_.ui;
    dictation::TranscriptionRuntime& runtime = *parts_.transcription;
    const auto status = [this](std::string text) {
        setStatus(qt::toQString(text));
    };

    links_.push_back(runtime.clear_partial_transcription.connect(ui, [this] {
        applyToQuickRecord("clear_partial_transcription",
                           [](QuickRecordTab* tab) { tab->clearPartialTranscription(); });
    }));
    links_.push_back(runtime.large_file_detected.connect(
        ui, [this](double megabytes) { router_->showLargeFileState(megabytes); }));
    if (UploadPresenter* upload = parts_.upload) {
        links_.push_back(
            runtime.batch_progress.connect(ui, [upload](int position, int total, std::string name) {
                upload->setBatchProgress(position, total, qt::toQString(name));
            }));
        links_.push_back(runtime.batch_item_finished.connect(
            ui, [upload](int position, bool success, std::string text) {
                upload->setBatchItemFinished(position, success, qt::toQString(text));
            }));
        links_.push_back(runtime.batch_completed.connect(
            ui, [upload](dictation::BatchResult result) { upload->setBatchResult(result); }));
    }

    if (dictation::StreamingRuntime* streaming = parts_.streaming) {
        // streaming.py connect_signals. The overlay's show is delivered inline when the preview
        // starts on the UI thread, ahead of the recording's Listening state.
        links_.push_back(streaming->partial_transcription.connect(ui, [this](std::string text,
                                                                             bool final) {
            const QString shown = qt::toQString(text);
            applyToQuickRecord("set_partial_transcription", [shown, final](QuickRecordTab* tab) {
                tab->setPartialTranscription(shown, final);
            });
        }));
        links_.push_back(
            streaming->streaming_text_update.connect(ui, [this](std::string text, bool final) {
                router_->updateStreamingText(qt::toQString(text), final);
            }));
        links_.push_back(streaming->streaming_overlay_show.connect(
            ui, [this] { router_->showStreamingOverlay(); }));
        links_.push_back(streaming->streaming_overlay_hide.connect(
            ui, [this] { router_->hideStreamingOverlay(); }));
        links_.push_back(streaming->status.connect(ui, status));
    }

    if (models::ModelRuntime* models = parts_.models) {
        links_.push_back(models->status.connect(ui, status));
    }
    if (models::ComponentRuntime* components = parts_.components) {
        links_.push_back(components->status.connect(ui, status));
    }

    links_.push_back(
        parts_.hotkeys->cycle_language_requested.connect(ui, [this] { cycleDictationLanguage(); }));

    if (GuidedSetup* setup = parts_.guidedSetup) {
        // The try box on Ready hears the next dictation (transcription_received and
        // transcription_error_received).
        QPointer<GuidedSetup> guided = setup;
        links_.push_back(runtime.transcription_completed.connect(
            ui, [guided](std::string text, std::optional<std::string>,
                         std::optional<dictation::CleanupInfo>) {
                if (guided != nullptr) {
                    guided->dictationReceived(qt::toQString(text), false);
                }
            }));
        links_.push_back(runtime.transcription_failed.connect(ui, [guided](std::string message) {
            if (guided != nullptr) {
                guided->dictationFailed(qt::toQString(message));
            }
        }));
    }
}

// The History panel's requests (main_window.py's history wiring that reaches the controller).
void UiController::wireHistory() {
    HistoryPanel* panel = parts_.history;
    if (panel == nullptr) {
        return;
    }
    connect(panel, &HistoryPanel::retranscribeRequested, this, [this](const QString& path) {
        kLog.info("Re-transcribe requested: {}", qt::toUtf8(path));
        parts_.app->retranscribe_audio(base_utf8::path_from_utf8(qt::toUtf8(path)));
    });
    connect(panel, &HistoryPanel::copiedFromDialog, this, &UiController::showCopiedAnimation);
}

void UiController::attachTray(SystemTray& tray) {
    tray_ = &tray;
    router_->setTray(&tray);
    connect(&tray, &SystemTray::toggleRecording, this,
            [this] { onTrayToggleRecording(QStringLiteral("ui")); });
    connect(&tray, &SystemTray::copyLastOriginalRequested, this, &UiController::copyLastOriginal);
    connect(&tray, &SystemTray::languageMenuRequested, this, &UiController::populateLanguageMenu);
    if (parts_.scratchpad != nullptr) {
        parts_.scratchpad->connectTray(tray);
    }
    tray.setRecording(isRecording_);
}

// ---- requests ------------------------------------------------------------------------------

// ui_controller.py:720-737.
bool UiController::startRecording(const QString& source) {
    return startRecording(source, QString());
}

// start_recording (720-737).
bool UiController::startRecording(const QString& source, const QString& profileId) {
    const bool started = !profileId.isEmpty() && onProfileRecordStart
                             ? onProfileRecordStart(qt::toUtf8(profileId), qt::toUtf8(source))
                             : parts_.app->start_recording(qt::toUtf8(source));
    if (!started) {
        revertRefusedRecordStart();
        return false;
    }
    return true;
}

// _revert_refused_record_start (739-744).
void UiController::revertRefusedRecordStart() {
    kLog.info("Recording start refused; reverting recording state");
    isRecording_ = false;
    parts_.window->setRecordingActive(false);
}

void UiController::stopRecording() {
    parts_.app->stop_recording();
}

// cancel_recording (757-770).
void UiController::cancelRecording() {
    isRecording_ = false;
    kLog.info("Recording canceled");
    if (parts_.window->isRecording()) {
        parts_.window->setRecordingActive(false);
        kLog.info("Main window recording state updated");
    }
    parts_.app->cancel();
    kLog.info("Record cancel callback called");
    applyToQuickRecord("clear_transcription",
                       [](QuickRecordTab* tab) { tab->clearTranscription(); });
}

// _on_record_toggled (618-622). The profile Quick Record selected arrives with profiles (M4).
// _on_record_toggled (618-622): Quick Record's chosen profile formats the recording.
void UiController::onRecordToggled(bool recording) {
    if (recording) {
        const QuickRecordTab* tab = quickRecord();
        startRecording(QStringLiteral("ui"),
                       tab != nullptr ? tab->selectedCleanupProfileId() : QString());
    } else {
        stopRecording();
    }
}

// _on_tray_toggle_recording (661-666).
void UiController::onTrayToggleRecording(const QString& source) {
    if (isRecording_) {
        stopRecording();
    } else {
        startRecording(source);
    }
}

// _on_model_changed (624-627): the tab gives the picker's label.
void UiController::onModelChanged(const QString& displayName) {
    kLog.info("Model changed to: {}", qt::toUtf8(displayName));
    parts_.app->on_model_changed(backendValue(displayName));
}

// _on_quick_record_copy (2497-2499).
void UiController::onQuickRecordCopy(const QString& text) {
    const bool succeeded = parts_.clipboard->copyToClipboard(text);
    if (QuickRecordTab* tab = quickRecord()) {
        tab->showCopyResult(succeeded);
    }
}

// The tray's Copy original: it blocks on the database, so it runs on io.
void UiController::copyLastOriginal() {
    std::shared_ptr<HistoryStore> store = parts_.historyStore;
    if (store == nullptr) {
        setStatus(QString::fromUtf8(history_actions::kReadFailed));
        return;
    }
    parts_.io->post([bridge = historyBridge_, store] { copy_last_original(*bridge, *store); });
}

void UiController::pasteLastOriginal() {
    parts_.app->paste_last_original();
}

void UiController::showAbout() {
    if (aboutBox) {
        aboutBox();
        return;
    }
    showAboutBox(parts_.window);
}

// ---- results -------------------------------------------------------------------------------

// _display_transcript (686-694): an upload's result stays in its own tab.
void UiController::displayTranscript(const QString& text, const std::optional<QString>& raw) {
    if (parts_.upload == nullptr || !parts_.upload->displayTranscript(text, raw)) {
        applyToQuickRecord("set_transcript", [text, raw](QuickRecordTab* tab) {
            tab->setTranscript(text, raw);
            tab->expandTranscription();
        });
    }
    router_->hideOverlay();
}

// _display_transcription_error (696-703): the status line says why; the transcript box stays
// empty, because Copy would copy an error there as if it were a transcript.
void UiController::displayTranscriptionError(const QString& message) {
    if (parts_.upload == nullptr || !parts_.upload->displayTranscriptionError(message)) {
        applyToQuickRecord("clear_transcription",
                           [](QuickRecordTab* tab) { tab->clearTranscription(); });
    }
    router_->hideOverlay();
}

// _apply_status_to_main_window (706-713): an upload job's statuses go to its progress panel.
void UiController::setStatus(const QString& status) {
    if (parts_.upload != nullptr) {
        parts_.upload->applyStatus(status);
        return;
    }
    parts_.window->setStatus(status);
}

// set_ambient_status (871-873).
void UiController::setAmbientStatus(const QString& status) {
    applyToQuickRecord("set_ambient_message",
                       [status](QuickRecordTab* tab) { tab->setAmbientMessage(status); });
}

// set_device_info (779-783) through MainWindow.update_transcription_tabs.
void UiController::setDeviceInfo(const QString& deviceInfo, std::optional<bool> ready) {
    parts_.screens->updateGroup(kTranscriptionGroup, "set_device_info",
                                [deviceInfo, ready](QWidget* screen) {
                                    if (auto* tab = qobject_cast<TranscriptionTabBase*>(screen)) {
                                        tab->setDeviceInfo(deviceInfo, ready);
                                    }
                                });
    // Every engine switch and reload reports here, so the Remote backend's Model field follows
    // what the host said it runs when it connected.
    refreshRemoteModels();
}

void UiController::refreshRemoteModels() {
    if (!getRemoteModels) {
        return;
    }
    const auto models = getRemoteModels();
    parts_.screens->updateGroup(kTranscriptionGroup, "set_remote_models",
                                [models](QWidget* screen) {
                                    if (auto* tab = qobject_cast<TranscriptionTabBase*>(screen)) {
                                        tab->setRemoteModels(models);
                                    }
                                });
}

void UiController::setRemoteLink(const std::optional<remote::RemoteLink>& link) {
    parts_.screens->updateGroup(kTranscriptionGroup, "set_remote_link", [link](QWidget* screen) {
        if (auto* tab = qobject_cast<TranscriptionTabBase*>(screen)) {
            tab->setRemoteLink(link);
        }
    });
}

void UiController::setRemoteClients(const std::vector<ServedClient>& clients) {
    parts_.screens->updateGroup(kTranscriptionGroup, "set_remote_clients",
                                [clients](QWidget* screen) {
                                    if (auto* tab = qobject_cast<TranscriptionTabBase*>(screen)) {
                                        tab->setRemoteClients(clients);
                                    }
                                });
}

// set_engine_busy (827-841).
void UiController::setEngineBusy(bool busy) {
    parts_.screens->updateGroup(kTranscriptionGroup, "set_engine_busy", [busy](QWidget* screen) {
        if (auto* tab = qobject_cast<TranscriptionTabBase*>(screen)) {
            tab->setEngineBusy(busy);
        }
    });
    // Idle again: the model pages follow the model now loaded (the Downloads delete lock).
    if (!busy && parts_.downloads != nullptr) {
        parts_.downloads->refreshModelViews();
    }
}

// set_transcription_stats (843-859).
void UiController::setTranscriptionStats(const dictation::TranscriptionStats& stats) {
    if (parts_.upload != nullptr && parts_.upload->setTranscriptionStats(stats)) {
        return;
    }
    applyToQuickRecord("set_transcription_stats", [stats](QuickRecordTab* tab) {
        tab->setTranscriptionStats(stats.transcription_time, stats.audio_duration, stats.file_size,
                                   stats.cleanup_time.value_or(0.0), stats.remote);
    });
}

// clear_transcription_stats (861-866): the Upload tab is built on first visit, and a recording
// must not build it.
void UiController::clearTranscriptionStats() {
    applyToQuickRecord("clear_transcription_stats",
                       [](QuickRecordTab* tab) { tab->clearTranscriptionStats(); });
    if (QWidget* upload = parts_.screens->existing(screen_key::upload)) {
        if (upload->metaObject()->indexOfMethod("clearTranscriptionStats()") >= 0) {
            QMetaObject::invokeMethod(upload, "clearTranscriptionStats");
        }
    }
}

// show_notice (992-999).
void UiController::showNotice(const QString& text, bool done) {
    router_->showNotice(text, done, parts_.window->isActiveWindow());
}

void UiController::showCopiedAnimation() {
    router_->showCopiedAnimation();
}

void UiController::setHandsFree(bool on) {
    router_->setHandsFree(on);
}

// on_recording_device_switched (1369-1372).
void UiController::onRecordingDeviceSwitched(const QString& /*oldName*/, const QString& newName) {
    const QString name =
        newName.isEmpty() ? QStringLiteral("the system default microphone") : newName;
    setStatus(QStringLiteral("Microphone disconnected — continuing on %1").arg(name));
    router_->showCaption(QStringLiteral("Switched to %1").arg(name));
}

// ApplicationController._on_recording_state_changed (1278-1283).
void UiController::onRecordingStateChanged(bool recording) {
    isRecording_ = recording;
    if (recording) {
        onDictationStarted(qt::toQString(parts_.transcription->recording_profile()));
    }
    parts_.window->setRecordingActive(recording);
}

// on_dictation_started (746-748).
void UiController::onDictationStarted(const QString& profileId) {
    if (parts_.upload != nullptr) {
        // start_recording and on_dictation_started: the recording's results are Quick Record's.
        parts_.upload->recordingStarted();
    }
    applyToQuickRecord("set_recording_profile",
                       [profileId](QuickRecordTab* tab) { tab->setRecordingProfile(profileId); });
}

// on_dictionary_term_learned (1374-1383).
void UiController::onDictionaryTermLearned(const QString& term, bool protectedTerm) {
    setStatus(protectedTerm ? QStringLiteral("AI cleanup will keep \"%1\" as you said it").arg(term)
                            : QStringLiteral("Added \"%1\" to your dictionary").arg(term));
    if (parts_.settingsWindow != nullptr) {
        if (SettingsDialog* dialog = parts_.settingsWindow->dialog()) {
            dialog->refreshPage(destinations::dictionary);
        }
    }
}

// refresh_cleanup_controls (1748-1750).
void UiController::refreshCleanupControls() {
    applyToQuickRecord("load_cleanup_setting",
                       [](QuickRecordTab* tab) { tab->loadCleanupSetting(); });
    parts_.screens->update(RecordFeature::kUploadScreen, "load_cleanup_setting",
                           [](QWidget* screen) {
                               if (auto* tab = qobject_cast<UploadFileTab*>(screen)) {
                                   tab->loadCleanupSetting();
                               }
                           });
}

// open_cleanup_profiles (1590-1594).
void UiController::openCleanupProfiles() {
    if (parts_.settingsWindow != nullptr) {
        parts_.settingsWindow->open(qt::toQString(destinations::cleanup_profiles));
    }
}

// _on_cleanup_profiles_changed (1596-1599).
void UiController::onCleanupProfilesChanged() {
    applyToQuickRecord("refresh_cleanup_profiles",
                       [](QuickRecordTab* tab) { tab->refreshCleanupProfiles(); });
    if (onCleanupProfilesChangedHook) {
        onCleanupProfilesChangedHook();
    }
}

// _on_profile_hotkey_capture (1601-1603).
void UiController::onProfileHotkeyCapture(bool suspended) {
    if (onProfileHotkeyCaptureHook) {
        onProfileHotkeyCaptureHook(suspended);
    }
}

// _on_live_preview_changed (639-643), after MainWindow._on_live_preview_changed refreshed the
// tabs.
void UiController::onLivePreviewChanged() {
    kLog.info("Live preview toggled via main GUI");
    parts_.record->refreshLivePreviewControls();
    if (parts_.streaming != nullptr) {
        parts_.streaming->reconfigure();
    }
}

void UiController::cycleDictationLanguage() {
    cycleLanguage(*languageUi_);
}

void UiController::populateLanguageMenu(QMenu* menu) {
    if (menu != nullptr) {
        ::fv::populateLanguageMenu(menu, *languageUi_);
    }
}

// update_hotkey_display (2518-2537); MainWindow.update_hotkeys forwards to Quick Record and the
// compact controller.
void UiController::updateHotkeyDisplay(const std::map<std::string, std::string>& hotkeys) {
    const QString record = displayOf(hotkeys, "record_toggle", true);
    const QString cancel = displayOf(hotkeys, "cancel", true);
    const QString enableDisable = displayOf(hotkeys, "enable_disable", true);
    const QString minimize = displayOf(hotkeys, "minimize_tray", true);
    const QString walkthrough = displayOf(hotkeys, "walkthrough", false);
    const QString screenshot = displayOf(hotkeys, "screenshot", false);
    parts_.window->updateHotkeys(record, cancel, enableDisable, minimize, walkthrough, screenshot);
    applyToQuickRecord("update_hotkeys", [record, cancel, enableDisable](QuickRecordTab* tab) {
        tab->updateHotkeys(record, cancel, enableDisable);
    });
    if (CompactRecordController* compact = compactController()) {
        compact->updateHotkeys(record, cancel);
    }
}

// ui_controller.py:1415-1420; the update notice's refresh joins it with the updater (M3).
void UiController::settingsPageChanged(const QString& kind) {
    if (onSettingsChanged) {
        onSettingsChanged(qt::toUtf8(kind));
    }
}

void UiController::refreshHistory() {
    if (parts_.history != nullptr) {
        parts_.history->refresh();
    }
}

// cleanup (2892-2930), the parts M1 has. The tray and the window are Bootstrap's to close.
void UiController::cleanup() {
    kLog.info("Starting UI Controller cleanup...");
    parts_.clipboard->cleanup();
    try {
        router_->cleanup();
    } catch (const std::exception& error) {
        kLog.debug("Error closing overlay: {}", error.what());
    }
    kLog.info("UI Controller cleaned up");
}

} // namespace fv
