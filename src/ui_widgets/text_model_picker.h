#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <QIcon>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "llm/profiles.h"

class QLabel;

namespace fv {

class Button;
class ElidingComboBox;
class SearchableComboBox;

// What the endpoint combo says about one endpoint, as its tooltip's second half and the sentence
// the Settings pages quote: the built-in table's line (any other endpoint speaks the OpenAI Chat
// Completions API), plus "Running on this computer." when a server of its kind answers at its own
// address, or "Answering at <url> — choose Edit to use it." for a built-in one whose kind answers
// at another address. `runningServers` is {base_url: server} as server detection found it, in
// the order it found them; the first match names the other address. A missing profile is
// "Unknown endpoint.".
QString textEndpointDescription(const std::optional<llm::TextLLMProfile>& profile,
                                const std::vector<std::pair<QString, QString>>& runningServers);

// The single-column endpoint and model picker of the Settings language-model sections (object
// name textModelPicker).
//
// Choosing a model assigns it: there is no confirm step, and an "Active" badge inside the model
// combo marks the saved assignment, so the value itself is the only report of what is in use. A
// search fragment left in the editor is not a choice: it reverts when focus leaves without Enter
// or a pick from the list, and it never follows the user to another endpoint (every endpoint
// keeps its own staged model). The last endpoint entry, "Custom…", is an action rather than an
// endpoint: picking it emits addEndpointRequested and puts the selection back, so backing out
// of the add dialog changes nothing.
//
// The widget holds no credentials and reads no settings: the host hands it the profiles, the
// catalog, the assignment in use, the servers found running and the credential line of the shown
// endpoint (which needs the credential store, so it is computed off the GUI thread). Affinity:
// GUI thread.
class TextModelPicker : public QWidget {
    Q_OBJECT
public:
    // Combo data of the trailing "add a custom endpoint" action; it never names a profile.
    static constexpr const char* kCustomOption = "__custom__";
    static constexpr const char* kInstalledAgentOption = "__installed_agent__";
    static constexpr const char* kDefaultIdleStatus =
        "Open this destination to load the model catalog.";
    // The catalog orders: (label, saved value), in the sort combo's order.
    static const std::vector<std::pair<QString, QString>>& sortOptions();

    explicit TextModelPicker(QString idleStatus = QString::fromUtf8(kDefaultIdleStatus),
                             QWidget* parent = nullptr);

    // The selectable endpoints, built-in first; an empty list means the built-in ones. The shown
    // endpoint stays when it is still listed, else the first one is shown. The catalog is kept.
    void setProfiles(std::vector<llm::TextLLMProfile> profiles);
    // Cleanup can add a runner action without treating it as an endpoint selection.
    void enableInstalledAgentAction();
    // The id of the shown endpoint.
    const QString& provider() const { return provider_; }
    // The shown endpoint's profile; nullopt only when its id names no profile.
    std::optional<llm::TextLLMProfile> currentProfile() const;
    // Shows one endpoint and, with a model, stages that model for it. An unknown id is ignored.
    // Emits nothing.
    void setProvider(const QString& provider, std::optional<QString> model = std::nullopt);

    // The catalog choices of the shown endpoint, keeping its staged model in the editor. A model
    // that model_spec refuses is listed disabled with the reason as its tooltip.
    void setModels(const QStringList& models);
    // Disables Refresh while a catalog loads and says so in the status line.
    void setLoading(bool loading);
    // The status line under the model row.
    void setStatus(const QString& text);
    // Records the saved assignment the "Active" badge reports, and stages its model.
    void setActiveSelection(const QString& provider, const QString& model);

    // The order of the OpenRouter catalog; alphabetical for any other endpoint.
    QString currentSort() const;
    // Selects a saved order without emitting sortChanged.
    void setSort(const QString& sort);

    // Marks the local servers found answering, {base_url: server}: their endpoints read
    // "<name> · Running" and the descriptions say where they answer. The overload keeps the order
    // the servers were found in, which decides the address a description names when several of
    // one kind answer; the map's own order is by address.
    void setRunningServers(const std::map<QString, QString>& servers);
    void setRunningServers(std::vector<std::pair<QString, QString>> servers);

    // The shown endpoint's credential line: `text` as credential_status words it, `available`
    // whether the key (or no key at all) is found, `url` the base URL of the endpoint it was
    // computed for ("" for an endpoint with none). The picker adds the "Key needed in Settings →
    // App → API keys" wording when a cloud endpoint lacks its key. An answer for another address
    // than the shown endpoint's is stale (the user moved on while the store was read) and is
    // dropped, "" included when the shown endpoint has an address; so is the answer held for the
    // previous endpoint when another is shown. An endpoint
    // that needs no key shows "No API key required" at once; a keyed one shows nothing until the
    // host answers.
    void setCredentialStatus(const QString& text, bool available, const QString& url);
    // The same answer computed for the endpoint `provider`: it counts only while that endpoint is
    // shown, so of two endpoints at one address (different key variables) neither takes the
    // other's late answer, which the address alone cannot tell apart.
    void setCredentialStatus(const QString& text, bool available, const QString& url,
                             const QString& provider);

    // The model each endpoint shows. Only a committed pick or the host changes it, so switching
    // endpoints never carries a typed fragment along.
    const std::map<QString, QString>& stagedModels() const { return stagedModels_; }
    void setStagedModel(const QString& provider, const QString& model);

    ElidingComboBox* providerCombo() const { return providerCombo_; }
    SearchableComboBox* modelCombo() const { return modelCombo_; }
    ElidingComboBox* sortCombo() const { return sortCombo_; }
    QLabel* sortLabel() const { return sortLabel_; }
    QLabel* statusLabel() const { return statusLabel_; }
    QLabel* credentialIcon() const { return credentialIcon_; }
    QLabel* requirementLabel() const { return requirementLabel_; }
    QLabel* urlLabel() const { return urlLabel_; }
    Button* editButton() const { return editButton_; }
    Button* deleteButton() const { return deleteButton_; }
    Button* refreshButton() const { return refreshButton_; }

Q_SIGNALS:
    // The user chose another endpoint.
    void providerChanged(const QString& provider);
    void refreshRequested(const QString& provider);
    // A model was picked or typed and confirmed, and it is not the one in use: assign it.
    void activationRequested(const QString& provider);
    void sortChanged(const QString& provider);
    void addEndpointRequested();
    void installedAgentRequested();
    void editEndpointRequested(const QString& provider);
    void deleteEndpointRequested(const QString& provider);

private:
    void reloadProviderCombo();
    void onProviderComboChanged();
    void renderProvider();
    void updateCredentialStatus();
    void restoreProviderSelection();
    bool isRunning(const llm::TextLLMProfile& profile) const;
    QString description(const std::optional<llm::TextLLMProfile>& profile) const;
    // The model the shown endpoint holds: staged, else the endpoint's default.
    QString stagedModel() const;
    QString defaultModel(const QString& provider) const;
    void onModelActivated();
    void onModelEditingFinished();
    void commitModelSelection();
    void restoreStagedModel();
    QString activeProfileName() const;
    void updateActiveBadge();
    void emitEdit();
    void emitDelete();
    bool canEdit(const llm::TextLLMProfile& profile) const;
    // The refusal model_spec gives for model on the shown endpoint, if any.
    std::optional<QString> modelRefusal(const QString& model) const;

    // The host's answer, with the endpoint (id and address) it was given for.
    struct CredentialLine {
        QString text;
        bool available = false;
        QString url;
        QString provider;
    };

    QString provider_;
    QString activeProvider_;
    QString activeModel_;
    QString idleStatus_;
    std::vector<llm::TextLLMProfile> profiles_;
    std::vector<std::pair<QString, QString>> runningServers_;
    std::map<QString, QString> stagedModels_;
    // What the host last said about the shown endpoint's key; empty until it answers.
    std::optional<CredentialLine> credential_;
    bool modelEdited_ = false;
    bool installedAgentAction_ = false;
    QIcon modelIcon_;

    ElidingComboBox* providerCombo_;
    QLabel* credentialIcon_;
    QLabel* requirementLabel_;
    QLabel* urlLabel_;
    Button* editButton_;
    Button* deleteButton_;
    QLabel* sortLabel_;
    ElidingComboBox* sortCombo_;
    SearchableComboBox* modelCombo_;
    Button* refreshButton_;
    QLabel* statusLabel_;
};

} // namespace fv
