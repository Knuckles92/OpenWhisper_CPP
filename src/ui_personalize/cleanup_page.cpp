#include "ui_personalize/cleanup_page.h"

#include <algorithm>
#include <map>
#include <memory>
#include <utility>

#include <QComboBox>
#include <QEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "text/cleanup_levels.h"
#include "text/cleanup_prompts.h"
#include "ui_personalize/cleanup_model_section.h"
#include "ui_personalize/cleanup_prompt_dialog.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/personalize_host.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"
#include "ui_theme/icons.h"
#include "ui_widgets/button_row.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/text_model_picker.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

namespace levels = text::cleanup_levels;

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

const QString kSwitchTitle = QStringLiteral("Clean up transcripts with AI");
const QString kSwitchDescription =
    QStringLiteral("Runs the selected language model on each dictated transcript after "
                   "transcription, together with your learned rules. The provider's key lives "
                   "under API keys.");
const QString kLevelTitle = QStringLiteral("Cleanup level");
const QString kLevelDescription =
    QStringLiteral("How much the AI changes your words. Every level keeps the language you "
                   "spoke and never adds anything you didn't say.");
const QString kPromptTitle = QStringLiteral("Custom prompt (optional)");
const QString kPromptDescription =
    QStringLiteral("Replaces the level's instructions with your own. Learned rules still apply. "
                   "While it's empty, the box previews the level's instructions.");

// resolve_transcript_cleanup_level: the saved preset name.
std::string savedLevel(const Json& settings) {
    return keys::transcript_cleanup_level.read(settings.is_object() ? settings : Json::object());
}

// settings_home.py _cleanup_status, with whether the provider's key is found (nullopt while
// that is not known yet: the line then says nothing about the key).
QString cleanupStatus(const Json& settings, std::optional<bool> keyFound) {
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(provider, settings);
    const QString name = q(llm::profile_display_name(provider, settings));
    if (llm::is_installed_cleanup_agent(provider)) {
        if (keys::fv_cleanup_agent_consent.read(settings) != provider) {
            return name + QStringLiteral(u" · Approval needed in AI cleanup settings.");
        }
        return name + QStringLiteral(u" · Uses your installed agent's sign-in. Transcript text "
                                     u"is sent to its model.");
    }
    if (profile && profile->is_local()) {
        return name + QStringLiteral(u" · Transcript text stays on this computer.");
    }
    const QString status =
        name + QStringLiteral(u" · Transcript text goes to this provider when cleanup is on.");
    const bool configured = profile && (!profile->requires_api_key() || keyFound.value_or(true));
    return configured ? status : status + QStringLiteral(" API key needed.");
}

} // namespace

CleanupPage::CleanupPage(PersonalizeHost& host, SettingsPage& container, CleanupSettings& cleanup)
    : PersonalizePage(host, container), cleanup_(cleanup) {
    cleanupTile_ = new SettingTile(kSwitchTitle, kSwitchDescription,
                                   ui::designIcon(QStringLiteral("stack-purple.svg")));
    connect(cleanupTile_->checkbox(), &QAbstractButton::toggled, this,
            &CleanupPage::onCleanupEnabledChanged);
    std::vector<std::pair<QString, QString>> segments;
    for (const levels::Segment& segment : levels::SEGMENTS) {
        segments.emplace_back(q(segment.title), q(segment.detail));
    }
    levelBar_ = new SegmentedBar(segments);
    levelBar_->setAccessibleName(QStringLiteral("Cleanup level"));
    // activated, not currentIndexChanged: loading the saved level must not save it back or ask
    // about a custom prompt.
    connect(levelBar_, &SegmentedBar::activated, this, &CleanupPage::onLevelActivated);
    levelBar_->buttons().back()->setToolTip(
        QStringLiteral("Write a custom prompt below to use it instead of a level"));
    levelTile_ = new FieldTile(kLevelTitle, kLevelDescription, levelBar_,
                               ui::designIcon(QStringLiteral("wand-purple.svg")));
    levelNote_ = new WrappedLabel(QString());
    levelNote_->setObjectName(QStringLiteral("cleanupLevelNote"));
    levelTile_->addBody(levelNote_);
    // No captions: each card's own title already says what it is.
    addTileGroup(QString(), {cleanupTile_, levelTile_}, 1);

    CleanupModelSection::Hooks hooks;
    hooks.settings = [this] {
        return this->host().settings();
    };
    hooks.say = [this](const QString& text) {
        this->host().showMessage(text);
    };
    hooks.refreshRail = [this] {
        if (SettingsDialog* dialog = settingsDialogOf(this)) {
            dialog->refreshRailValues();
        }
    };
    hooks.isCurrent = [this] {
        SettingsDialog* dialog = settingsDialogOf(this);
        return dialog != nullptr && dialog->currentDestination() == q(destinations::cleanup);
    };
    model_ = new CleanupModelSection(cleanup_.context().settings(), cleanup_.services(),
                                     std::move(hooks));
    // Who runs AI cleanup, above the card that holds the chosen runner's model
    // (build_cleanup_model_section).
    pageLayout()->addWidget(model_->runnerControls());
    pageLayout()->addWidget(model_);
    pageLayout()->addSpacing(6);

    // Plain, not rich text: its placeholder, the level's instructions, wraps instead of being
    // clipped to one line.
    promptEdit_ = new QPlainTextEdit;
    promptEdit_->setObjectName(QStringLiteral("cleanupPromptEditor"));
    promptEdit_->setMinimumHeight(96);
    promptEdit_->setMaximumHeight(120);
    promptEdit_->setAccessibleName(QStringLiteral("Custom prompt"));
    promptEdit_->installEventFilter(this);
    promptTile_ = new FieldTile(kPromptTitle, kPromptDescription, promptEdit_,
                                ui::designIcon(QStringLiteral("typography-blue.svg")));
    promptEditButton_ = neutralButton(new Button(QStringLiteral(u"Open editor…")));
    promptEditButton_->setToolTip(
        QStringLiteral("Edit in a larger window, starting from the level's instructions"));
    fitCompactButton(promptEditButton_, 0);
    connect(promptEditButton_, &QPushButton::clicked, this, &CleanupPage::openCleanupPromptEditor);
    promptResetButton_ = neutralButton(new Button(QStringLiteral("Remove custom prompt")));
    promptResetButton_->setToolTip(QStringLiteral("Go back to the level's instructions"));
    fitCompactButton(promptResetButton_, 0);
    connect(promptResetButton_, &QPushButton::clicked, this, &CleanupPage::resetCleanupPrompt);
    // Wraps rather than overflowing a narrow tiled window at 130%.
    promptTile_->addBody(new ButtonRow({promptEditButton_, promptResetButton_}));
    connect(promptEdit_, &QPlainTextEdit::textChanged, this, &CleanupPage::updatePromptButtons);
    addTileGroup(QString(), {promptTile_});
    // The bar selects a clicked segment after its activated handlers run, so a declined switch
    // away from Custom is put back on the next turn.
    levelResync_ = new QTimer(this);
    levelResync_->setSingleShot(true);
    levelResync_->setInterval(0);
    connect(levelResync_, &QTimer::timeout, this, &CleanupPage::updateCleanupLevelUi);
    // OpenWhisper's page column ends in a stretch (ensure_page).
    pageLayout()->addStretch();

    editPrompt = [](const QString& prompt, QWidget* parent) -> std::optional<QString> {
        CleanupPromptDialog dialog(prompt, parent);
        if (dialog.exec() != QDialog::Accepted) {
            return std::nullopt;
        }
        return dialog.promptText();
    };

    exposeControl("transcript_cleanup_tile", cleanupTile_);
    exposeControl("transcript_cleanup_check", cleanupTile_->checkbox());
    exposeControl("cleanup_level_tile", levelTile_);
    exposeControl("cleanup_level_bar", levelBar_);
    exposeControl("cleanup_level_note", levelNote_);
    exposeControl("cleanup_model_tile", model_);
    exposeControl("cleanup_prompt_edit", promptEdit_);
    exposeControl("cleanup_prompt_tile", promptTile_);
    exposeControl("cleanup_prompt_edit_btn", promptEditButton_);
    exposeControl("cleanup_prompt_reset_btn", promptResetButton_);
    exposeControl("text_model_picker", model_->picker());
    exposeControl("cleanup_reasoning_combo", model_->reasoningCombo());

    if (SettingsDialog* dialog = settingsDialogOf(&container)) {
        cleanup_.attach(*dialog);
    }
    cleanup_.addPage(this);
}

CleanupPage::~CleanupPage() = default;

QString CleanupPage::railValue(const Json& settings) {
    const std::string model = llm::resolve_transcript_cleanup_model(settings);
    QString value = q(text::level_label(settings));
    if (text::resolve_level(settings) != text::CleanupLevel::None && !model.empty()) {
        value += QStringLiteral(u" · ") + q(model);
    }
    return value;
}

SettingsSwitch* CleanupPage::cleanupCheck() const {
    return cleanupTile_->checkbox();
}

bool CleanupPage::cleanupEnabled() const {
    return cleanupTile_->checkbox()->isChecked();
}

void CleanupPage::load(const Json& settings) {
    cleanupTile_->checkbox()->setChecked(keys::transcript_cleanup_enabled.read(settings));
    // Empty unless a real custom prompt is saved; the preset shows as the placeholder.
    const QString prompt = q(text::custom_prompt(settings));
    promptEdit_->setPlainText(prompt);
    savedPrompt_ = prompt;
    model_->loadTextSettings(settings);
    cleanup_.updateCleanupUi(this);
}

void CleanupPage::onCleanupEnabledChanged(bool checked) {
    cleanup_.setCleanupEnabled(checked, this);
}

void CleanupPage::onLevelActivated(int index) {
    if (index < 0 || index >= static_cast<int>(levels::SEGMENTS.size())) {
        return;
    }
    const std::string_view level = levels::SEGMENTS[static_cast<std::size_t>(index)].value;
    // Custom can only be clicked while the custom prompt is in use.
    if (level != levels::CUSTOM) {
        cleanup_.chooseCleanupLevel(q(level), this);
    }
    // Started after the confirmation's own event loop has closed, so it runs once the bar has
    // selected the clicked segment.
    levelResync_->start();
}

void CleanupPage::updateCleanupPromptUi() {
    const bool enabled = cleanupEnabled();
    levelTile_->setEnabled(enabled);
    promptTile_->setEnabled(enabled);
    updateCleanupLevelUi();
}

void CleanupPage::updateCleanupLevelUi() {
    const Json settings = host().settings();
    const std::string level = savedLevel(settings);
    const bool custom = !text::custom_prompt(settings).empty();
    const std::string choice = custom ? std::string(levels::CUSTOM) : level;
    levelBar_->buttons().back()->setEnabled(custom);
    levelBar_->setCurrentIndex(static_cast<int>(levels::segment_index(choice)));
    levelNote_->setText(q(levels::note(choice)));
    promptEdit_->setPlaceholderText(q(text::preset(level)));
    updatePromptButtons();
}

void CleanupPage::updatePromptButtons() {
    promptResetButton_->setEnabled(!savedPrompt_.isEmpty() ||
                                   !promptEdit_->toPlainText().trimmed().isEmpty());
}

void CleanupPage::forgetCustomPrompt() {
    savedPrompt_.clear();
    promptEdit_->clear();
}

void CleanupPage::persistCleanupPrompt() {
    const QString text = promptEdit_->toPlainText();
    Json probe = Json::object();
    probe[std::string(keys::transcript_cleanup_prompt.key())] = u(text);
    const QString custom = q(text::custom_prompt(probe));
    if (custom.isEmpty() && !text.isEmpty()) {
        promptEdit_->clear();
    }
    if (custom == savedPrompt_) {
        return;
    }
    const bool saved =
        custom.isEmpty()
            ? saveMany(Json::object(), {std::string(keys::transcript_cleanup_prompt.key())})
            : save(keys::transcript_cleanup_prompt.key(), u(custom));
    if (saved) {
        savedPrompt_ = custom;
        cleanup_.updateCleanupUi(this);
    }
}

void CleanupPage::openCleanupPromptEditor() {
    // With no custom prompt yet, start from the level's instructions.
    QString text = promptEdit_->toPlainText().trimmed();
    if (text.isEmpty()) {
        text = q(text::preset(savedLevel(host().settings())));
    }
    const std::optional<QString> edited = editPrompt ? editPrompt(text, window()) : std::nullopt;
    if (edited) {
        promptEdit_->setPlainText(*edited);
        persistCleanupPrompt();
    }
}

void CleanupPage::resetCleanupPrompt() {
    // Remove the custom prompt, after asking, so the level applies again.
    const Json settings = host().settings();
    if (!text::custom_prompt(settings).empty() &&
        !cleanup_.confirmCustomPromptRemoval(q(savedLevel(settings)), window())) {
        return;
    }
    if (saveMany(Json::object(), {std::string(keys::transcript_cleanup_prompt.key())})) {
        forgetCustomPrompt();
        cleanup_.updateCleanupUi(this);
    }
}

bool CleanupPage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == promptEdit_ && event->type() == QEvent::FocusOut) {
        persistCleanupPrompt();
    }
    return PersonalizePage::eventFilter(watched, event);
}

bool CleanupPage::saveDrafts(bool ask) {
    (void)ask;
    persistCleanupPrompt();
    return true;
}

void CleanupPage::onShown() {
    model_->onDestinationShown();
}

// --- Registration ---

void registerCleanupPage(AppContext& context) {
    PageDescriptor descriptor;
    descriptor.destination = destinations::cleanup;
    descriptor.title = QStringLiteral("AI cleanup");
    descriptor.subtitle = QStringLiteral("Rewrite a finished dictation with a language model, "
                                         "together with your learned rules.");
    descriptor.create = [&context](PersonalizeHost& host,
                                   SettingsPage& container) -> PersonalizePage* {
        return new CleanupPage(host, container, context.service<CleanupSettings>());
    };
    descriptor.railValue = &CleanupPage::railValue;
    descriptor.searchFields = {
        {"transcript_cleanup_tile", kSwitchTitle, kSwitchDescription},
        {"cleanup_level_tile", kLevelTitle, kLevelDescription},
        {"cleanup_prompt_tile", kPromptTitle, kPromptDescription},
        {"cleanup_model_tile", QStringLiteral("Cleanup model"), CleanupModelSection::kDescription},
        {"cleanup_reasoning_combo", QStringLiteral("Thinking level"), QString()},
    };
    descriptor.controls = {
        "transcript_cleanup_tile",  "transcript_cleanup_check", "cleanup_level_tile",
        "cleanup_level_bar",        "cleanup_level_note",       "cleanup_model_tile",
        "cleanup_prompt_edit",      "cleanup_prompt_tile",      "cleanup_prompt_edit_btn",
        "cleanup_prompt_reset_btn", "text_model_picker",        "cleanup_reasoning_combo",
    };
    registerPersonalizePage(context, std::move(descriptor));
}

// --- Home ---

void addCleanupHomeRows(HomePage& home) {
    SettingsDialog& dialog = home.dialog();
    AppContext& context = dialog.context();
    CleanupSettings* cleanup = context.find<CleanupSettings>();
    home.toggle(
        HomeCardId::Dictation, QStringLiteral("Clean up my dictation"),
        QStringLiteral("Fix punctuation and formatting after recording."),
        std::string(destinations::cleanup), "transcript_cleanup_check",
        [](const Json& settings) {
            return QVariant(keys::transcript_cleanup_enabled.read(settings));
        },
        std::string(keys::transcript_cleanup_enabled.key()));

    // Not a bind projection: choosing a level can remove a custom prompt (after asking), which
    // CleanupSettings owns, and it must not build the AI cleanup page.
    ElidingComboBox* combo = home.combo();
    WrappedLabel* detail =
        home.addRow(HomeCardId::Dictation, QStringLiteral("Cleanup level"), QString(), combo);
    home.registerControl(std::string(keys::transcript_cleanup_level.key()), combo);
    auto* status = new WrappedLabel(QString());
    status->setObjectName(QStringLiteral("homeStatus"));
    home.addWidget(HomeCardId::Dictation, status);

    const auto refreshLevel = [combo, detail](const Json& settings) {
        const bool custom = !text::custom_prompt(settings).empty();
        const std::string choice = custom ? std::string(levels::CUSTOM) : savedLevel(settings);
        {
            const QSignalBlocker blocker(combo);
            combo->clear();
            for (const levels::Segment& segment : levels::SEGMENTS) {
                if (segment.value != levels::CUSTOM || custom) {
                    combo->addItem(q(segment.title), q(segment.value));
                }
            }
            combo->setCurrentIndex(std::max(0, combo->findData(q(choice))));
        }
        combo->setEnabled(text::resolve_level(settings) != text::CleanupLevel::None);
        detail->setText(q(levels::short_note(choice)));
    };
    QObject::connect(combo, &QComboBox::activated, &home,
                     [combo, cleanup, refreshLevel, &dialog](int index) {
                         const QString level = combo->itemData(index).toString();
                         if (cleanup != nullptr && level != q(levels::CUSTOM)) {
                             cleanup->chooseCleanupLevel(level, combo);
                         }
                         // A declined confirmation saves nothing; show the saved choice again.
                         refreshLevel(*dialog.settingsSnapshot());
                     });

    // Whether a provider's key is found is read off the GUI thread; the last answer per endpoint
    // shows until the next one arrives.
    struct KeyAnswers {
        std::map<std::string, bool> found;
        qt::TokenGate gate;
    };
    auto answers = std::make_shared<KeyAnswers>();
    QPointer<WrappedLabel> label(status);
    home.addRefreshHook([label, answers, refreshLevel, cleanup](const Json& settings) {
        refreshLevel(settings);
        if (label == nullptr) {
            return;
        }
        const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
        const std::optional<llm::TextLLMProfile> profile = llm::get_profile(provider, settings);
        const std::string key = profile ? profile->id + "|" + profile->api_key_env : std::string();
        std::optional<bool> found;
        if (const auto known = answers->found.find(key); known != answers->found.end()) {
            found = known->second;
        }
        label->setText(cleanupStatus(settings, found));
        if (!profile || !profile->requires_api_key() || cleanup == nullptr) {
            return;
        }
        CleanupServices& services = cleanup->services();
        if (services.io == nullptr || services.ui == nullptr || !services.credentialStatus) {
            return;
        }
        const std::uint64_t token = answers->gate.next();
        services.io->post([status = services.credentialStatus, ui = services.ui, profile = *profile,
                           label, answers, key, settings, token] {
            bool available = false;
            try {
                available = status(profile).available;
            } catch (const std::exception&) {
                return;
            }
            ui->post([label, answers, key, settings, token, available] {
                answers->found[key] = available;
                if (label != nullptr && answers->gate.isCurrent(token)) {
                    label->setText(cleanupStatus(settings, available));
                }
            });
        });
    });
    if (cleanup != nullptr) {
        cleanup->attach(dialog);
    }
}

} // namespace fv
