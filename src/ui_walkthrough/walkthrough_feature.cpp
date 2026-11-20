#include "ui_walkthrough/walkthrough_feature.h"

#include <QPointer>

#include "appcore/activity.h"
#include "appcore/cancel_router.h"
#include "appcore/overlay_state.h"
#include "appcore/shutdown_plan.h"
#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/qt_bridge.h"
#include "base/utf8.h"
#include "clipboard/temporary_clipboard.h"
#include "dictation/hotkey_runtime.h"
#include "dictation/recorder_port.h"
#include "dictation/transcription_runtime.h"
#include "dictation/walkthrough_delivery.h"
#include "dictation/walkthrough_recording.h"
#include "documents/qt_renderer.h"
#include "focus/capture_service.h"
#include "focus/synthetic_keys.h"
#include "hotkeys/conflicts.h"
#include "hotkeys/grammar.h"
#include "hotkeys/hotkey_manager.h"
#include "hotkeys/mouse_hook.h"
#include "recorder/recorder.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_history/history_feature.h"
#include "ui_history/history_sidebar.h"
#include "ui_overlay/waveform_overlay.h"
#include "ui_settings/settings_window.h"
#include "ui_shell/bootstrap.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"
#include "ui_snap/live_drawing_port.h"
#include "ui_snap/snap_port.h"
#include "ui_snap/support.h"
#include "ui_walkthrough/keep_out.h"
#include "ui_walkthrough/panel_port.h"
#include "ui_walkthrough/past_walkthroughs.h"
#include "ui_walkthrough/walkthrough_library.h"
#include "ui_walkthrough/walkthrough_panel.h"
#include "ui_walkthrough/walkthrough_presenter.h"
#include "ui_walkthrough/walkthrough_ready.h"
#include "ui_walkthrough/walkthrough_tab.h"
#include "ui_widgets/tabbed_content.h"
#include "walkthrough/library.h"
#include "walkthrough/paste.h"
#include "walkthrough/renderer.h"
#include "walkthrough/runtime.h"
#include "walkthrough/storage.h"

namespace fv {
namespace {
class OverlayPort final : public walkthrough::OverlayPort {
public:
    explicit OverlayPort(WaveformOverlay& overlay) : overlay_(overlay) {}
    void show_caption(const std::string& text) override {
        overlay_.showCaption(qt::toQString(text));
    }
    bool keep_out_of_screenshots(bool enabled) override {
        return setKeepOutOfScreenshots(&overlay_, enabled);
    }

private:
    WaveformOverlay& overlay_;
};
} // namespace

struct WalkthroughFeature::State final : QObject, dictation::WalkthroughDeliveryPort {
    explicit State(AppContext& c)
        : context(c), window(c.service<MainWindow>()),
          transcription(c.service<dictation::TranscriptionRuntime>()),
          clipboard(c.service<clipboard::TemporaryClipboard>()),
          overlay(c.service<WaveformOverlay>()), overlayPort(overlay),
          // The walkthrough's Cancel is the user's: the recording or job, then the write-up,
          // then the actions still running.
          recording(transcription, c.service<dictation::RecorderAdapter>(), c.activity(),
                    [&router = c.cancel()] { router.cancel(); }),
          snap(&c.settings(),
               ui_snap::QtSnapPort::Hooks{[this] {
                                              return presenter ? presenter->snapKeepOut()
                                                               : QList<QPointer<QWidget>>{};
                                          },
                                          [this] {
                                              return presenter ? presenter->snapFloatAbove()
                                                               : std::vector<ui_snap::Parkable>{};
                                          },
                                          {}}),
          pool("fv-walkthrough-library", 1),
          strand(pool, "walkthrough-library", 1, ShutdownMode::Drain) {
        if (!ui_snap::currentPlatform().wayland) {
            panel = new WalkthroughPanel(c.clock());
            panelPort = std::make_unique<WalkthroughPanelPort>(*panel);
        }
    }
    ~State() override {
        links.clear();
        transcription.set_walkthrough_delivery(nullptr);
        transcription.set_walkthrough_strand(nullptr);
        if (runtime)
            runtime->cleanup();
        overlay.setStandIn({});
        presenter.reset();
        overlay.setWalkthroughControls(false);
        strand.shutdown(std::chrono::seconds(2));
        pool.shutdown(std::chrono::seconds(2));
        delete ready;
        delete panel;
    }
    bool copy_files_to_clipboard(const std::vector<std::filesystem::path>& files,
                                 const std::string& text) override {
        focus::ClipboardFiles content;
        content.paths = files;
        content.text = text;
        return clipboard.write_files(std::move(content));
    }
    void show_walkthrough_ready(const walkthrough::WalkthroughResult& result,
                                bool copied) override {
        if (panel)
            panel->complete(result.shots, result.duration_s);
        if (!ready) {
            ready = new WalkthroughReadyCard;
            connect(ready.data(), &WalkthroughReadyCard::reviewRequested, this,
                    [this](const QString& folder) {
                        if (openLibrary)
                            openLibrary(folder);
                    });
            connect(ready.data(), &WalkthroughReadyCard::copyRequested, this,
                    [this](const QString&, const QString& format) {
                        if (ready && ready->result())
                            copyAgain(*ready->result(), qt::toUtf8(format));
                    });
        }
        ready->present(result, copied, context.settings().get(keys::auto_paste),
                       context.settings().get(keys::walkthrough_copy_format));
        if (past)
            past->refresh();
        if (tab)
            tab->showWalkthrough(result.folder);
        // A write-up lands long after History was written: its card's title and "AI write-up"
        // label are only on disk until the panel reloads.
        if (auto* history = context.find<HistoryPanel>())
            history->refresh();
        if (library && library->isVisible() && !library->review()->isBusy() &&
            !library->review()->isDirty())
            library->refresh(result.folder);
    }
    void paste_walkthrough(const walkthrough::WalkthroughResult& result,
                           std::optional<focus::AppIdentity> target) override {
        if (!paste)
            return;
        // ui_controller.paste_walkthrough: one paste at a time, each report naming the
        // walkthrough's own warnings and shown on its ready card.
        if (paste->busy()) {
            window.setStatus(
                QStringLiteral("Walkthrough saved; another walkthrough paste is in progress"));
            return;
        }
        pasting = result;
        paste->paste(result, std::move(target));
        if (paste->busy())
            reportPaste("Pasting PDF, then instructions\xE2\x80\xA6");
    }
    void reportPaste(const std::string& status) {
        QStringList parts{qt::toQString(status)};
        if (pasting && !pasting->write_up_error.empty())
            parts.append(QStringLiteral("Write-up failed: ") +
                         qt::toQString(pasting->write_up_error));
        if (pasting && pasting->lost_shots > 0)
            parts.append(
                QStringLiteral("%1 screenshot(s) couldn't be saved").arg(pasting->lost_shots));
        window.setStatus(parts.join(QStringLiteral(". ")));
        if (ready && pasting)
            ready->showPasteStatus(*pasting, qt::toQString(status));
    }
    // ui_controller._copy_walkthrough_again: the result as it is on disk now.
    void copyAgain(const walkthrough::WalkthroughResult& shown, const std::string& format) {
        try {
            const auto result = walkthrough::load_result(shown.folder);
            auto [files, text] = result.clipboard(format);
            window.setStatus(copy_files_to_clipboard(files, text)
                                 ? QStringLiteral("Walkthrough copied")
                                 : QStringLiteral("Couldn't copy the walkthrough"));
        } catch (const std::exception& error) {
            window.setStatus(QStringLiteral("Couldn't open the walkthrough: ") +
                             QString::fromUtf8(error.what()));
        }
    }
    void dismiss_walkthrough_panel() override {
        if (panel)
            panel->dismiss();
    }
    AppContext& context;
    MainWindow& window;
    dictation::TranscriptionRuntime& transcription;
    clipboard::TemporaryClipboard& clipboard;
    WalkthroughPanel* panel = nullptr;
    std::unique_ptr<WalkthroughPanelPort> panelPort;
    WaveformOverlay& overlay;
    OverlayPort overlayPort;
    dictation::WalkthroughRecording recording;
    ui_snap::QtSnapPort snap;
    std::weak_ptr<ui_snap::QtLiveDrawingPort> live;
    std::unique_ptr<WalkthroughPresenter> presenter;
    ThreadPool pool;
    Strand strand;
    std::unique_ptr<walkthrough::WalkthroughRuntime> runtime;
    std::unique_ptr<walkthrough::WalkthroughPaste> paste;
    // The walkthrough the paste in progress delivers, for its reports.
    std::optional<walkthrough::WalkthroughResult> pasting;
    QPointer<WalkthroughTab> tab;
    QPointer<PastWalkthroughsPanel> past;
    QPointer<WalkthroughReadyCard> ready;
    QPointer<WalkthroughLibraryDialog> library;
    QPointer<SystemTray> tray;
    ReviewHost host;
    std::function<void(const QString&)> openLibrary;
    std::vector<Subscription> links;
};

WalkthroughFeature::WalkthroughFeature() = default;
WalkthroughFeature::WalkthroughFeature(walkthrough::SnapPort* snap, ClickHook clicks)
    : snap_(snap), clicks_(std::move(clicks)) {}
WalkthroughFeature::~WalkthroughFeature() = default;
void WalkthroughFeature::provide(AppContext&) {
    walkthrough::set_document_renderer(std::make_shared<documents::QtDocumentRenderer>());
}
void WalkthroughFeature::install(AppContext& c) {
    state_ = std::make_unique<State>(c);
    State* s = state_.get();
    auto root = walkthrough::captures_root(c.paths());
    QPointer<State> hostState = s;
    s->host.copyFiles = [hostState](const auto& paths, const QString& text, const QImage& image) {
        return hostState &&
               hostState->clipboard.writeFiles(
                   paths, text, image.isNull() ? std::nullopt : std::optional<QImage>(image));
    };
    s->host.recover = [hostState](const QString& folder) {
        if (hostState)
            hostState->transcription.recover_walkthrough(
                base_utf8::path_from_utf8(qt::toUtf8(folder)));
    };
    s->host.submit = [hostState](Task task) {
        if (!hostState)
            return false;
        hostState->strand.post(std::move(task));
        return true;
    };
    s->host.acquire = [hostState]() -> std::optional<QString> {
        if (!hostState)
            return QStringLiteral("Walkthroughs are unavailable.");
        if (auto reason = walkthrough_library_refusal(hostState->context.activity().snapshot()))
            return qt::toQString(*reason);
        hostState->context.activity().set_library_change(true);
        return std::nullopt;
    };
    s->host.release = [hostState] {
        if (hostState)
            hostState->context.activity().set_library_change(false);
    };
    c.screens().define(
        screen_key::walkthrough, order::tab::walkthrough, QStringLiteral("Walkthrough"),
        [hostState, root](QWidget* parent) -> QWidget* {
            State* s = hostState.data();
            if (!s)
                throw std::runtime_error("Walkthroughs are unavailable.");
            WalkthroughTabOptions options;
            options.capturesRoot = root;
            options.io = &s->context.io();
            auto* tab = new WalkthroughTab(s->context.settings(), s->host, options, parent);
            s->tab = tab;
            // The storyboard keeps a height the user drags, as an open transcript does.
            s->window.setPageElastic(tab, tab->fillsHeight());
            QObject::connect(tab, &WalkthroughTab::walkthroughRequested, s, [s] {
                if (s->runtime)
                    s->runtime->toggle("ui");
            });
            QObject::connect(tab, &WalkthroughTab::cancelRequested, s, [s] {
                if (s->runtime)
                    s->runtime->cancel();
            });
            QObject::connect(tab, &WalkthroughTab::settingsRequested, s,
                             [s](const QString& destination) {
                                 if (auto* settings = s->context.find<SettingsWindow>())
                                     settings->open(destination);
                                 else
                                     s->window.openSettings();
                             });
            QObject::connect(tab, &WalkthroughTab::walkthroughsChanged, s, [s] {
                if (s->past)
                    s->past->refresh();
                // ui_controller._refresh_walkthrough_library: an open, idle library shows what
                // the tab saved or deleted.
                WalkthroughLibraryDialog* library = s->library.data();
                if (library && library->isVisible() && !library->review()->isBusy() &&
                    !library->review()->isDirty())
                    library->refresh(library->review()->folder());
            });
            return tab;
        });
    auto openLibrary = [s, root](const QString& folder) {
        if (!s->library) {
            s->library =
                new WalkthroughLibraryDialog(s->context.settings(), root, s->host, &s->window);
            QObject::connect(s->library.data(), &WalkthroughLibraryDialog::changed, s, [s] {
                if (s->tab)
                    s->tab->reloadWalkthrough();
                if (s->past)
                    s->past->refresh();
            });
        }
        s->library->openFolder(folder.isEmpty()
                                   ? std::nullopt
                                   : std::optional<std::filesystem::path>(
                                         base_utf8::path_from_utf8(qt::toUtf8(folder))));
    };
    s->openLibrary = openLibrary;
    QObject::connect(&s->window, &MainWindow::walkthroughLibraryRequested, s, openLibrary);
    if (auto* history = c.find<HistoryPanel>(); history && history->sidebar()) {
        s->past = new PastWalkthroughsPanel(root);
        s->past->setExecutor(&c.io());
        history->sidebar()->setPageWidget(HistorySidebar::Page::Walkthroughs, s->past);
        QObject::connect(
            s->past.data(), &PastWalkthroughsPanel::walkthroughSelected, s,
            [s, openLibrary](const QString& folder) {
                if (folder.isEmpty()) {
                    openLibrary(folder);
                    return;
                }
                s->window.tabbedContent()->setCurrentIndex(TabbedContentWidget::TabWalkthrough);
                if (s->tab)
                    s->tab->showWalkthrough(base_utf8::path_from_utf8(qt::toUtf8(folder)), true);
            });
    }
    walkthrough::RuntimeServices services{
        c.ui(),        c.clock(),    c.settings(),
        c.paths(),     s->recording, snap_ ? *snap_ : static_cast<walkthrough::SnapPort&>(s->snap),
        s->overlayPort};
    services.live = [s](auto labels) {
        // Keep marks, Infill, Snap and the delay chip are read and saved where Snap's are.
        ui_snap::LiveAnnotations::Options options;
        options.settings = &s->context.settings();
        options.clock = &s->context.clock();
        auto drawing = std::make_shared<ui_snap::QtLiveDrawingPort>(s->panel, labels, options);
        s->live = drawing;
        return drawing;
    };
    services.clicks = [make = clicks_](auto callback) -> std::unique_ptr<Subscription> {
        auto hook = std::shared_ptr<hotkeys::MouseHookSource>(
            make ? make(std::move(callback))
                 : hotkeys::make_mouse_hook_source(std::move(callback)));
        hook->start({}, {});
        if (!hook->wait_until_installed()) {
            hook->stop();
            return nullptr;
        }
        return std::make_unique<Subscription>(std::make_shared<CallbackGuard>(),
                                              [hook] { hook->stop(); });
    };
    services.marks = [&manager = c.service<hotkeys::HotkeyManager>()](auto bindings,
                                                                      auto callback) {
        hotkeys::HotkeyList list;
        for (const auto& [action, key] : bindings)
            list.emplace_back(action, key);
        manager.set_dynamic_hotkeys("walkthrough_marks", list, std::move(callback));
    };
    services.hotkey_conflict = [&settings = c.settings()](const std::string& key) {
        return hotkeys::hotkey_conflict(key, *settings.snapshot());
    };
    services.tell = [s](std::string text, bool done) {
        s->window.setStatus(qt::toQString(text));
        s->overlay.showNotice(qt::toQString(text), done);
    };
    services.copy_image = [s](auto image, auto path, std::string text) {
        focus::ClipboardFiles files;
        if (path)
            files.paths.push_back(*path);
        files.text = std::move(text);
        files.png_image = image->png_bytes();
        return s->clipboard.write_files(std::move(files));
    };
    s->runtime = std::make_unique<walkthrough::WalkthroughRuntime>(std::move(services));
    s->runtime->attach_panel(s->panelPort.get());
    auto& capture = c.service<focus::CaptureService>();
    s->paste = std::make_unique<walkthrough::WalkthroughPaste>(
        c.ui(), s->clipboard, c.service<focus::SyntheticKeys>(),
        [&capture] { return capture.current_identity(); },
        [s](std::string_view token) { return s->clipboard.markerPresent(token); },
        [s](std::string text) { s->reportPaste(text); });
    s->transcription.set_walkthrough_strand(&s->strand);
    s->transcription.set_walkthrough_delivery(s);
    // ui_controller's walkthrough glue: the column stands in for the overlay (not during the AI
    // write-up), what a snap keeps out and parks, and the buttons that reach the runtime.
    WalkthroughPresenter::Parts parts;
    parts.overlay = &s->overlay;
    parts.panel = s->panel;
    parts.setTrayActive = [s](bool active) {
        if (s->tray)
            s->tray->setWalkthroughActive(active);
    };
    parts.setWindowActive = [s](bool active) {
        s->window.setWalkthroughActive(active);
    };
    parts.windowVisible = [s] {
        return s->window.isVisible();
    };
    parts.minimizeToTray = [s] {
        s->window.minimizeToTray();
    };
    parts.minimizeTicked = [s] {
        return s->context.settings().get(keys::walkthrough_minimize_to_tray);
    };
    parts.writingUp = [s] {
        return s->transcription.writing_up();
    };
    parts.drawingWindows = [s] {
        QList<QWidget*> windows;
        if (auto drawing = s->live.lock(); drawing && drawing->live()) {
            for (QWidget* window : drawing->live()->windows())
                windows.append(window);
        }
        return windows;
    };
    parts.onWalkthrough = [s](const std::string& source) {
        s->runtime->toggle(source);
    };
    parts.onScreenshot = [s](const std::string& source) {
        s->runtime->snap(source);
    };
    s->presenter = std::make_unique<WalkthroughPresenter>(std::move(parts));
    WalkthroughPresenter* presenter = s->presenter.get();
    QObject::connect(&s->overlay, &WaveformOverlay::walkthroughSnapRequested, s,
                     [presenter] { presenter->onOverlaySnap(); });
    QObject::connect(&s->overlay, &WaveformOverlay::walkthroughFinishRequested, s,
                     [presenter] { presenter->onOverlayFinish(); });
    QObject::connect(&s->window, &MainWindow::walkthroughRequested, s,
                     [presenter] { presenter->onWindowWalkthrough(); });
    auto& hk = c.service<dictation::HotkeyRuntime>();
    const auto updateHotkeys = [s](const std::map<std::string, std::string>& map) {
        const auto label = [&map](const char* key) {
            const auto it = map.find(key);
            return it == map.end() ? QString()
                                   : qt::toQString(hotkeys::format_hotkey_display(
                                         it->second, hotkeys::platform_vocabulary()));
        };
        const QString walkthrough = label("walkthrough"), screenshot = label("screenshot");
        s->context.screens().update(screen_key::walkthrough, "hotkeys",
                                    [walkthrough, screenshot](QWidget* widget) {
                                        if (auto* tab = qobject_cast<WalkthroughTab*>(widget))
                                            tab->setHotkeys(walkthrough, screenshot);
                                    });
        s->overlay.setWalkthroughShortcuts(screenshot, walkthrough);
        if (s->panel)
            s->panel->setSnapMode(s->panel->snapMode(), screenshot);
    };
    updateHotkeys(c.service<hotkeys::HotkeyManager>().hotkeys());
    s->links.push_back(hk.hotkeys_changed.connect(c.ui(), updateHotkeys));
    s->links.push_back(hk.walkthrough_requested.connect(c.ui(), [s] { s->runtime->toggle(); }));
    s->links.push_back(hk.screenshot_requested.connect(c.ui(), [s] { s->runtime->snap(); }));
    s->links.push_back(hk.paste_walkthrough_requested.connect(
        c.ui(), [s, root](std::optional<focus::AppIdentity> target) {
            QPointer<State> guarded = s;
            Executor* ui = &s->context.ui();
            s->context.io().post([guarded, root, target = std::move(target), ui] {
                std::optional<walkthrough::WalkthroughResult> result;
                QString failure;
                try {
                    result = walkthrough::latest_result(root);
                } catch (const std::exception& error) {
                    failure = QStringLiteral("Couldn't open the latest walkthrough: ") +
                              QString::fromUtf8(error.what());
                }
                ui->post([guarded, result = std::move(result), failure, target] {
                    if (!guarded)
                        return;
                    if (!failure.isEmpty())
                        guarded->window.setStatus(failure);
                    else if (result)
                        guarded->paste_walkthrough(*result, target);
                    else
                        guarded->window.setStatus(
                            QStringLiteral("No completed walkthrough to paste yet"));
                });
            });
        }));
    s->links.push_back(s->runtime->walkthrough_active.connect(
        c.ui(), [presenter](bool active) { presenter->setWalkthroughActive(active); }));
    s->links.push_back(s->transcription.recording_state_changed().connect(
        c.ui(), [s](bool on) { s->runtime->on_recording_state_changed(on); }));
    // A cancelled or failed job closes the column, unless it belongs to the AI write-up.
    s->links.push_back(s->transcription.transcription_failed.connect(
        c.ui(), [presenter](std::string) { presenter->dismissPanelForJob(); }));
    s->links.push_back(s->transcription.overlay_state_update.connect(
        c.ui(), [presenter](appcore::OverlayState state) {
            if (state == appcore::OverlayState::Canceling)
                presenter->dismissPanelForJob();
        }));
    s->links.push_back(c.service<Recorder>().level.connect(
        c.ui(), [presenter](float level) { presenter->hear(level); }));
    QPointer<State> guardedState = s;
    c.cancel().add(20, [guardedState] {
        if (!guardedState)
            return false;
        if (guardedState->library && guardedState->library->review()->isBusy()) {
            guardedState->library->review()->cancel();
            return true;
        }
        if (guardedState->tab && guardedState->tab->review()->isBusy()) {
            guardedState->tab->review()->cancel();
            return true;
        }
        return false;
    });
    c.shutdown().add(ShutdownStep::AutoMode, "Error stopping Auto Mode", [guardedState] {
        if (guardedState && guardedState->runtime)
            guardedState->runtime->cleanup();
    });
    c.shutdown().add(ShutdownStep::WalkthroughLibrary, "Error stopping walkthrough library",
                     [guardedState] {
                         if (!guardedState)
                             return;
                         if (guardedState->library)
                             guardedState->library->review()->cancel();
                         if (guardedState->tab)
                             guardedState->tab->review()->cancel();
                     });
    if (auto* life = c.find<ShellLifecycle>()) {
        auto previous = life->trayCreated;
        QPointer<State> guarded = s;
        life->trayCreated = [previous, guarded](SystemTray& tray) {
            if (previous)
                previous(tray);
            if (!guarded)
                return;
            guarded->tray = &tray;
            QObject::connect(&tray, &SystemTray::walkthroughRequested, guarded.data(), [guarded] {
                if (guarded)
                    guarded->presenter->onTrayWalkthrough();
            });
            QObject::connect(&tray, &SystemTray::screenshotRequested, guarded.data(), [guarded] {
                if (guarded)
                    guarded->presenter->onTrayScreenshot();
            });
        };
    }
}
} // namespace fv
