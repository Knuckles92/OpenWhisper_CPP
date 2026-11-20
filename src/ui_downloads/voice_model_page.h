#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QString>
#include <QWidget>

#include "appkit/settings_page.h"
#include "base/clock.h"
#include "base/json.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/speech_server_controls.h"

class QComboBox;
class QFrame;
class QLabel;

namespace fv {

class Button;
class DictationLanguagesField;
class Executor;
class SettingsStore;
class WrappedLabel;

namespace http {
class Client;
}

// What the Voice model page needs from the Settings window around it (the parts of
// settings_dialog.py the page's ModelAssignments calls: message_label, notify_changed and the
// rail). Affinity: GUI thread.
class VoiceModelPageHost {
public:
    virtual ~VoiceModelPageHost() = default;

    // message_label.setText: the footer line (an error, or "" to clear it).
    virtual void showMessage(const QString& message) = 0;
    // notify_changed(kind): "languages" after the dictation languages changed.
    virtual void notifyChanged(const QString& kind) = 0;
    // The rail's value under Voice model follows the saved engine and model (_refresh_rail_values).
    virtual void refreshRailValues() = 0;
    // focus_api_keys(env): API keys with the credential of the selected server ("" for none).
    virtual void focusApiKeys(const QString& env) = 0;

protected:
    VoiceModelPageHost() = default;
    VoiceModelPageHost(const VoiceModelPageHost&) = default;
    VoiceModelPageHost& operator=(const VoiceModelPageHost&) = default;
};

// What the page reads of this computer, so a test answers them without a disk, a driver or a
// network. Every member is optional.
struct VoiceModelPageOptions {
    // The keys of the speech models downloaded on this computer (SpeechModelCache::inventory).
    // Runs on the io executor, never on the GUI thread; what it throws reads as "the lookup
    // failed". Unset, nothing is on this computer.
    std::function<std::vector<std::string>()> cachedModels;
    // The last scan of what is downloaded that any window made, without touching the disk
    // (peek_cached_models: CachedModelsSnapshot::peek in the app); nullopt before the first. A
    // new page shows it until its own scan answers. Unset: nothing is known before that scan.
    std::function<std::optional<std::vector<std::string>>()> peekCachedModels;
    // The tail of the inventory line for a backend on a device: engine_runtime_label over the
    // saved settings and this computer's host. Runs on the io executor, since with device Auto
    // it can be the process's first NVML probe; what it throws reads as kRuntimeCheckFailed.
    // Unset, the tail says the runtime could not be checked.
    std::function<std::string(const std::string& backend, const std::string& device)> runtimeLabel;
    // The API key of a speech endpoint (speech::api_key over the credential store); runs on the
    // io executor.
    SpeechServerControls::KeyProvider apiKey;
    // The clock the OpenAI retirement dates are judged by (the API server's model list and the
    // rewrite of a pre-2.0 API engine name); the system clock when unset.
    const Clock* clock = nullptr;
};

// Settings > Voice model (settings_models.py build_voice_page and the handlers behind it): the
// recording engine, its caption, the local engine's Model, Device and Language fields or the API
// server's Server and Model fields, "Languages I dictate in", what the engine has on this
// computer with "Get models and runtimes", and a footnote on live previews.
//
// Every choice is saved as it is made. A change of engine goes through the controller's hook
// (setBackendHandler: the engine reloads there), or is saved directly when there is none; one
// that fails shows "Couldn't change voice model: <why>" and the combo goes back to the saved
// engine. The page follows the saved engine whoever changes it, through loadSettings and
// refreshEngineSelection.
//
// OpenWhisper's Whisper model picker (shown for Local Whisper only) is not here: Parakeet's
// models are chosen in the Model field of the local engine row. What is on this computer is read
// by a worker (refresh), delivered by generation, and never held by it: a page closed while it
// runs is simply not told.
// Affinity: GUI thread.
class VoiceModelPage : public QWidget, public SettingsPageModule {
    Q_OBJECT

public:
    // The settings and the dialog's helpers outlive the page. `http` and `io` serve the API
    // server's catalog requests and the scan of what is downloaded (both must outlive every
    // request the page started); `catalog` is the speech catalog the local fields read.
    VoiceModelPage(SettingsStore& settings, VoiceModelPageHost& host, http::Client& http,
                   Executor& io, LocalEngineCatalog catalog, VoiceModelPageOptions options = {},
                   QWidget* parent = nullptr);
    ~VoiceModelPage() override;

    // The controller's hook for a change of engine (on_backend_changed): given the engine's
    // display name ("Parakeet", "API", "Remote computer"). It saves the choice; what it throws
    // is shown and the choice reverted. A change of the API server or model calls it with "API".
    // Without one the page saves selected_model itself.
    using BackendHandler = std::function<void(const QString& displayName)>;
    void setBackendHandler(BackendHandler handler) { backendHandler_ = std::move(handler); }

    // The rail's value: the engine and model (voice_summary).
    static QString railValue(const Json& settings);

    // choose_backend: ignores an id that is not an engine, applies the choice (hook or save),
    // then shows it. false when it was ignored or failed.
    bool chooseBackend(const QString& backend);
    // refresh(scan): reloads the saved engine, redraws the inventory from what is known and, with
    // scan, asks the worker what is downloaded now.
    void refresh(bool scan = true);
    // refresh_engine_selection: the engine was saved elsewhere (the main window, the controller,
    // a paired computer): shows it.
    void refreshEngineSelection();
    // _on_engine_runtime_checked: a runtime lookup for (backend, device) finished with `label`.
    // The label is kept for that pair; it is shown only while the page is on that backend at that
    // device, so a result for an old device never overwrites a newer choice.
    void applyRuntimeLabel(const std::string& backend, const std::string& device,
                           const std::string& label);

    // The engine the combo shows ("api", "parakeet" or "remote").
    QString currentEngine() const;
    // The Downloads filter "Get models and runtimes" opens (engine_filter).
    QString engineFilter() const;

    QComboBox* engineCombo() const { return engineCombo_; }
    WrappedLabel* engineCaption() const { return caption_; }
    LocalEngineControls* speechControls() const { return speechControls_; }
    SpeechServerControls* apiServer() const { return apiServer_; }
    DictationLanguagesField* dictationLanguages() const { return languages_; }
    QLabel* inventoryTitle() const { return inventoryTitle_; }
    QWidget* inventoryRow() const { return inventoryRow_; }
    WrappedLabel* inventoryLabel() const { return inventoryLabel_; }
    Button* downloadsButton() const { return downloadsButton_; }
    WrappedLabel* footnote() const { return footnote_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    void refreshPage() override { refresh(true); }
    void onShown() override { refresh(true); }
    QWidget* namedControl(std::string_view name) override;

    // The names namedControl answers (MODEL_CONTROL_DESTINATIONS' voice_model entries, less the
    // Whisper picker).
    static const std::vector<std::string>& controlNames();

Q_SIGNALS:
    // A change of engine was applied; displayName as setBackendHandler's.
    void backendChanged(const QString& displayName);
    // A local engine's model, device or language changed (on_runtime_settings_changed): the
    // controller reloads the engine.
    void engineSettingsChanged();
    // "Get models and runtimes": Downloads, filtered to this engine ("" for none).
    void downloadsRequested(const QString& filter);
    // A help link of the API server's fields was followed (the destination it opens).
    void helpRequested(const QString& destination);

private:
    struct Mailbox;

    void onEngineComboChanged();
    void onSpeechSettingsChanged();
    void onApiServerChanged();
    void onApiHelp(const QString& destination);
    void onLanguagesChanged();
    void loadEngineAndRuntime();
    void updateEngineCaption();
    void updateEngineFields();
    void refreshEngineInventory();
    void startScan();
    void startRuntimeCheck(const std::string& backend, const std::string& device);
    void onScanFinished(const std::optional<std::set<std::string>>& found);
    QFrame* buildFootnote();

    SettingsStore& settings_;
    VoiceModelPageHost& host_;
    http::Client& http_;
    Executor& io_;
    LocalEngineCatalog catalog_;
    VoiceModelPageOptions options_;
    BackendHandler backendHandler_;

    QComboBox* engineCombo_ = nullptr;
    WrappedLabel* caption_ = nullptr;
    LocalEngineControls* speechControls_ = nullptr;
    SpeechServerControls* apiServer_ = nullptr;
    DictationLanguagesField* languages_ = nullptr;
    QLabel* inventoryTitle_ = nullptr;
    QWidget* inventoryRow_ = nullptr;
    WrappedLabel* inventoryLabel_ = nullptr;
    Button* downloadsButton_ = nullptr;
    WrappedLabel* footnote_ = nullptr;

    // What is downloaded, as the last scan found it (none before the first).
    std::set<std::string> cached_;
    bool scanned_ = false; // this page's own scan has answered
    bool lookupFailed_ = false;
    // Runtime tails by (backend, device), and the pair the inventory line shows.
    std::map<std::pair<std::string, std::string>, std::string> runtimeLabels_;
    std::pair<std::string, std::string> inventoryKey_;
    std::string inventoryCount_;
    std::shared_ptr<Mailbox> mailbox_;
};

} // namespace fv
