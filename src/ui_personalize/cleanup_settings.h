#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QString>

#include "audio/backend.h"
#include "base/json.h"
#include "llm/credentials.h"
#include "llm/profiles.h"

class QWidget;

namespace fv {

class AppContext;
class CleanupGates;
class CleanupPage;
class Executor;
class InfoTile;
class Recorder;
class RulesPage;
class SettingsDialog;

// What the AI cleanup and Learned rules pages reach outside the window: the blocking work
// OpenWhisper's pages start on threads of their own (catalog loads, key status, local-server
// probes, rule polish, rule-dictation transcription) and the rule recorder. The Personalize
// feature fills it from the context in provide(); a member it cannot fill stays empty, and the
// work that needs it does not start (a catalog load, a key check, a probe) or reports it cannot
// (a polish fails as "cleanup unavailable", a dictation as unavailable). Tests replace members.
// Every function blocks and runs on the executor named beside it, never on the GUI thread; the
// pages hand results back through qt::post and token checks, so a worker never holds a window.
struct CleanupServices {
    // The GUI executor results come back on (ctx.ui()): it lives as long as the app, so a
    // worker never posts to a window that may be gone; the window is looked up on arrival.
    Executor* ui = nullptr;
    // Key status, model catalogs and local-server probes (the io strand).
    Executor* io = nullptr;
    // Model-specific efforts from the CLI's on-disk cache. On io.
    std::function<std::vector<std::string>(const std::string& model)> codexAgentLevels;
    // Rule polish and rule-dictation transcription (the jobs strand).
    Executor* jobs = nullptr;
    // list_cleanup_models(provider, sort): the endpoint's chat models; throws with the message
    // the picker shows after "Couldn't load models: ". On io.
    std::function<std::vector<std::string>(const std::string& provider, const std::string& sort,
                                           const Json& settings)>
        listModels;
    // credential_status of one endpoint. On io.
    std::function<llm::KeyStatus(const llm::TextLLMProfile& profile)> credentialStatus;
    // detect_running_servers: {base_url: server} of the addresses that answer, in probe order.
    // On io.
    std::function<std::vector<std::pair<std::string, std::string>>(
        const std::vector<std::string>& urls)>
        detectServers;
    // polish_cleanup_rule(raw, provider, model, reasoning): (polished, error). On jobs.
    std::function<std::pair<std::string, std::optional<std::string>>(
        const std::string& raw, const std::string& provider, const std::string& model,
        const std::string& reasoning)>
        polishRule;
    // A recorder for rule dictation on the saved microphone order, saving to `output`. GUI
    // thread.
    std::function<std::shared_ptr<Recorder>(const std::vector<audio::DeviceKey>& priority,
                                            const std::filesystem::path& output)>
        makeRecorder;
    // Whether a meeting holds the microphone (get_meeting_active); false when unset.
    std::function<bool()> meetingActive;
};

// The Settings window `widget` sits in, or null.
SettingsDialog* settingsDialogOf(QWidget* widget);

// The AI cleanup API the rest of the app calls (settings_dialog.py choose_cleanup_level,
// turn_on_cleanup, cleanup_gate_tile, refresh_cleanup_gates, _update_cleanup_prompt_ui,
// focus_cleanup_model and the on_cleanup_changed, on_cleanup_profiles_changed and
// on_profile_hotkey_capture callbacks), provided once with ctx.provide<CleanupSettings>() by the
// Personalize feature. The AI cleanup and Learned rules pages of every Settings window register
// here, and one that is gone is forgotten.
//
// A call that names an `origin` widget saves through the Settings window it sits in (so nothing
// is saved while that window loads, and its rail redraws); without one, through the app's
// Settings window when it exists, else straight to the settings file (guided setup).
// Affinity: GUI thread.
class CleanupSettings : public QObject {
    Q_OBJECT
public:
    // QMessageBox::question's shape, so a test can answer instead of a real box.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;

    CleanupSettings(AppContext& context, CleanupGates& gates, QObject* parent = nullptr);
    ~CleanupSettings() override;

    AppContext& context() const { return context_; }
    CleanupServices& services() { return services_; }
    CleanupGates& gates() const { return gates_; }

    // Uses `level`'s preset for AI cleanup (Home, the AI cleanup page and guided setup share
    // this). A saved custom prompt replaces every preset, so choosing one removes it, after
    // asking "Remove your custom prompt?" (default No). Returns whether the level was saved.
    bool chooseCleanupLevel(const QString& level, QWidget* origin = nullptr);
    // Turns AI cleanup on at its saved level, as the AI cleanup page's switch does (through that
    // switch when the page is built in origin's window).
    void turnOnCleanup(QWidget* origin = nullptr);
    // Shows or hides every "AI cleanup is off" notice for the saved settings.
    void refreshCleanupGates();
    // An "AI cleanup is off" notice whose button turns cleanup on (CleanupGates::gateTile).
    InfoTile* cleanupGateTile(QString description, std::function<bool()> needed = {},
                              QWidget* parent = nullptr);
    // AI cleanup, revealing its model tile, in origin's window or the app's.
    void focusCleanupModel(QWidget* origin = nullptr);

    // _on_cleanup_enabled_changed: saves the switch, refreshes what follows it, and says
    // cleanup changed when it saved.
    void setCleanupEnabled(bool enabled, QWidget* origin);
    // _update_cleanup_prompt_ui for every registered page: the level, prompt and rule tiles
    // follow the switch, the gates show, Apps & styles re-reads (it locks while cleanup is off)
    // in origin's window and the windows the pages sit in.
    void updateCleanupUi(QWidget* origin = nullptr);
    // "Remove your custom prompt?" naming `level`'s preset; true for Yes.
    bool confirmCustomPromptRemoval(const QString& level, QWidget* parent);
    // _forget_custom_prompt on every AI cleanup page.
    void forgetCustomPrompt();

    // Pages register when built.
    void addPage(CleanupPage* page);
    void addRulesPage(RulesPage* page);
    // Forwards a Settings window's "cleanup_profiles" change and capture pauses as this
    // object's signals; a window is attached once.
    void attach(SettingsDialog& dialog);
    // The rule count the Learned rules rail shows: the list of the newest built page while one
    // is alive (it may hold rules not yet re-read), else the saved rules.
    std::size_t ruleCount(const Json& settings) const;

    // The confirmation's seam; QMessageBox::question by default.
    Question question;

Q_SIGNALS:
    // AI cleanup was turned on or off, or its level changed (on_cleanup_changed); the Settings
    // window it changed in also says "cleanup" changed.
    void cleanupChanged();
    // A profile was saved or deleted (on_cleanup_profiles_changed).
    void cleanupProfilesChanged();
    // A page captures a shortcut (true) or stopped (on_profile_hotkey_capture).
    void profileHotkeyCapture(bool capturing);

private:
    SettingsDialog* windowFor(QWidget* origin) const;
    bool persist(SettingsDialog* dialog, const Json& updates, std::vector<std::string> drops);
    Json snapshot(SettingsDialog* dialog) const;
    // A save went through: the window says "cleanup" changed, then cleanupChanged().
    void announce(SettingsDialog* dialog);

    AppContext& context_;
    CleanupGates& gates_;
    CleanupServices services_;
    std::vector<QPointer<CleanupPage>> pages_;
    std::vector<QPointer<RulesPage>> rulesPages_;
    std::vector<QPointer<SettingsDialog>> attached_;
};

} // namespace fv
