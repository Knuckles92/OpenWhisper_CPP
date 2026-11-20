#include "ui_personalize/cleanup_model_section.h"
#include "ui_personalize/cleanup_agent_offer.h"

#include <algorithm>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <utility>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPointer>
#include <QSignalBlocker>

#include "agents/agent_actions.h"
#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "base/executor.h"
#include "base/identity.h"
#include "base/log.h"
#include "llm/cleanup_settings.h"
#include "llm/model_catalog.h"
#include "llm/thinking_levels.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_config.h"
#include "ui_settings/settings_fields.h"
#include "ui_theme/icons.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/thinking_combo.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_models"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

QString thinkingName(std::string_view level) {
    return q(llm::thinking_label(level).value_or(llm::thinking_label("off").value_or("Off")));
}

QString builtinRunner() {
    return q(legacy_builtin_agent_id);
}

QString agentName(std::string_view id) {
    return q(agents::agent_spec(id).name);
}

bool agentApproved(const Json& settings, std::string_view provider) {
    return llm::is_installed_cleanup_agent(provider) &&
           keys::fv_cleanup_agent_consent.read(settings) == provider &&
           llm::resolve_transcript_cleanup_provider(settings) == provider;
}

} // namespace

const QString CleanupModelSection::kDescription =
    QStringLiteral("The provider and model that rewrite each dictation. Cleanup profiles use it "
                   "too. The provider's key lives under API keys.");

CleanupModelSection::CleanupModelSection(SettingsStore& store, CleanupServices& services,
                                         Hooks hooks, QWidget* parent)
    : InfoTile(QStringLiteral("Cleanup model"), kDescription,
               ui::designIcon(QStringLiteral("cpu-blue.svg")), parent),
      store_(store), services_(services), hooks_(std::move(hooks)) {
    activeProvider_ = std::string(llm::kOpenAiProfileId);
    activeModel_ = llm::default_transcript_cleanup_model(activeProvider_, Json::object());

    staged_ = builtinRunner();
    // Above the card: who runs AI cleanup, and the step that commits a choice there.
    runnerControls_ = new QWidget(this);
    runnerControls_->setObjectName(QStringLiteral("cleanupRunnerControls"));
    auto* runnerLayout = new QVBoxLayout(runnerControls_);
    runnerLayout->setContentsMargins(0, 0, 0, 0);
    runnerLayout->setSpacing(8);
    agents_ = new AgentPicker(QStringLiteral("AI cleanup"), false);
    runnerLayout->addWidget(agents_);
    applyRunner_ = new Button(QStringLiteral("Apply runner"));
    applyRunner_->setObjectName(QStringLiteral("applyCleanupRunner"));
    auto* applyRow = new QHBoxLayout;
    applyRow->setContentsMargins(0, 0, 0, 0);
    applyRow->addWidget(applyRunner_);
    applyRow->addStretch();
    runnerLayout->addLayout(applyRow);
    runnerControls_->hide();
    connect(applyRunner_, &Button::clicked, this, &CleanupModelSection::applyCleanupRunner);
    connect(agents_, &AgentPicker::choiceRequested, this, &CleanupModelSection::onAgentChoice);
    connect(agents_, &AgentPicker::modelChosen, this, &CleanupModelSection::onAgentModelChosen);

    picker_ = new TextModelPicker;
    picker_->enableInstalledAgentAction();
    connect(picker_, &TextModelPicker::installedAgentRequested, this,
            [this] { showCleanupRunner(true); });
    connect(picker_, &TextModelPicker::addEndpointRequested, this,
            &CleanupModelSection::addTextEndpoint);
    connect(picker_, &TextModelPicker::editEndpointRequested, this,
            &CleanupModelSection::editTextEndpoint);
    connect(picker_, &TextModelPicker::deleteEndpointRequested, this,
            &CleanupModelSection::deleteTextEndpoint);
    connect(picker_, &TextModelPicker::providerChanged, this,
            &CleanupModelSection::onTextProviderChanged);
    connect(picker_, &TextModelPicker::refreshRequested, this,
            [this](const QString& provider) { fetchCatalogModels(provider, true); });
    connect(picker_, &TextModelPicker::activationRequested, this,
            &CleanupModelSection::activateTextModel);
    connect(picker_, &TextModelPicker::sortChanged, this, &CleanupModelSection::onTextSortChanged);

    reasoningCombo_ = new ElidingComboBox;
    reasoningCombo_->setMinimumHeight(40);
    // Filled per runner and model by updateReasoningControls.
    connect(reasoningCombo_, &QComboBox::currentIndexChanged, this,
            [this] { onReasoningChanged(); });

    // One card for whichever runner is chosen above: the agent's model or the endpoint's, then
    // the thinking level, which agents and API models both take.
    addBody(agents_->modelCard());
    addBody(picker_);
    addBody(settingsField(QStringLiteral("Thinking level"), reasoningCombo_));
    reasoningNote_ = settingsCaption(QString());
    addBody(reasoningNote_);

    editEndpoint = [](std::optional<llm::TextLLMProfile> profile,
                      QWidget* dialogParent) -> std::optional<TextEndpointDialog::Payload> {
        TextEndpointDialog dialog(std::move(profile), dialogParent);
        if (dialog.exec() != QDialog::Accepted) {
            return std::nullopt;
        }
        return dialog.resultPayload();
    };
    question = [](QWidget* dialogParent, const QString& title, const QString& text,
                  QMessageBox::StandardButtons buttons, QMessageBox::StandardButton fallback) {
        return QMessageBox::question(dialogParent, title, text, buttons, fallback);
    };
}

CleanupModelSection::~CleanupModelSection() = default;

Json CleanupModelSection::snapshot() const {
    Json settings = hooks_.settings ? hooks_.settings() : *store_.snapshot();
    return settings.is_object() ? settings : Json::object();
}

void CleanupModelSection::say(const QString& text) const {
    if (hooks_.say) {
        hooks_.say(text);
    }
}

QString CleanupModelSection::textSummary(const std::string& provider, const std::string& model,
                                         const Json& settings) {
    const std::string name = llm::profile_display_name(provider, settings);
    return q(name) + QStringLiteral(u" · ") +
           (model.empty() ? QStringLiteral("default model") : q(model));
}

QString CleanupModelSection::textSummary() const {
    return textSummary(activeProvider_, activeModel_, snapshot());
}

// --- Loading ---

void CleanupModelSection::refreshPickerProfiles(const Json& settings) {
    const std::vector<llm::TextLLMProfile> profiles = llm::list_profiles(settings);
    const Json memory = keys::cleanup_model_memory.read(settings);
    if (memory.is_object()) {
        for (const auto& [provider, model] : memory.items()) {
            if (model.is_string()) {
                picker_->setStagedModel(q(provider), q(model.get<std::string>()));
            }
        }
    }
    picker_->setProfiles(profiles);
}

void CleanupModelSection::loadTextSettings(const Json& settings) {
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    const std::string model = llm::resolve_transcript_cleanup_model(settings);
    const std::string sort = llm::resolve_transcript_cleanup_model_sort(*store_.snapshot());
    activeProvider_ = provider;
    activeModel_ = model;
    // The address line is elided in the font the theme gives it, so the picker is styled first.
    picker_->ensurePolished();
    picker_->setSort(q(sort));
    refreshPickerProfiles(settings);
    const bool installed = llm::is_installed_cleanup_agent(provider);
    std::string endpoint = keys::cleanup_endpoint_provider.read(settings);
    if (!llm::get_profile(endpoint, settings)) {
        endpoint = std::string(text::config::TRANSCRIPT_CLEANUP_PROVIDER);
    }
    picker_->setProvider(q(installed ? endpoint : provider),
                         installed ? std::optional<QString>() : std::optional<QString>(q(model)));
    picker_->setActiveSelection(q(provider), q(model));
    std::map<QString, QString> models;
    if (const Json memory = keys::cleanup_model_memory.read(settings); memory.is_object()) {
        for (const auto& [id, remembered] : memory.items()) {
            if (remembered.is_string()) {
                models[q(id)] = q(remembered.get<std::string>());
            }
        }
    }
    if (installed) {
        // The saved model is what runs, whatever the memory last held.
        models[q(provider)] = q(model);
    }
    stagedModels_.clear();
    staged_ = savedRunner();
    agents_->setSavedModels(models);
    agents_->setChoice(staged_);
    showCleanupRunner();
    updateCredentialStatus();
}

QString CleanupModelSection::savedRunner() const {
    return llm::is_installed_cleanup_agent(activeProvider_) ? q(activeProvider_) : builtinRunner();
}

QString CleanupModelSection::appliedRunner() const {
    if (agentApproved(*store_.snapshot(), activeProvider_)) {
        return q(activeProvider_);
    }
    return llm::is_installed_cleanup_agent(activeProvider_) ? QString() : builtinRunner();
}

void CleanupModelSection::showCleanupRunner(bool reveal) {
    if (reveal) {
        revealed_ = true;
    }
    const Json settings = *store_.snapshot();
    const bool installed = llm::is_installed_cleanup_agent(activeProvider_);
    const bool approved = agentApproved(settings, activeProvider_);
    const bool stagedAgent = staged_ != builtinRunner();
    const bool stagedActive = staged_ == q(activeProvider_);
    // The endpoint picker shows unless an agent runs or another agent is staged; a saved agent
    // that is not approved runs nothing, so the endpoint stays in view beside it.
    picker_->setVisible(!stagedAgent || (!approved && stagedActive));
    runnerControls_->setVisible(installed || revealed_ || stagedAgent);
    applyRunner_->setEnabled(staged_ != appliedRunner());
    if (stagedAgent && !(approved && stagedActive)) {
        setDescription(QStringLiteral("%1 needs your approval before it can receive dictation. "
                                      "Apply the runner to review what will be shared.")
                           .arg(agentName(u(staged_))));
    } else if (approved && stagedActive) {
        // _show_cleanup_runner.
        setDescription(QStringLiteral("%1 rewrites each dictation. Cleanup profiles use it too.")
                           .arg(agentName(activeProvider_)));
    } else {
        setDescription(kDescription);
    }
    updateReasoningControls();
    refreshCodexLevels();
    if (reveal) {
        // Discovery only: the scan asks each CLI for its version and sign-in, never sends text.
        agents_->ensureScanned();
        agents_->setFocus(Qt::OtherFocusReason);
    }
}

void CleanupModelSection::onAgentChoice(const QString& id) {
    // _on_cleanup_agent_choice saves here; FioriVoce stages the tile until "Apply runner".
    agents_->setChoice(id);
    staged_ = agents_->choice();
    showCleanupRunner();
}

void CleanupModelSection::onAgentModelChosen(const QString& agent, const QString& model) {
    if (agent == q(activeProvider_) && agentApproved(*store_.snapshot(), activeProvider_)) {
        // _on_cleanup_agent_model_chosen.
        saveAgentModel(model);
        return;
    }
    // An agent not yet applied: its model is saved with it.
    stagedModels_[agent] = model;
}

void CleanupModelSection::applyCleanupRunner() {
    if (staged_ == builtinRunner()) {
        activateTextModel(picker_->provider());
        showCleanupRunner();
        if (!llm::is_installed_cleanup_agent(activeProvider_)) {
            onDestinationShown();
        }
        return;
    }
    const std::string provider = u(staged_);
    if (!llm::is_installed_cleanup_agent(provider)) {
        return;
    }
    const Json before = *store_.snapshot();
    if (agentApproved(before, provider)) {
        return;
    }
    const auto picked = stagedModels_.find(staged_);
    const std::optional<std::string> stagedModel =
        picked == stagedModels_.end() ? std::nullopt
                                      : std::optional<std::string>(u(picked->second));
    const QString name = q(llm::profile_display_name(provider, before));
    const QPointer<CleanupModelSection> self(this);
    const auto consent = agentConsentQuestion(name);
    const auto confirmed = question ? question(window(), consent.title, consent.text,
                                               QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                                    : QMessageBox::No;
    if (self == nullptr) {
        return;
    }
    if (confirmed != QMessageBox::Yes) {
        loadTextSettings(*store_.snapshot());
        showCleanupRunner(true);
        return;
    }
    try {
        const Json saved = store_.mutate([&](Json& settings) {
            if (llm::resolve_transcript_cleanup_provider(settings) !=
                    llm::resolve_transcript_cleanup_provider(before) ||
                keys::fv_cleanup_agent_consent.read(settings) !=
                    keys::fv_cleanup_agent_consent.read(before)) {
                throw std::runtime_error("The cleanup runner changed. Apply your choice again.");
            }
            const std::string previous = llm::resolve_transcript_cleanup_provider(settings);
            Json memory = keys::cleanup_model_memory.read(settings);
            if (!llm::is_installed_cleanup_agent(previous)) {
                settings[keys::cleanup_endpoint_provider.key()] = previous;
            }
            memory[previous] = llm::resolve_transcript_cleanup_model(settings);
            const auto remembered = memory.find(provider);
            const std::string model = stagedModel ? *stagedModel
                                      : remembered != memory.end() && remembered->is_string()
                                          ? remembered->get<std::string>()
                                          : std::string();
            memory[provider] = model;
            settings[keys::cleanup_model_memory.key()] = std::move(memory);
            settings[keys::transcript_cleanup_provider.key()] = provider;
            settings[keys::transcript_cleanup_model.key()] = model;
            settings[keys::fv_cleanup_agent_consent.key()] = provider;
        });
        loadTextSettings(saved);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't activate cleanup agent");
        say(QStringLiteral("Couldn't set cleanup runner: %1").arg(q(error.what())));
        loadTextSettings(*store_.snapshot());
        return;
    }
    say(QStringLiteral("AI cleanup will run through %1").arg(textSummary()));
    if (hooks_.refreshRail) {
        hooks_.refreshRail();
    }
}

void CleanupModelSection::saveAgentModel(const QString& model) {
    const std::string chosen = u(model.trimmed());
    if (!chosen.empty() && !agents::valid_agent_model_id(chosen)) {
        say(QStringLiteral("Enter a valid agent model ID or use the default."));
        loadTextSettings(*store_.snapshot());
        return;
    }
    const std::string provider = activeProvider_;
    try {
        const Json saved = store_.mutate([&](Json& settings) {
            if (!agentApproved(settings, provider) ||
                llm::resolve_transcript_cleanup_model(settings) != activeModel_) {
                throw std::runtime_error(
                    "The cleanup model or its approval changed. Review the current runner.");
            }
            Json memory = keys::cleanup_model_memory.read(settings);
            memory[provider] = chosen;
            settings[keys::cleanup_model_memory.key()] = std::move(memory);
            settings[keys::transcript_cleanup_model.key()] = chosen;
        });
        loadTextSettings(saved);
    } catch (const std::exception& error) {
        say(QStringLiteral("Couldn't set agent model: %1").arg(q(error.what())));
        loadTextSettings(*store_.snapshot());
        return;
    }
    say(QStringLiteral("AI cleanup will run through %1").arg(textSummary()));
    if (hooks_.refreshRail) {
        hooks_.refreshRail();
    }
}

void CleanupModelSection::refreshCodexLevels() {
    if (activeProvider_ != "codex" || !agentApproved(*store_.snapshot(), activeProvider_)) {
        if (loadingCodexModel_) {
            (void)codexLevelsGate_.next();
            loadingCodexModel_.reset();
        }
        return;
    }
    if (loadingCodexModel_ == activeModel_ || !services_.io || !services_.ui ||
        !services_.codexAgentLevels) {
        return;
    }
    loadingCodexModel_ = activeModel_;
    const std::uint64_t token = codexLevelsGate_.next();
    QPointer<CleanupModelSection> self(this);
    services_.io->post([self, ui = services_.ui, gate = codexLevelsGate_, token,
                        model = activeModel_, read = services_.codexAgentLevels] {
        std::optional<std::vector<std::string>> levels;
        try {
            levels = read(model);
        } catch (const std::exception& failure) {
            kLog.debug("Couldn't read Codex efforts ({})", type_display_name(typeid(failure)));
        }
        ui->post([self, gate, token, model, levels = std::move(levels)]() mutable {
            if (self == nullptr || !gate.isCurrent(token)) {
                return;
            }
            self->loadingCodexModel_.reset();
            if (levels) {
                self->codexLevels_ = std::pair(model, std::move(*levels));
            }
            self->updateReasoningControls();
        });
    });
}

void CleanupModelSection::updateReasoningControls(bool refill) {
    const Json settings = snapshot();
    // The picker may show a choice that isn't saved yet.
    const std::optional<llm::TextLLMProfile> profile =
        llm::get_profile(u(picker_->provider()), settings);
    const bool agent = agentApproved(settings, activeProvider_);
    std::vector<std::string> offered;
    if (agent && activeProvider_ == "codex") {
        offered = codexLevels_ && codexLevels_->first == activeModel_
                      ? codexLevels_->second
                      : std::vector<std::string>{"low", "medium", "high"};
    } else if (agent) {
        offered = llm::agent_levels(activeProvider_, activeModel_);
    } else {
        offered = llm::endpoint_levels(profile ? &*profile : nullptr,
                                       u(picker_->modelCombo()->currentText().trimmed()));
    }
    QString note = !offered.empty()
                       ? QStringLiteral("How much the model reasons before it rewrites. Higher is "
                                        "slower.")
                       : QStringLiteral("This model doesn't take a thinking level, so it uses its "
                                        "provider's default.");
    if (agent) {
        const agents::AgentSpec& spec = agents::agent_spec(activeProvider_);
        note = spec.transport == "acp"
                   ? QStringLiteral("Sent to %1 as its reasoning effort when the model offers "
                                    "that level.")
                         .arg(q(spec.name))
                   : QStringLiteral("Sent to %1 as its reasoning effort.").arg(q(spec.name));
    }
    const std::string saved = llm::resolve_transcript_cleanup_reasoning(settings);
    std::optional<std::string> shown;
    if (refill) {
        shown = fillThinkingCombo(reasoningCombo_, offered, saved);
    } else {
        const QVariant chosen = reasoningCombo_->currentData();
        if (chosen.isValid() && !chosen.isNull()) {
            shown = u(chosen.toString());
        }
    }
    if (!offered.empty() && shown.value_or(std::string()) != saved) {
        note += QStringLiteral(" Your %1 setting runs at %2 here.")
                    .arg(thinkingName(saved), thinkingName(shown.value_or(std::string())));
    }
    reasoningCombo_->setToolTip(note);
    reasoningNote_->setText(note);
}

void CleanupModelSection::onReasoningChanged() {
    const QVariant chosen = reasoningCombo_->currentData();
    const Json value =
        chosen.isValid() && !chosen.isNull() ? Json(u(chosen.toString())) : Json(nullptr);
    try {
        store_.save_setting(keys::transcript_cleanup_reasoning.key(), value);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't save cleanup thinking level");
        say(QStringLiteral("Couldn't save thinking level: %1").arg(q(error.what())));
        return;
    }
    updateReasoningControls(false);
}

// --- Assignment ---

bool CleanupModelSection::validateTextModel(const QString& provider, const QString& model) {
    if (model.isEmpty()) {
        return false;
    }
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(u(provider), snapshot());
    if (!profile) {
        return false;
    }
    try {
        (void)llm::model_spec(*profile, u(model));
        return true;
    } catch (const std::invalid_argument& error) {
        say(q(error.what()));
    } catch (const py::ValueError& error) {
        say(q(error.what()));
    }
    return false;
}

void CleanupModelSection::activateTextModel(const QString& provider) {
    if (provider != picker_->provider()) {
        return;
    }
    const QString model = picker_->modelCombo()->currentText().trimmed();
    if (!validateTextModel(provider, model)) {
        return;
    }
    const std::string id = u(provider);
    const std::string chosen = u(model);
    if (id == activeProvider_ && chosen == activeModel_ &&
        keys::fv_cleanup_agent_consent.read(*store_.snapshot()).empty()) {
        return;
    }
    try {
        Json memory = keys::cleanup_model_memory.read(snapshot());
        if (!memory.is_object()) {
            memory = Json::object();
        }
        memory[id] = chosen;
        Json patch = Json::object();
        patch[std::string(keys::transcript_cleanup_provider.key())] = id;
        patch[std::string(keys::transcript_cleanup_model.key())] = chosen;
        patch[std::string(keys::cleanup_model_memory.key())] = memory;
        patch[std::string(keys::cleanup_endpoint_provider.key())] = id;
        patch[std::string(keys::fv_cleanup_agent_consent.key())] = "";
        store_.update(patch);
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't activate text model");
        say(QStringLiteral("Couldn't set text model: %1").arg(q(error.what())));
        return;
    }
    activeProvider_ = id;
    activeModel_ = chosen;
    picker_->setActiveSelection(provider, model);
    // An endpoint runs now: the agent tiles go back behind "Installed agent…".
    revealed_ = false;
    staged_ = builtinRunner();
    agents_->setChoice(staged_);
    showCleanupRunner();
    say(QStringLiteral("Text model set to %1").arg(textSummary()));
    if (hooks_.refreshRail) {
        hooks_.refreshRail();
    }
}

// --- Endpoints ---

void CleanupModelSection::addTextEndpoint() {
    const std::optional<TextEndpointDialog::Payload> payload =
        editEndpoint ? editEndpoint(std::nullopt, window()) : std::nullopt;
    if (!payload) {
        return;
    }
    llm::TextLLMProfile profile;
    try {
        store_.mutate([&](Json& settings) {
            profile = llm::upsert_custom_profile(
                settings, payload->name, payload->baseUrl, payload->apiKeyEnv, std::nullopt,
                payload->protocol.empty() ? std::string_view("chat")
                                          : std::string_view(payload->protocol));
        });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't add text endpoint");
        say(QStringLiteral("Couldn't add endpoint: %1").arg(q(error.what())));
        return;
    }
    refreshPickerProfiles(snapshot());
    picker_->setProvider(q(profile.id));
    fetchCatalogModels(q(profile.id));
    say(QStringLiteral("Added endpoint \"%1\"").arg(q(profile.name)));
}

void CleanupModelSection::editTextEndpoint(const QString& profileId) {
    const std::string id = u(profileId);
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(id, snapshot());
    if (!profile || (profile->builtin && !llm::is_local_server_id(profile->id))) {
        return;
    }
    const std::optional<TextEndpointDialog::Payload> payload =
        editEndpoint ? editEndpoint(profile, window()) : std::nullopt;
    if (!payload) {
        return;
    }
    try {
        if (profile->builtin) {
            store_.mutate([&](Json& settings) {
                llm::save_local_server(settings, profile->id, payload->baseUrl, payload->apiKeyEnv);
            });
        } else {
            store_.mutate([&](Json& settings) {
                (void)llm::upsert_custom_profile(
                    settings, payload->name, payload->baseUrl, payload->apiKeyEnv, id,
                    payload->protocol.empty() ? std::string_view(profile->protocol)
                                              : std::string_view(payload->protocol));
            });
        }
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't edit text endpoint");
        say(QStringLiteral("Couldn't save endpoint: %1").arg(q(error.what())));
        return;
    }
    invalidateTextCatalog(profileId);
    refreshPickerProfiles(snapshot());
    if (profile->builtin) {
        detectLocalServers(true);
    }
    if (picker_->provider() == profileId) {
        fetchCatalogModels(profileId, true);
    }
    say(QStringLiteral("Updated \"%1\". Active meetings keep their original endpoint.")
            .arg(q(llm::profile_display_name(id, snapshot()))));
}

void CleanupModelSection::deleteTextEndpoint(const QString& profileId) {
    const std::string id = u(profileId);
    const Json settings = snapshot();
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(id, settings);
    if (!profile || profile->builtin) {
        return;
    }
    const std::string cleanupId = llm::resolve_transcript_cleanup_provider(settings);
    const std::string meetingId = llm::resolve_meeting_llm_provider(settings);
    if (id == cleanupId || id == meetingId) {
        say(QStringLiteral("\"%1\" is in use. Choose another text model before deleting this "
                           "endpoint.")
                .arg(q(profile->name)));
        return;
    }
    const QMessageBox::StandardButton confirmed =
        question ? question(window(), QStringLiteral("Delete endpoint"),
                            QStringLiteral("Delete \"%1\"?\n\nMeetings that already recorded this "
                                           "endpoint can still retry using the stored connection "
                                           "snapshot.")
                                .arg(q(profile->name)),
                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No)
                 : QMessageBox::No;
    if (confirmed != QMessageBox::Yes) {
        return;
    }
    try {
        store_.mutate([&](Json& stored) { (void)llm::remove_custom_profile(stored, id); });
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't delete text endpoint");
        say(QStringLiteral("Couldn't delete endpoint: %1").arg(q(error.what())));
        return;
    }
    refreshPickerProfiles(snapshot());
    say(QStringLiteral("Deleted endpoint \"%1\"").arg(q(profile->name)));
}

// --- Catalogs ---

void CleanupModelSection::onTextProviderChanged(const QString& provider) {
    updateReasoningControls();
    updateCredentialStatus();
    if (hooks_.isCurrent && hooks_.isCurrent()) {
        fetchCatalogModels(provider);
    }
}

void CleanupModelSection::onTextSortChanged(const QString& provider) {
    try {
        store_.save_setting(keys::transcript_cleanup_model_sort.key(),
                            Json(u(picker_->currentSort())));
    } catch (const std::exception& error) {
        kLog.warning("Couldn't save text model sort ({})", type_display_name(typeid(error)));
    }
    fetchCatalogModels(provider);
}

void CleanupModelSection::invalidateTextCatalog(const QString& provider) {
    std::erase_if(cache_, [&](const auto& entry) { return entry.first.first == provider; });
    for (auto it = catalogTokens_.begin(); it != catalogTokens_.end();) {
        if (it->first.first == provider) {
            loading_.erase(it->first);
            it = catalogTokens_.erase(it);
        } else {
            ++it;
        }
    }
}

bool CleanupModelSection::hasCachedCatalog(const QString& provider, const QString& sort) const {
    return cache_.contains({provider, sort});
}

std::optional<std::uint64_t> CleanupModelSection::catalogToken(const QString& provider,
                                                               const QString& sort) const {
    const auto found = catalogTokens_.find({provider, sort});
    return found == catalogTokens_.end() ? std::nullopt
                                         : std::optional<std::uint64_t>(found->second);
}

void CleanupModelSection::fetchCatalogModels(const QString& provider, bool force) {
    if (provider == picker_->provider()) {
        updateCredentialStatus();
    }
    const QString sort = picker_->currentSort();
    const CatalogKey key{provider, sort};
    if (!force) {
        if (const auto cached = cache_.find(key); cached != cache_.end()) {
            applyCatalogToPicker(provider, sort, cached->second, QString());
            return;
        }
    }
    if (loading_.contains(key)) {
        if (provider == picker_->provider()) {
            picker_->setLoading(true);
        }
        return;
    }
    if (services_.io == nullptr || services_.ui == nullptr || !services_.listModels) {
        // Nothing can load a catalog here (a context without the network services).
        return;
    }
    loading_.insert(key);
    const std::uint64_t token = ++nextCatalogToken_;
    catalogTokens_[key] = token;
    if (provider == picker_->provider()) {
        picker_->setLoading(true);
    }
    QPointer<CleanupModelSection> self(this);
    services_.io->post([self, ui = services_.ui, list = services_.listModels, provider, sort, token,
                        settings = snapshot()] {
        QStringList models;
        QString error;
        try {
            for (const std::string& model : list(u(provider), u(sort), settings)) {
                models << q(model);
            }
        } catch (const std::exception& failure) {
            kLog.warning("Couldn't list text models ({})", type_display_name(typeid(failure)));
            models.clear();
            error = q(failure.what());
        }
        ui->post([self, provider, sort, models, error, token] {
            if (self != nullptr) {
                self->onTextModelsLoaded(provider, sort, models, error, token);
            }
        });
    });
}

void CleanupModelSection::applyCatalogToPicker(const QString& provider, const QString& sort,
                                               const QStringList& models, const QString& error) {
    if (provider != picker_->provider()) {
        return;
    }
    const bool providerLoading =
        std::any_of(loading_.begin(), loading_.end(),
                    [&](const CatalogKey& key) { return key.first == provider; });
    picker_->setLoading(providerLoading);
    if (sort != picker_->currentSort()) {
        return;
    }
    if (!error.isEmpty()) {
        if (const auto cached = cache_.find({provider, sort}); cached != cache_.end()) {
            picker_->setModels(cached->second);
        }
        picker_->setStatus(QStringLiteral("Couldn't load models: %1").arg(error));
        return;
    }
    picker_->setModels(models);
    picker_->setStatus(QStringLiteral("%1 models available").arg(models.size()));
}

void CleanupModelSection::onTextModelsLoaded(const QString& provider, const QString& sort,
                                             const QStringList& models, const QString& error,
                                             std::optional<std::uint64_t> token) {
    const CatalogKey key{provider, sort};
    if (token) {
        const auto current = catalogTokens_.find(key);
        if (current == catalogTokens_.end() || current->second != *token) {
            return;
        }
    }
    loading_.erase(key);
    if (error.isEmpty()) {
        cache_[key] = models;
    }
    applyCatalogToPicker(provider, sort, models, error);
    if (picker_->provider() == provider) {
        // A server the user runs reports each model's reasoning control with its catalog.
        updateReasoningControls();
    }
}

// --- Local servers and keys ---

void CleanupModelSection::onDestinationShown() {
    if (!runnerControls_->isHidden()) {
        // on_destination_shown: the cached scan, or a new one (discovery only).
        agents_->ensureScanned();
    }
    if (agentApproved(*store_.snapshot(), activeProvider_)) {
        refreshCodexLevels();
        return;
    }
    detectLocalServers();
    fetchCatalogModels(picker_->provider());
}

void CleanupModelSection::detectLocalServers(bool force) {
    if (serversProbed_ && !force) {
        return;
    }
    if (services_.io == nullptr || services_.ui == nullptr || !services_.detectServers) {
        return;
    }
    serversProbed_ = true;
    const std::uint64_t token = serversGate_.next();
    std::vector<std::string> urls;
    for (const llm::TextLLMProfile& profile : llm::list_profiles(snapshot())) {
        if (llm::is_local_server_id(profile.id) && profile.base_url) {
            urls.push_back(*profile.base_url);
        }
    }
    for (const llm::LocalServer& server : llm::kLocalServers) {
        urls.emplace_back(server.default_url);
    }
    QPointer<CleanupModelSection> self(this);
    services_.io->post([self, ui = services_.ui, gate = serversGate_,
                        detect = services_.detectServers, urls = std::move(urls), token] {
        std::vector<std::pair<std::string, std::string>> found;
        try {
            found = detect(urls);
        } catch (const std::exception& failure) {
            kLog.debug("Local server detection failed ({})", type_display_name(typeid(failure)));
            found.clear();
        }
        ui->post([self, gate, token, found = std::move(found)] {
            if (self == nullptr || !gate.isCurrent(token)) {
                return;
            }
            std::vector<std::pair<QString, QString>> servers;
            for (const auto& [url, server] : found) {
                servers.emplace_back(q(url), q(server));
            }
            self->picker_->setRunningServers(std::move(servers));
        });
    });
}

void CleanupModelSection::updateCredentialStatus() {
    const std::optional<llm::TextLLMProfile> profile = picker_->currentProfile();
    if (!profile || !profile->requires_api_key()) {
        // The picker words an endpoint that needs no key itself.
        return;
    }
    if (services_.io == nullptr || services_.ui == nullptr || !services_.credentialStatus) {
        return;
    }
    const std::uint64_t token = credentialGate_.next();
    const QString url = q(profile->base_url.value_or(std::string()));
    QPointer<CleanupModelSection> self(this);
    services_.io->post([self, ui = services_.ui, gate = credentialGate_,
                        status = services_.credentialStatus, profile = *profile, url, token] {
        llm::KeyStatus answer;
        try {
            answer = status(profile);
        } catch (const std::exception& failure) {
            kLog.warning("Couldn't read the key status ({})", type_display_name(typeid(failure)));
            return;
        }
        ui->post([self, gate, token, answer, url, provider = q(profile.id)] {
            if (self != nullptr && gate.isCurrent(token)) {
                // The endpoint it was asked for, too: two endpoints can share an address.
                self->picker_->setCredentialStatus(q(answer.text), answer.available, url, provider);
            }
        });
    });
}

} // namespace fv
