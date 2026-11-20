#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <QString>
#include <QStringList>

#include "appkit/qt_bridge.h"
#include "base/json.h"
#include "llm/profiles.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/text_endpoint_dialog.h"

class QComboBox;

namespace fv {

class AgentPicker;
class Button;
class ElidingComboBox;
class SettingsStore;
class TextModelPicker;
class WrappedLabel;

// The Cleanup model card of the AI cleanup page (settings_models.py build_cleanup_model_section
// and the ModelAssignments handlers behind it): the endpoint and model picker (TextModelPicker,
// "text_model_picker"), the Thinking level ("cleanup_reasoning_combo") and its note.
//
// Choosing a model assigns it at once (provider, model, the per-endpoint model memory and the
// endpoint provider), straight to the settings file as OpenWhisper's model handlers write; the
// thinking level and the catalog order are saved the same way, never while the card is filled.
// The catalog of the shown endpoint loads off the GUI thread when the page is shown and when
// another endpoint is chosen there; results are cached per (endpoint, order) and apply only while
// the endpoint and order still match, and a failed load keeps the cached list. Local servers are
// probed once per Settings window, and the shown endpoint's key status is read off the GUI
// thread. Custom endpoints are added, edited and deleted here (an endpoint in use cannot be
// deleted); a local server preset edits only its address and key variable.
//
// "Who runs AI cleanup" (AgentPicker, purpose "AI cleanup", runnerControls()) goes above the card
// and shows once "Installed agent…" is chosen in the provider list or while an agent is the saved
// provider: the agent tiles with what the scan found, and the chosen agent's model catalog in this
// card. Unlike OpenWhisper, where a tile click saves, a click only stages the tile; "Apply runner"
// commits it, and an agent is saved only after the consent question that names it
// (difference a04-agent-cleanup-scope). Applying "API / local model" clears the approval. A model
// picked for the approved agent saves at once; one picked for a staged agent is kept for Apply.
// The previous endpoint and each runner's model are remembered when switching.
// Affinity: GUI thread.
class CleanupModelSection : public InfoTile {
    Q_OBJECT
public:
    // _CLEANUP_ENDPOINT_DESCRIPTION.
    static const QString kDescription;

    // What the card asks of the page around it.
    struct Hooks {
        // The settings snapshot the window reads.
        std::function<Json()> settings;
        // The window's message line.
        std::function<void(const QString&)> say;
        // Redraws the window's rail.
        std::function<void()> refreshRail;
        // Whether AI cleanup is the page shown.
        std::function<bool()> isCurrent;
    };

    CleanupModelSection(SettingsStore& store, CleanupServices& services, Hooks hooks,
                        QWidget* parent = nullptr);
    ~CleanupModelSection() override;

    TextModelPicker* picker() const { return picker_; }
    // "Who runs AI cleanup" with its "Apply runner" button: the page places it above the card.
    QWidget* runnerControls() const { return runnerControls_; }
    AgentPicker* agentPicker() const { return agents_; }
    Button* applyRunnerButton() const { return applyRunner_; }
    // The tile staged in the agent picker: an agent id, or legacy_builtin_agent_id.
    const QString& stagedRunner() const { return staged_; }
    ElidingComboBox* reasoningCombo() const { return reasoningCombo_; }
    WrappedLabel* reasoningNote() const { return reasoningNote_; }

    // "<endpoint name> · <model or 'default model'>" (text_summary), for a provider id and model.
    static QString textSummary(const std::string& provider, const std::string& model,
                               const Json& settings);
    // The assignment in use, as text_summary.
    QString textSummary() const;
    const std::string& activeProvider() const { return activeProvider_; }
    const std::string& activeModel() const { return activeModel_; }

    // _load_text_settings: shows the saved assignment, order and model memory.
    void loadTextSettings(const Json& settings);
    // on_destination_shown(CLEANUP): probes local servers (once) and loads the shown endpoint's
    // catalog.
    void onDestinationShown();

    // The handlers, public for the page and tests.
    void activateTextModel(const QString& provider);
    void addTextEndpoint();
    void editTextEndpoint(const QString& profileId);
    void deleteTextEndpoint(const QString& profileId);
    void onTextProviderChanged(const QString& provider);
    void onTextSortChanged(const QString& provider);
    void fetchCatalogModels(const QString& provider, bool force = false);
    void invalidateTextCatalog(const QString& provider);
    // A catalog result; `token` is the request's (nullopt applies it whatever was asked).
    void onTextModelsLoaded(const QString& provider, const QString& sort, const QStringList& models,
                            const QString& error,
                            std::optional<std::uint64_t> token = std::nullopt);
    void detectLocalServers(bool force = false);
    void updateReasoningControls(bool refill = true);
    // Reads the shown endpoint's key status off the GUI thread.
    void updateCredentialStatus();

    // Whether a catalog of (provider, sort) is cached, and the token of its request in flight.
    bool hasCachedCatalog(const QString& provider, const QString& sort) const;
    std::optional<std::uint64_t> catalogToken(const QString& provider, const QString& sort) const;

    // The endpoint dialog's seam: the saved fields, or nullopt when cancelled.
    std::function<std::optional<TextEndpointDialog::Payload>(
        std::optional<llm::TextLLMProfile> profile, QWidget* parent)>
        editEndpoint;
    // The confirmation seam (QMessageBox::question's shape).
    CleanupSettings::Question question;

private:
    using CatalogKey = std::pair<QString, QString>;

    void refreshPickerProfiles(const Json& settings);
    void showCleanupRunner(bool reveal = false);
    // The tile the saved settings show: the saved agent, else the built-in one.
    QString savedRunner() const;
    // The tile whose choice is in effect: the approved agent, the built-in one while an endpoint
    // runs, or "" for a saved agent that is not approved.
    QString appliedRunner() const;
    void onAgentChoice(const QString& id);
    void onAgentModelChosen(const QString& agent, const QString& model);
    void applyCleanupRunner();
    void saveAgentModel(const QString& model);
    void refreshCodexLevels();
    bool validateTextModel(const QString& provider, const QString& model);
    void applyCatalogToPicker(const QString& provider, const QString& sort,
                              const QStringList& models, const QString& error);
    void onReasoningChanged();
    Json snapshot() const;
    void say(const QString& text) const;

    SettingsStore& store_;
    CleanupServices& services_;
    Hooks hooks_;
    TextModelPicker* picker_ = nullptr;
    QWidget* runnerControls_ = nullptr;
    AgentPicker* agents_ = nullptr;
    Button* applyRunner_ = nullptr;
    // The staged tile, and models picked for agents not yet applied.
    QString staged_;
    std::map<QString, QString> stagedModels_;
    // "Installed agent…" was chosen; cleared when an endpoint is applied.
    bool revealed_ = false;
    ElidingComboBox* reasoningCombo_ = nullptr;
    WrappedLabel* reasoningNote_ = nullptr;

    std::string activeProvider_;
    std::string activeModel_;
    std::map<CatalogKey, QStringList> cache_;
    std::map<CatalogKey, std::uint64_t> catalogTokens_;
    std::set<CatalogKey> loading_;
    std::uint64_t nextCatalogToken_ = 0;
    qt::TokenGate serversGate_;
    bool serversProbed_ = false;
    qt::TokenGate credentialGate_;
    qt::TokenGate codexLevelsGate_;
    std::optional<std::string> loadingCodexModel_;
    std::optional<std::pair<std::string, std::vector<std::string>>> codexLevels_;
};

} // namespace fv
