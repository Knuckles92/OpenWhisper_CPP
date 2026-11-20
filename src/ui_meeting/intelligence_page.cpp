#include "ui_meeting/intelligence_page.h"

#include <algorithm>
#include <map>
#include <utility>

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "agents/installed_agents.h"
#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/env.h"
#include "base/identity.h"
#include "llm/catalog_client.h"
#include "llm/cleanup_settings.h"
#include "llm/thinking_levels.h"
#include "meeting_agent/agent_settings.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_meeting/ai_insights_copy.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_widgets/agent_picker.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/text_endpoint_dialog.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/thinking_combo.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

QString q(std::string_view value) {
    return qt::toQString(value);
}

QWidget* field(const QString& label, QWidget* control) {
    auto* row = new QWidget;
    auto* layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(new WrappedLabel(label), 1);
    control->setAccessibleName(label);
    layout->addWidget(control);
    return row;
}

} // namespace

IntelligencePage::IntelligencePage(SettingsPage& page, AppContext& context,
                                   FolderChooser chooseFolder)
    : QWidget(&page), page_(page), context_(context) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    // Who runs AI insights comes first, above the Insights model card, which holds only the
    // picker's model row (build_meeting_model_section).
    agents_ = new AgentPicker(QStringLiteral("AI insights"), false);
    layout->addWidget(agents_);
    auto* modelTile =
        new InfoTile(QStringLiteral("Insights model"),
                     QStringLiteral("The chosen model can receive transcript text and dashboard "
                                    "state when you turn on AI insights for a meeting."));
    modelTile->setObjectName(QStringLiteral("meeting_insights_model_tile"));
    builtinDescription_ = modelTile->descriptionLabel()->text();
    auto* modelBody = new QWidget;
    auto* modelLayout = new QVBoxLayout(modelBody);
    modelLayout->setContentsMargins(0, 0, 0, 0);
    modelLayout->addWidget(agents_->modelCard());
    models_ =
        new TextModelPicker(QStringLiteral("Refresh to ask the selected endpoint for its models."));
    modelLayout->addWidget(models_);
    auto* thinkingNote = new WrappedLabel(QString());
    thinkingNote->setObjectName(QStringLiteral("meetingBuiltinThinking"));
    modelLayout->addWidget(thinkingNote);
    live_ = new QComboBox;
    live_->setObjectName(QStringLiteral("meeting_live_thinking"));
    modelLayout->addWidget(field(QStringLiteral("Live thinking"), live_));
    final_ = new QComboBox;
    final_->setObjectName(QStringLiteral("meeting_final_thinking"));
    modelLayout->addWidget(field(QStringLiteral("Final thinking"), final_));
    backup_ = new QComboBox;
    backup_->setObjectName(QStringLiteral("meeting_backup_agent"));
    modelLayout->addWidget(field(QStringLiteral("Backup agent"), backup_));
    modelTile->addBody(modelBody);
    layout->addWidget(modelTile);

    auto* recallTile =
        new InfoTile(QStringLiteral("Search past transcripts"),
                     QStringLiteral("Let the meeting agent include relevant excerpts from earlier "
                                    "meetings in its requests."));
    recall_ = new SettingsSwitch;
    recall_->setObjectName(QStringLiteral("meeting_past_recall_enabled"));
    recallTile->addBody(field(QStringLiteral("Allow past-meeting search"), recall_));
    layout->addWidget(recallTile);

    auto* folderTile = new InfoTile(QStringLiteral("Search a knowledge folder"),
                                    QStringLiteral("Let the meeting agent search this folder and "
                                                   "include relevant passages in its requests."));
    folderEnabled_ = new SettingsSwitch;
    folderEnabled_->setObjectName(QStringLiteral("meeting_context_folder_enabled"));
    folderTile->addBody(field(QStringLiteral("Allow knowledge-folder search"), folderEnabled_));
    auto* location = new QWidget;
    auto* locationRow = new QHBoxLayout(location);
    locationRow->setContentsMargins(0, 0, 0, 0);
    folder_ = new QLineEdit;
    folder_->setObjectName(QStringLiteral("meeting_context_folder_path"));
    folder_->setAccessibleName(QStringLiteral("Knowledge folder"));
    folder_->setPlaceholderText(QStringLiteral("No folder selected"));
    auto* browse = new Button(QStringLiteral("Browse…"));
    auto* clear = new Button(QStringLiteral("Clear"));
    browse->setObjectName(QStringLiteral("browseMeetingKnowledgeFolder"));
    clear->setObjectName(QStringLiteral("clearMeetingKnowledgeFolder"));
    locationRow->addWidget(folder_, 1);
    locationRow->addWidget(browse);
    locationRow->addWidget(clear);
    folderTile->addBody(location);
    layout->addWidget(folderTile);

    connect(agents_, &AgentPicker::choiceRequested, this, [this](const QString& id) {
        const auto core = qt::toUtf8(id);
        save(keys::meeting_agent_core.key(),
             meeting_agent::is_installed_core(core) ? core : lastBuiltin_);
    });
    connect(agents_, &AgentPicker::modelChosen, this,
            [this](const QString& id, const QString& model) {
                auto saved = context_.settings().get(keys::meeting_agent_models);
                saved[qt::toUtf8(id)] = qt::toUtf8(model);
                save(keys::meeting_agent_models.key(), std::move(saved));
            });
    connect(models_, &TextModelPicker::activationRequested, this, &IntelligencePage::assignModel);
    connect(models_, &TextModelPicker::providerChanged, this, [this] {
        loading_.cancel();
        models_->setLoading(false);
        models_->setModels({});
    });
    connect(models_, &TextModelPicker::refreshRequested, this, [this] { refreshModels(); });
    connect(models_, &TextModelPicker::addEndpointRequested, this, [this] { editEndpoint(false); });
    connect(models_, &TextModelPicker::editEndpointRequested, this, [this] { editEndpoint(true); });
    connect(models_, &TextModelPicker::deleteEndpointRequested, this,
            &IntelligencePage::deleteEndpoint);
    for (const auto& [combo, key] : {std::pair{backup_, keys::meeting_backup_agent.key()},
                                     std::pair{live_, keys::meeting_live_thinking.key()},
                                     std::pair{final_, keys::meeting_final_thinking.key()}}) {
        connect(combo, &QComboBox::currentIndexChanged, this, [this, combo, key] {
            if (combo->currentData().isValid()) {
                save(key, qt::toUtf8(combo->currentData().toString()));
            }
        });
    }
    connect(recall_, &QAbstractButton::toggled, this,
            [this](bool checked) { save(keys::meeting_past_recall_enabled.key(), checked); });
    connect(folderEnabled_, &QAbstractButton::toggled, this,
            [this](bool checked) { save(keys::meeting_context_folder_enabled.key(), checked); });
    connect(folder_, &QLineEdit::editingFinished, this, [this] {
        save(keys::meeting_context_folder_path.key(), qt::toUtf8(folder_->text().trimmed()));
    });
    connect(browse, &QPushButton::clicked, this, [this, chooseFolder = std::move(chooseFolder)] {
        const auto title = QStringLiteral("Select knowledge folder");
        // The field's folder only while it exists, else the home folder (_browse_context_folder).
        const QString current = folder_->text().trimmed();
        const QString start =
            !current.isEmpty() && QFileInfo(current).isDir() ? current : QDir::homePath();
        const auto& seam = folderChooserSeam();
        const auto directory = chooseFolder ? chooseFolder(this, title, start)
                               : seam       ? seam(this, title, start)
                                            : QFileDialog::getExistingDirectory(this, title, start);
        if (!directory.isEmpty()) {
            saveFolder(normalizedFolder(directory));
        }
    });
    connect(clear, &QPushButton::clicked, this, [this] { saveFolder({}); });
    loadSettings(*context_.settings().snapshot());
    agents_->ensureScanned();
}

IntelligencePage::~IntelligencePage() {
    loading_.cancel();
}

void IntelligencePage::loadSettings(const Json& settings) {
    const std::string core = meeting_agent::resolve_meeting_agent_core(settings);
    const bool installed = meeting_agent::is_installed_core(core);
    if (!installed) {
        lastBuiltin_ = core;
    }
    agents_->setChoice(q(core));
    agents_->tiles().back()->setIdentity(q(identity().product), QStringLiteral("F"));
    std::map<QString, QString> saved;
    for (const auto& [id, model] : meeting_agent::resolve_meeting_agent_models(settings)) {
        saved.emplace(q(id), q(model));
    }
    agents_->setSavedModels(saved);

    models_->setProfiles(llm::list_profiles(settings));
    const auto provider = q(llm::resolve_meeting_llm_provider(settings));
    const auto model = q(llm::resolve_meeting_llm_model(settings));
    models_->setProvider(provider, model);
    models_->setActiveSelection(provider, model);
    models_->setVisible(!installed);
    refreshRunner(settings, core);
    const QSignalBlocker backupBlock(backup_);
    backup_->clear();
    backup_->addItem(QStringLiteral("None"), QString());
    for (const auto id : meeting_agent::INSTALLED_CORES) {
        if (id != core) {
            backup_->addItem(q(llm::profile_display_name(id, settings)), q(id));
        }
    }
    backup_->setCurrentIndex(
        std::max(0, backup_->findData(q(meeting_agent::resolve_meeting_backup_agent(settings)))));
    const QSignalBlocker recallBlock(recall_);
    const QSignalBlocker folderBlock(folderEnabled_);
    recall_->setChecked(keys::meeting_past_recall_enabled.read(settings));
    folderEnabled_->setChecked(keys::meeting_context_folder_enabled.read(settings));
    folder_->setText(q(keys::meeting_context_folder_path.read(settings)));
}

// _apply_meeting_agent_choice's description and _refresh_meeting_thinking (settings_models.py
// 1078-1116): an installed agent takes the levels it offers for its model; the built-in agent
// thinks at Medium itself, so its combos show Medium, disabled.
void IntelligencePage::refreshRunner(const Json& settings, const std::string& core) {
    auto* tile = findChild<InfoTile*>(QStringLiteral("meeting_insights_model_tile"));
    auto* note = findChild<WrappedLabel*>(QStringLiteral("meetingBuiltinThinking"));
    QString text;
    if (!meeting_agent::is_installed_core(core)) {
        tile->setDescription(builtinDescription_);
        for (auto* combo : {live_, final_}) {
            fillThinkingCombo(combo, {"medium"}, "medium");
            combo->setEnabled(false);
        }
        text = QStringLiteral("%1 thinks at Medium on every pass when the model can think.")
                   .arg(agents_->tile(q(legacy_builtin_agent_id))->name());
    } else {
        const auto name = q(agents::agent_spec(core).name);
        tile->setDescription(QStringLiteral("%1 runs live cards, the note taker, polish, "
                                            "summaries, and the final report.")
                                 .arg(name));
        const auto offered =
            llm::agent_levels(core, meeting_agent::resolve_meeting_agent_model(core, settings));
        const std::string saved[] = {meeting_agent::resolve_meeting_live_thinking(settings),
                                     meeting_agent::resolve_meeting_final_thinking(settings)};
        text = QStringLiteral("Sent to %1 as its reasoning effort. Live passes run every few "
                              "minutes, so a lower level keeps cards quick.")
                   .arg(name);
        QStringList moved;
        QComboBox* combos[] = {live_, final_};
        for (int i = 0; i < 2; ++i) {
            const auto shown = fillThinkingCombo(combos[i], offered, saved[i]);
            combos[i]->setEnabled(true);
            if (shown && *shown != saved[i]) {
                const auto label = [](std::string_view level) {
                    return q(llm::thinking_label(level).value_or(level));
                };
                const QString change =
                    QStringLiteral("%1 runs at %2").arg(label(saved[i]), label(*shown));
                if (!moved.contains(change)) {
                    moved.append(change);
                }
            }
        }
        if (!moved.isEmpty()) {
            text +=
                QStringLiteral(" With this model, %1.").arg(moved.join(QStringLiteral(" and ")));
        }
    }
    for (auto* combo : {live_, final_}) {
        combo->setToolTip(text);
    }
    note->setText(text);
}

QWidget* IntelligencePage::namedControl(std::string_view name) {
    // The picker keeps the object name its style sheet rules select.
    if (name == "meeting_agent_picker") {
        return agents_;
    }
    return findChild<QWidget*>(q(name));
}

IntelligencePage::FolderChooser& IntelligencePage::folderChooserSeam() {
    static FolderChooser chooser;
    return chooser;
}

QString IntelligencePage::normalizedFolder(const QString& path) {
    // os.path.normpath: "." and ".." segments, doubled and trailing separators, and the
    // platform's separator.
    return QDir::toNativeSeparators(QDir::cleanPath(path));
}

void IntelligencePage::saveFolder(const QString& path) {
    const Json updates = {
        {std::string(keys::meeting_context_folder_enabled.key()), !path.isEmpty()},
        {std::string(keys::meeting_context_folder_path.key()), qt::toUtf8(path)}};
    if (auto* dialog = page_.dialog()) {
        // The dialog's _persist_many reports a failure as "Couldn't save settings: <error>".
        dialog->persistMany(updates);
    } else {
        try {
            context_.settings().update(updates);
        } catch (const std::exception&) {
            // A page outside the dialog (tests) has nowhere to say so; the reload shows the
            // stored value.
        }
    }
    loadSettings(*context_.settings().snapshot());
}

void IntelligencePage::save(std::string_view key, Json value) {
    try {
        auto& settings = context_.settings();
        if (settings.value(key) != value) {
            settings.save_setting(key, value);
        }
        loadSettings(*settings.snapshot());
    } catch (const std::exception&) {
        page_.dialog()->showMessage(
            QStringLiteral("Could not save the meeting intelligence setting."));
    }
}

void IntelligencePage::assignModel(const QString& id) {
    const auto model = models_->stagedModels().find(id);
    if (model == models_->stagedModels().end() || model->second.trimmed().isEmpty()) {
        return;
    }
    try {
        loadSettings(context_.settings().update(
            {{std::string(keys::meeting_llm_provider.key()), qt::toUtf8(id)},
             {std::string(keys::meeting_llm_model.key()), qt::toUtf8(model->second.trimmed())}}));
    } catch (const std::exception&) {
        page_.dialog()->showMessage(QStringLiteral("Could not save the meeting insights model."));
    }
}

void IntelligencePage::refreshModels() {
    const auto profile = models_->currentProfile();
    if (!profile) {
        return;
    }
    try {
        auto* client = &context_.http();
        auto dotenv = std::make_shared<DotEnv>(context_.paths().env_file());
        auto* credentialsStore = &context_.credentials();
        auto* ui = &context_.ui();
        loading_.cancel();
        loading_ = CancelSource{};
        const auto token = loading_.token();
        models_->setLoading(true);
        context_.io().post([client, credentialsStore, dotenv, ui, token, profile = *profile,
                            sort = qt::toUtf8(models_->currentSort()),
                            page = QPointer<IntelligencePage>(this)] {
            QStringList models;
            QString status;
            llm::KeyStatus credentials;
            try {
                llm::KeySources sources{*credentialsStore, system_env(), *dotenv};
                credentials = llm::credential_status(profile, sources);
                for (const auto& model :
                     llm::list_chat_models(*client, profile, {}, sort, token, &sources)) {
                    models.push_back(q(model));
                }
            } catch (const std::exception&) {
                status = QStringLiteral("Could not load models from this endpoint.");
            }
            ui->post([page, profile, token, models = std::move(models), status, credentials] {
                if (!page || token.cancelled() || page->models_->currentProfile() != profile) {
                    return;
                }
                page->models_->setLoading(false);
                page->models_->setModels(models);
                page->models_->setStatus(status);
                page->models_->setCredentialStatus(q(credentials.text), credentials.available,
                                                   q(profile.base_url.value_or("")), q(profile.id));
            });
        });
    } catch (const ServiceUnavailable&) {
        models_->setStatus(QStringLiteral("Model catalog access is unavailable."));
    }
}

void IntelligencePage::editEndpoint(bool editing) {
    const auto current = editing ? models_->currentProfile() : std::nullopt;
    TextEndpointDialog dialog(current, this);
    if (dialog.exec() != QDialog::Accepted || !dialog.resultPayload()) {
        return;
    }
    try {
        const auto result = *dialog.resultPayload();
        std::string id;
        const auto settings = context_.settings().mutate([&](Json& settings) {
            if (current && llm::is_local_server_id(current->id)) {
                llm::save_local_server(settings, current->id, result.baseUrl, result.apiKeyEnv);
                id = current->id;
            } else {
                id = llm::upsert_custom_profile(
                         settings, result.name, result.baseUrl, result.apiKeyEnv,
                         result.id ? std::optional<std::string_view>(*result.id) : std::nullopt,
                         result.protocol)
                         .id;
            }
        });
        loadSettings(settings);
        models_->setProvider(q(id));
    } catch (const std::exception&) {
        page_.dialog()->showMessage(QStringLiteral("Could not save the text endpoint."));
    }
}

void IntelligencePage::deleteEndpoint(const QString& id) {
    if (QMessageBox::question(this, QStringLiteral("Delete text endpoint?"),
                              QStringLiteral("Remove this custom endpoint from Settings?"),
                              QMessageBox::Yes | QMessageBox::No,
                              QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    try {
        loadSettings(context_.settings().mutate(
            [&](Json& settings) { llm::remove_custom_profile(settings, qt::toUtf8(id)); }));
    } catch (const std::exception&) {
        page_.dialog()->showMessage(QStringLiteral("Could not remove the text endpoint."));
    }
}

void registerIntelligencePage(AppContext& context) {
    SettingsPageSpec spec;
    spec.destination = destinations::meeting_intelligence;
    spec.title = QStringLiteral("Intelligence");
    // OpenWhisper names Pi, its packaged agent; the built-in agent here is the product (M8 lead
    // decision 1).
    spec.subtitle = QStringLiteral("%1 or a coding agent you already use runs every Meeting Mode "
                                   "pass. Nothing is sent until you turn on AI insights for a "
                                   "meeting.")
                        .arg(q(identity().product));
    spec.create = [&context](QWidget* parent) {
        return new IntelligencePage(*static_cast<SettingsPage*>(parent), context);
    };
    spec.railValue = [](const Json& settings) {
        try {
            return q(meeting_text_summary(settings));
        } catch (const std::exception&) {
            return q(meetingIntelligenceSummary(settings));
        }
    };
    spec.controls = {"meeting_agent_picker",           "meeting_insights_model_tile",
                     "meeting_live_thinking",          "meeting_final_thinking",
                     "meeting_backup_agent",           "meeting_past_recall_enabled",
                     "meeting_context_folder_enabled", "meeting_context_folder_path"};
    context.settingsPages().addPage(std::move(spec));
}

} // namespace fv
