#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include <QPointer>

#include "appkit/feature.h"
#include "base/executor.h"
#include "base/json.h"
#include "ui_record/engine_selection.h"
#include "ui_widgets/speech_server_controls.h"

class QWidget;

namespace fv {

class CommandHintReadiness;
class CompactRecordController;
class QuickRecordTab;
class SettingsStore;
class TranscriptionTabBase;
class UploadFileTab;

namespace http {
class Client;
}

// The widgets of the record feature, built on first use and kept: there is one Quick Record tab
// and one compact controller. The tab is also what the main window's screen registry builds when
// the tab is first shown, so the controller and the window reach the same instance. Each tab
// follows the saved engine (and Upload File and the Settings pages join through
// engineSelection()). Provided as a service. The window owns the widgets it was given; a widget
// that never got a parent is deleted with this object. Affinity: GUI thread.
class RecordWidgets {
public:
    RecordWidgets(SettingsStore& settings, Executor& ui);
    ~RecordWidgets();
    RecordWidgets(const RecordWidgets&) = delete;
    RecordWidgets& operator=(const RecordWidgets&) = delete;

    // The tab, created under parent when it does not exist yet, showing the saved engine
    // (MainWindow._load_saved_settings).
    QuickRecordTab* quickRecord(QWidget* parent = nullptr);
    CompactRecordController* compactController(QWidget* parent = nullptr);
    // The Upload File tab, created under parent when it does not exist yet, showing the saved
    // engine (what the main window's screen registry builds on the tab's first show).
    UploadFileTab* upload(QWidget* parent = nullptr);
    // Each, when it exists; null otherwise (nothing is built).
    QuickRecordTab* existingQuickRecord() const;
    UploadFileTab* existingUpload() const;
    EngineSelection& engineSelection() { return engineSelection_; }

    // What the API backend's Server and Model fields need: the shared HTTP client and the io
    // executor (both outlive the tabs), and the key a server's catalog is asked with. Without
    // them the tabs list the API backend with no fields. Applies to tabs built afterwards.
    void setApiServerServices(http::Client* client, Executor* io,
                              SpeechServerControls::KeyProvider keys);
    // MainWindow.refresh_live_preview_controls: every tab shows the saved Live preview choice
    // again, so a flip to another tab agrees after one tab's toggle (the controller calls it
    // from a tab's livePreviewChanged).
    void refreshLivePreviewControls();
    // How Quick Record learns whether the AI model Command Mode rewrites with is set up: `probe`
    // (llm::provider_readiness over the key sources; it blocks) runs on `worker`, the answer
    // comes back on `ui`. Applies to the tab now and when it is built.
    void setCommandReadiness(std::function<std::optional<bool>(const Json& settings)> probe,
                             Executor& worker);

private:
    void addApiServer(TranscriptionTabBase& tab);

    SettingsStore& settings_;
    Executor& ui_;
    EngineSelection engineSelection_;
    http::Client* client_ = nullptr;
    Executor* io_ = nullptr;
    SpeechServerControls::KeyProvider keys_;
    std::shared_ptr<CommandHintReadiness> readiness_;
    QPointer<QuickRecordTab> quickRecord_;
    QPointer<CompactRecordController> compact_;
    QPointer<UploadFileTab> upload_;
};

// Registers the Quick Record screen (appkit/order.h, tab 100) and the Upload File screen (tab
// 200) as keyed screens of the main window's transcription group (fv::kTranscriptionGroup,
// ui_shell/main_window.h), and provides RecordWidgets to the controller, which wires the tab and
// the compact controller to the runtime and the window. Its install() builds and provides the
// UploadPresenter over the transcription runtime and the clipboard, when the controller provided
// them. Affinity: UI actor.
class RecordFeature final : public Feature {
public:
    static constexpr std::string_view kId = "record";
    // The Quick Record screen's key in the ScreenRegistry.
    static constexpr char kQuickRecordScreen[] = "quick_record";
    // The Upload File screen's key (ui_shell's screen_key::upload).
    static constexpr char kUploadScreen[] = "upload";

    std::string_view id() const override { return kId; }
    void provide(AppContext& context) override;
    void install(AppContext& context) override;
};

} // namespace fv
