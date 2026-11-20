#include "ui_widgets/text_model_picker.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPixmap>
#include <QPushButton>
#include <QSize>
#include <QStandardItem>
#include <QStandardItemModel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "llm/cleanup_settings.h"
#include "llm/model_catalog.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/searchable_combo.h"

namespace fv {
namespace {

// _DESCRIPTIONS: the built-in endpoints' lines, by profile id.
struct Description {
    std::string_view id;
    std::string_view text;
};
constexpr std::array<Description, 10> kDescriptions{{
    {"openai", "Direct access to OpenAI chat and reasoning models."},
    {"openrouter", "One catalog with models from OpenAI, Anthropic, Google, and more."},
    {"ollama", "Connect to your Ollama server. Install and manage models in Ollama."},
    {"lm_studio",
     "Connect to LM Studio's local server. Load a model and start the server in LM Studio."},
    {"vllm", "Connect to a vLLM server started with vllm serve."},
    {"llama_cpp", "Connect to llama.cpp's llama-server."},
    {"localai", "Connect to your LocalAI server."},
    {"groq", "Cloud text models served by Groq."},
    {"opencode_go", "OpenCode Go subscription endpoint."},
    {"opencode_zen", "OpenCode Zen usage-billed endpoint."},
}};

constexpr std::string_view kCustomEndpointDescription =
    "Any server that speaks the OpenAI Chat Completions API.";

// The URL label's width: a longer address elides in the middle.
constexpr int kUrlElideWidth = 220;

constexpr const char* kAlphabetical = "alphabetical";

QString q(std::string_view text) {
    return qt::toQString(text);
}

QString stripped(const QString& text) {
    return q(py::strip(text.toStdString()));
}

QString profileTooltip(const QString& name, const QString& description) {
    return name + QStringLiteral(u" — ") + description;
}

// A server map with Python dict semantics: a repeated address keeps its first place and takes
// the later server.
std::vector<std::pair<QString, QString>>
withoutRepeats(std::vector<std::pair<QString, QString>> servers) {
    std::vector<std::pair<QString, QString>> kept;
    for (auto& [url, server] : servers) {
        const auto found = std::find_if(kept.begin(), kept.end(),
                                        [&](const auto& entry) { return entry.first == url; });
        if (found != kept.end()) {
            found->second = std::move(server);
        } else {
            kept.emplace_back(std::move(url), std::move(server));
        }
    }
    return kept;
}

// The server answering at `url`, or nullptr.
const QString* serverAt(const std::vector<std::pair<QString, QString>>& servers,
                        const QString& url) {
    for (const auto& entry : servers) {
        if (entry.first == url) {
            return &entry.second;
        }
    }
    return nullptr;
}

bool isRunningAt(const llm::TextLLMProfile& profile,
                 const std::vector<std::pair<QString, QString>>& servers) {
    const QString* server = serverAt(servers, q(profile.base_url.value_or(std::string())));
    return server != nullptr && *server == q(profile.kind);
}

} // namespace

QString textEndpointDescription(const std::optional<llm::TextLLMProfile>& profile,
                                const std::vector<std::pair<QString, QString>>& runningServers) {
    if (!profile) {
        return QStringLiteral("Unknown endpoint.");
    }
    std::string_view line = kCustomEndpointDescription;
    for (const Description& entry : kDescriptions) {
        if (entry.id == profile->id) {
            line = entry.text;
            break;
        }
    }
    QString description = q(line);
    if (isRunningAt(*profile, runningServers)) {
        return description + QStringLiteral(" Running on this computer.");
    }
    if (profile->builtin) {
        const QString kind = q(profile->kind);
        for (const auto& [url, server] : runningServers) {
            if (server == kind) {
                return description + QStringLiteral(u" Answering at ") + url +
                       QStringLiteral(u" — choose Edit to use it.");
            }
        }
    }
    return description;
}

const std::vector<std::pair<QString, QString>>& TextModelPicker::sortOptions() {
    static const std::vector<std::pair<QString, QString>> options{
        {QStringLiteral(u"A → Z"), QStringLiteral("alphabetical")},
        {QStringLiteral("Most popular"), QStringLiteral("most-popular")},
        {QStringLiteral("Top this week"), QStringLiteral("top-weekly")},
        {QStringLiteral("Newest"), QStringLiteral("newest")},
        {QStringLiteral("Cheapest first"), QStringLiteral("pricing-low-to-high")},
        {QStringLiteral("Priciest first"), QStringLiteral("pricing-high-to-low")},
        {QStringLiteral("Largest context"), QStringLiteral("context-high-to-low")},
        {QStringLiteral("Highest throughput"), QStringLiteral("throughput-high-to-low")},
        {QStringLiteral("Lowest latency"), QStringLiteral("latency-low-to-high")},
    };
    return options;
}

TextModelPicker::TextModelPicker(QString idleStatus, QWidget* parent)
    : QWidget(parent), provider_(QStringLiteral("openai")), idleStatus_(std::move(idleStatus)),
      profiles_(llm::builtin_profiles()) {
    for (const llm::TextLLMProfile& profile : profiles_) {
        stagedModels_[q(profile.id)] = defaultModel(q(profile.id));
    }

    setObjectName(QStringLiteral("textModelPicker"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    auto* endpointLabel = new QLabel(QStringLiteral("Endpoint"));
    endpointLabel->setObjectName(QStringLiteral("textModelFieldLabel"));
    layout->addWidget(endpointLabel);

    providerCombo_ = new ElidingComboBox;
    providerCombo_->setObjectName(QStringLiteral("textModelProviderCombo"));
    providerCombo_->setMinimumHeight(40);
    providerCombo_->setIconSize(QSize(20, 20));
    connect(providerCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { onProviderComboChanged(); });
    layout->addWidget(providerCombo_);

    auto* credentialRow = new QHBoxLayout;
    credentialRow->setSpacing(8);
    credentialIcon_ = new QLabel;
    credentialIcon_->setObjectName(QStringLiteral("textProviderCredentialIcon"));
    credentialIcon_->setFixedSize(16, 16);
    requirementLabel_ = new QLabel;
    requirementLabel_->setObjectName(QStringLiteral("textProviderRequirement"));
    // Wrapped so the credential copy cannot set the window's width floor.
    requirementLabel_->setWordWrap(true);
    credentialRow->addWidget(credentialIcon_, 0, Qt::AlignTop);
    credentialRow->addWidget(requirementLabel_, 1);
    urlLabel_ = new QLabel;
    urlLabel_->setObjectName(QStringLiteral("textProviderUrl"));
    credentialRow->addWidget(urlLabel_);
    layout->addLayout(credentialRow);

    auto* manageRow = new QHBoxLayout;
    manageRow->setSpacing(8);
    editButton_ = new Button(QStringLiteral("Edit"));
    editButton_->setObjectName(QStringLiteral("textEndpointEditButton"));
    connect(editButton_, &QPushButton::clicked, this, [this] { emitEdit(); });
    deleteButton_ = new Button(QStringLiteral("Delete"));
    deleteButton_->setObjectName(QStringLiteral("textEndpointDeleteButton"));
    connect(deleteButton_, &QPushButton::clicked, this, [this] { emitDelete(); });
    for (Button* button : {editButton_, deleteButton_}) {
        button->setBaseMinimumSize(0, 28);
        button->setMaximumHeight(28);
        manageRow->addWidget(button);
    }
    manageRow->addStretch();
    layout->addLayout(manageRow);

    auto* separator = new QFrame;
    separator->setObjectName(QStringLiteral("textModelGroupSeparator"));
    separator->setFixedHeight(1);
    layout->addWidget(separator);

    auto* modelLabelRow = new QHBoxLayout;
    modelLabelRow->setSpacing(10);
    auto* modelLabel = new QLabel(QStringLiteral("Model"));
    modelLabel->setObjectName(QStringLiteral("textModelFieldLabel"));
    modelLabelRow->addWidget(modelLabel);
    modelLabelRow->addStretch();
    sortLabel_ = new QLabel(QStringLiteral("Sort catalog"));
    sortLabel_->setObjectName(QStringLiteral("textModelFieldLabel"));
    modelLabelRow->addWidget(sortLabel_);
    sortCombo_ = new ElidingComboBox;
    sortCombo_->setObjectName(QStringLiteral("textModelSort"));
    for (const auto& [label, value] : sortOptions()) {
        sortCombo_->addItem(label, value);
    }
    sortCombo_->setMinimumHeight(26);
    sortCombo_->setMaximumHeight(26);
    connect(sortCombo_, &QComboBox::currentIndexChanged, this,
            [this](int) { Q_EMIT sortChanged(provider_); });
    modelLabelRow->addWidget(sortCombo_);
    layout->addLayout(modelLabelRow);

    auto* modelRow = new QHBoxLayout;
    modelRow->setSpacing(10);
    modelCombo_ = new SearchableComboBox;
    modelCombo_->setObjectName(QStringLiteral("textModelCombo"));
    modelCombo_->setMinimumHeight(40);
    modelIcon_ = ui::designIcon(QStringLiteral("box-blue.svg"));
    modelCombo_->setIconSize(QSize(20, 20));
    connect(modelCombo_, &QComboBox::currentTextChanged, this,
            [this](const QString&) { updateActiveBadge(); });
    connect(modelCombo_, &QComboBox::textActivated, this,
            [this](const QString&) { onModelActivated(); });
    QLineEdit* modelLineEdit = modelCombo_->lineEdit();
    connect(modelLineEdit, &QLineEdit::textEdited, this,
            [this](const QString&) { modelEdited_ = true; });
    connect(modelLineEdit, &QLineEdit::returnPressed, this, [this] { commitModelSelection(); });
    connect(modelLineEdit, &QLineEdit::editingFinished, this, [this] { onModelEditingFinished(); });
    modelRow->addWidget(modelCombo_, 1);

    refreshButton_ = neutralButton(new Button(QStringLiteral("Refresh")));
    refreshButton_->setObjectName(QStringLiteral("textModelRefreshButton"));
    refreshButton_->setIcon(ui::designIcon(QStringLiteral("refresh-blue.svg")));
    refreshButton_->setIconSize(QSize(16, 16));
    refreshButton_->setBaseMinimumSize(100, 40);
    refreshButton_->setMaximumHeight(40);
    refreshButton_->setToolTip(QStringLiteral("Reload the selected provider's catalog"));
    connect(refreshButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT refreshRequested(provider_); });
    modelRow->addWidget(refreshButton_);
    layout->addLayout(modelRow);

    statusLabel_ = new QLabel(idleStatus_);
    statusLabel_->setObjectName(QStringLiteral("textModelStatus"));
    statusLabel_->setWordWrap(true);
    layout->addWidget(statusLabel_);

    reloadProviderCombo();
    renderProvider();
}

void TextModelPicker::enableInstalledAgentAction() {
    installedAgentAction_ = true;
    reloadProviderCombo();
    restoreProviderSelection();
}

void TextModelPicker::setProfiles(std::vector<llm::TextLLMProfile> profiles) {
    if (profiles.empty()) {
        profiles = llm::builtin_profiles();
    }
    QString current = provider_;
    profiles_ = std::move(profiles);
    for (const llm::TextLLMProfile& profile : profiles_) {
        const QString id = q(profile.id);
        if (stagedModels_.find(id) == stagedModels_.end()) {
            stagedModels_[id] = defaultModel(id);
        }
    }
    reloadProviderCombo();
    const std::string currentId = current.toStdString();
    const bool listed = std::any_of(profiles_.begin(), profiles_.end(),
                                    [&](const auto& profile) { return profile.id == currentId; });
    if (!llm::get_profile(currentId, Json::object()) && !listed) {
        current = q(profiles_.front().id);
    }
    setProvider(current);
}

std::optional<llm::TextLLMProfile> TextModelPicker::currentProfile() const {
    const std::string id = provider_.toStdString();
    for (const llm::TextLLMProfile& profile : profiles_) {
        if (profile.id == id) {
            return profile;
        }
    }
    return llm::get_profile(id, Json::object());
}

void TextModelPicker::setProvider(const QString& provider, std::optional<QString> model) {
    const std::string id = provider.toStdString();
    const bool known = std::any_of(profiles_.begin(), profiles_.end(),
                                   [&](const auto& profile) { return profile.id == id; });
    if (!known && !llm::get_profile(id, Json::object())) {
        return;
    }
    if (model && !model->isEmpty()) {
        stagedModels_[provider] = *model;
    }
    provider_ = provider;
    restoreProviderSelection();
    renderProvider();
}

void TextModelPicker::restoreProviderSelection() {
    const int index = providerCombo_->findData(provider_);
    const bool blocked = providerCombo_->blockSignals(true);
    providerCombo_->setCurrentIndex(std::max(0, index));
    providerCombo_->blockSignals(blocked);
}

void TextModelPicker::reloadProviderCombo() {
    const QIcon custom = ui::designIcon(QStringLiteral("stack-slate.svg"));
    const bool blocked = providerCombo_->blockSignals(true);
    providerCombo_->clear();
    for (const llm::TextLLMProfile& profile : profiles_) {
        QIcon icon = custom;
        if (profile.id == llm::kOpenAiProfileId) {
            icon = ui::designIcon(QStringLiteral("box-blue.svg"));
        } else if (profile.id == llm::kOpenRouterProfileId) {
            icon = ui::designIcon(QStringLiteral("stack-purple.svg"));
        }
        const QString name = q(profile.name);
        providerCombo_->addItem(
            icon, isRunning(profile) ? name + QStringLiteral(u" · Running") : name, q(profile.id));
        providerCombo_->setItemData(providerCombo_->count() - 1,
                                    profileTooltip(name, description(profile)), Qt::ToolTipRole);
    }
    if (installedAgentAction_) {
        providerCombo_->addItem(QStringLiteral(u"Installed agent…"),
                                QString::fromLatin1(kInstalledAgentOption));
    }
    providerCombo_->addItem(ui::designIcon(QStringLiteral("plus-blue.svg")),
                            QStringLiteral(u"Custom…"), QString::fromLatin1(kCustomOption));
    providerCombo_->setItemData(
        providerCombo_->count() - 1,
        QStringLiteral(u"Custom — add another OpenAI-compatible server or gateway"),
        Qt::ToolTipRole);
    providerCombo_->blockSignals(blocked);
}

void TextModelPicker::onProviderComboChanged() {
    const QString provider = providerCombo_->currentData().toString();
    if (provider.isEmpty()) {
        return;
    }
    if (provider == QLatin1String(kCustomOption)) {
        // Restore first, so cancelling the add dialog leaves the current endpoint selected; a
        // saved endpoint is selected by the host.
        restoreProviderSelection();
        Q_EMIT addEndpointRequested();
        return;
    }
    if (provider == QLatin1String(kInstalledAgentOption)) {
        restoreProviderSelection();
        Q_EMIT installedAgentRequested();
        return;
    }
    provider_ = provider;
    renderProvider();
    Q_EMIT providerChanged(provider);
}

QString TextModelPicker::description(const std::optional<llm::TextLLMProfile>& profile) const {
    return textEndpointDescription(profile, runningServers_);
}

void TextModelPicker::setRunningServers(const std::map<QString, QString>& servers) {
    setRunningServers(std::vector<std::pair<QString, QString>>(servers.begin(), servers.end()));
}

void TextModelPicker::setRunningServers(std::vector<std::pair<QString, QString>> servers) {
    runningServers_ = withoutRepeats(std::move(servers));
    reloadProviderCombo();
    restoreProviderSelection();
    if (const auto profile = currentProfile()) {
        providerCombo_->setToolTip(profileTooltip(q(profile->name), description(profile)));
    }
    updateCredentialStatus();
}

bool TextModelPicker::isRunning(const llm::TextLLMProfile& profile) const {
    return isRunningAt(profile, runningServers_);
}

bool TextModelPicker::canEdit(const llm::TextLLMProfile& profile) const {
    return !profile.builtin || llm::is_local_server_id(profile.id);
}

void TextModelPicker::renderProvider() {
    if (credential_ && credential_->provider != provider_) {
        credential_.reset();
    }
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    const QString text = description(profile);
    const QString name = profile ? q(profile->name) : QStringLiteral("Endpoint");
    providerCombo_->setToolTip(profileTooltip(name, text));
    if (profile && profile->base_url && !profile->base_url->empty()) {
        const QString url = q(*profile->base_url);
        urlLabel_->setText(
            urlLabel_->fontMetrics().elidedText(url, Qt::ElideMiddle, kUrlElideWidth));
        urlLabel_->setToolTip(url);
        urlLabel_->setVisible(true);
    } else {
        urlLabel_->setText(QString());
        urlLabel_->setToolTip(QString());
        urlLabel_->setVisible(false);
    }
    updateCredentialStatus();
    const bool showSort = profile && profile->kind == llm::kProfileKindOpenRouter;
    sortLabel_->setVisible(showSort);
    sortCombo_->setVisible(showSort);
    editButton_->setEnabled(profile && canEdit(*profile));
    deleteButton_->setEnabled(profile && !profile->builtin);

    modelEdited_ = false;
    modelCombo_->blockSignals(true);
    modelCombo_->clear();
    modelCombo_->setCurrentText(stagedModel());
    modelCombo_->blockSignals(false);
    statusLabel_->setText(QStringLiteral(u"Loading models…"));
    updateActiveBadge();
}

void TextModelPicker::setCredentialStatus(const QString& text, bool available, const QString& url) {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    const QString shown = profile ? q(profile->base_url.value_or(std::string())) : QString();
    // "" is the address of OpenAI (the one endpoint without one), not a wildcard: an answer read
    // for OpenAI that arrives after the user moved to another endpoint must not land on it.
    if (url != shown) {
        return;
    }
    credential_ = CredentialLine{text, available, shown, provider_};
    updateCredentialStatus();
}

void TextModelPicker::setCredentialStatus(const QString& text, bool available, const QString& url,
                                          const QString& provider) {
    if (provider != provider_) {
        return;
    }
    setCredentialStatus(text, available, url);
}

void TextModelPicker::updateCredentialStatus() {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    // The host's answer counts while it is for this endpoint and address. credential_status
    // needs no store for an endpoint without a key, so that one reads right away.
    std::optional<std::pair<bool, QString>> status;
    if (!profile) {
        status = std::pair{false, QStringLiteral("Unknown endpoint.")};
    } else if (credential_ && credential_->provider == provider_ &&
               credential_->url == q(profile->base_url.value_or(std::string()))) {
        status = std::pair{credential_->available, credential_->text};
    } else if (!profile->requires_api_key()) {
        status = std::pair{true, QStringLiteral("No API key required")};
    }
    if (!status) {
        requirementLabel_->setToolTip(QString());
        requirementLabel_->setText(QString());
        credentialIcon_->setPixmap(QPixmap());
        ui::setStyleProperty(requirementLabel_, ui::prop::available, false);
        return;
    }
    const bool available = status->first;
    QString text = status->second;
    requirementLabel_->setToolTip(text);
    const llm::TextLLMProfile* running = nullptr;
    for (const llm::TextLLMProfile& candidate : profiles_) {
        if (isRunning(candidate)) {
            running = &candidate;
            break;
        }
    }
    if (profile && !available && !profile->self_hosted() && running != nullptr) {
        text = QStringLiteral(u"Key needed in Settings → App → API keys, or choose ") +
               q(running->name) + QStringLiteral(", running on this computer");
    } else if (profile && profile->builtin && !available &&
               profile->kind != llm::kProfileKindOpenAi &&
               profile->kind != llm::kProfileKindOpenRouter) {
        text = QStringLiteral(u"Key needed in Settings → App → API keys");
    }
    requirementLabel_->setText(text);
    const QIcon icon = ui::designIcon(available ? QStringLiteral("check-green.svg")
                                                : QStringLiteral("info-warning.svg"));
    credentialIcon_->setPixmap(icon.pixmap(16, 16));
    ui::setStyleProperty(requirementLabel_, ui::prop::available, available);
}

QString TextModelPicker::defaultModel(const QString& provider) const {
    const std::string id = provider.toStdString();
    for (const llm::TextLLMProfile& profile : profiles_) {
        if (profile.id == id) {
            return q(llm::default_model_for_profile(profile));
        }
    }
    return q(llm::default_transcript_cleanup_model(id, Json::object()));
}

QString TextModelPicker::stagedModel() const {
    const auto found = stagedModels_.find(provider_);
    return found != stagedModels_.end() ? found->second : defaultModel(provider_);
}

void TextModelPicker::setStagedModel(const QString& provider, const QString& model) {
    stagedModels_[provider] = model;
}

void TextModelPicker::onModelActivated() {
    // Assign a model the moment it is picked from the catalog.
    commitModelSelection();
}

void TextModelPicker::onModelEditingFinished() {
    // Typing filters the dropdown, so the editor can hold a fragment such as "clau" that the user
    // never chose. Only Enter or a pick from the list assigns; anything else rewinds to the
    // model already staged.
    if (modelEdited_) {
        restoreStagedModel();
    }
}

std::optional<QString> TextModelPicker::modelRefusal(const QString& model) const {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    if (!profile) {
        return std::nullopt;
    }
    try {
        llm::model_spec(*profile, model.toStdString());
    } catch (const std::invalid_argument& error) {
        return q(error.what());
    } catch (const py::ValueError& error) {
        return q(error.what());
    }
    return std::nullopt;
}

void TextModelPicker::commitModelSelection() {
    modelEdited_ = false;
    const QString model = stripped(modelCombo_->currentText());
    if (model.isEmpty()) {
        restoreStagedModel();
        return;
    }
    if (const std::optional<QString> refusal = modelRefusal(model)) {
        statusLabel_->setText(*refusal);
        return;
    }
    stagedModels_[provider_] = model;
    updateActiveBadge();
    const bool alreadyActive = provider_ == activeProvider_ && model == activeModel_;
    if (!alreadyActive) {
        Q_EMIT activationRequested(provider_);
    }
}

void TextModelPicker::restoreStagedModel() {
    modelEdited_ = false;
    const QString staged = stagedModel();
    if (modelCombo_->currentText() != staged) {
        modelCombo_->setCurrentText(staged);
    }
    updateActiveBadge();
}

QString TextModelPicker::currentSort() const {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    if (!profile || profile->kind != llm::kProfileKindOpenRouter) {
        return QString::fromLatin1(kAlphabetical);
    }
    const QString sort = sortCombo_->currentData().toString();
    return sort.isEmpty() ? QString::fromLatin1(kAlphabetical) : sort;
}

void TextModelPicker::setSort(const QString& sort) {
    const int index = sortCombo_->findData(sort);
    const bool blocked = sortCombo_->blockSignals(true);
    sortCombo_->setCurrentIndex(std::max(0, index));
    sortCombo_->blockSignals(blocked);
}

void TextModelPicker::setModels(const QStringList& models) {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    auto* itemModel = qobject_cast<QStandardItemModel*>(modelCombo_->model());
    modelCombo_->blockSignals(true);
    modelCombo_->clear();
    for (const QString& model : models) {
        modelCombo_->addItem(modelIcon_, model);
        if (const std::optional<QString> refusal = modelRefusal(model);
            refusal && itemModel != nullptr) {
            QStandardItem* item = itemModel->item(modelCombo_->count() - 1);
            if (item != nullptr) {
                item->setEnabled(false);
                item->setToolTip(*refusal);
            }
        }
    }
    modelCombo_->setCurrentText(stagedModel());
    modelCombo_->blockSignals(false);
    modelEdited_ = false;
    updateActiveBadge();
}

void TextModelPicker::setLoading(bool loading) {
    refreshButton_->setEnabled(!loading);
    if (loading) {
        statusLabel_->setText(QStringLiteral(u"Loading models…"));
    }
}

void TextModelPicker::setStatus(const QString& text) {
    statusLabel_->setText(text);
}

void TextModelPicker::setActiveSelection(const QString& provider, const QString& model) {
    activeProvider_ = provider;
    activeModel_ = model;
    if (!model.isEmpty()) {
        stagedModels_[provider] = model;
    }
    updateActiveBadge();
}

QString TextModelPicker::activeProfileName() const {
    const std::string id = activeProvider_.toStdString();
    for (const llm::TextLLMProfile& profile : profiles_) {
        if (profile.id == id) {
            return q(profile.name);
        }
    }
    if (const auto profile = llm::get_profile(id, Json::object())) {
        return q(profile->name);
    }
    if (activeProvider_ == QLatin1String("openai")) {
        return QStringLiteral("OpenAI");
    }
    if (activeProvider_ == QLatin1String("openrouter")) {
        return QStringLiteral("OpenRouter");
    }
    return activeProvider_.isEmpty() ? QStringLiteral("Endpoint") : activeProvider_;
}

void TextModelPicker::updateActiveBadge() {
    // The badge is the only report of what is in use, so an endpoint being browsed shows no badge
    // and the combo tooltip names the real one.
    const QString selected = stripped(modelCombo_->currentText());
    const bool isActive =
        !activeProvider_.isEmpty() && provider_ == activeProvider_ && selected == activeModel_;
    modelCombo_->setBadge(isActive ? QStringLiteral("Active") : QString());
    if (isActive) {
        modelCombo_->setToolTip(activeProfileName() + QStringLiteral(u" · ") + activeModel_ +
                                QStringLiteral(" is the model in use."));
    } else {
        modelCombo_->setToolTip(
            QStringLiteral(u"Choose from the catalog or type a model id — the choice is saved "
                           u"immediately. In use now: ") +
            activeProfileName() + QStringLiteral(u" · ") + activeModel_ + QStringLiteral("."));
    }
}

void TextModelPicker::emitEdit() {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    if (profile && canEdit(*profile)) {
        Q_EMIT editEndpointRequested(q(profile->id));
    }
}

void TextModelPicker::emitDelete() {
    const std::optional<llm::TextLLMProfile> profile = currentProfile();
    if (profile && !profile->builtin) {
        Q_EMIT deleteEndpointRequested(q(profile->id));
    }
}

} // namespace fv
