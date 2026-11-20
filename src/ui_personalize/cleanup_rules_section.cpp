#include "ui_personalize/cleanup_rules_section.h"

#include <algorithm>
#include <exception>
#include <typeinfo>
#include <utility>

#include <QDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QSize>
#include <QTimer>
#include <QVBoxLayout>

#include "appcore/mic_priority.h"
#include "appkit/app_context.h"
#include "base/executor.h"
#include "base/log.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "pyport/text.h"
#include "recorder/recorder.h"
#include "text/cleanup_config.h"
#include "text/cleanup_prompts.h"
#include "ui_personalize/cleanup_rule_dialog.h"
#include "ui_personalize/cleanup_settings.h"
#include "ui_personalize/personalize_page.h"
#include "ui_personalize/rule_activity.h"
#include "ui_personalize/rule_dictation.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/setting_tile.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.settings_cleanup_rules"};

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string u(const QString& text) {
    return qt::toUtf8(text);
}

QString folded(const QString& text) {
    return q(py::casefold(u(text)));
}

// Runs a level callback on the recorder's own thread; it only posts to the GUI executor.
class DirectExecutor final : public Executor {
public:
    void post(Task task) override { task(); }
    TimerHandle post_after(std::chrono::milliseconds, Task task) override {
        task();
        return {};
    }
    bool is_current() const override { return true; }
};

DirectExecutor& directExecutor() {
    static DirectExecutor executor;
    return executor;
}

constexpr const char* kDictateTip = "Speak the instruction instead of typing it";
constexpr const char* kStopTip = "Stop and transcribe · Esc throws the recording away";

} // namespace

const std::vector<std::string>& cleanupRulesControls() {
    static const std::vector<std::string> names{
        "cleanup_rules_composer_tile", "cleanup_rule_input",    "cleanup_rule_mic_btn",
        "cleanup_rule_add_btn",        "cleanup_rule_activity", "cleanup_rule_status",
        "cleanup_rules_library_tile",  "cleanup_rules_count",   "cleanup_rules_list",
        "cleanup_rules_empty",         "cleanup_rule_edit_btn", "cleanup_rule_delete_btn",
    };
    return names;
}

CleanupRulesSection::CleanupRulesSection(CleanupServices& services, Hooks hooks, QObject* parent)
    : QObject(parent), services_(services), hooks_(std::move(hooks)),
      dictationPath_(ruleDictationPath()) {
    dictationTimer_ = new QTimer(this);
    dictationTimer_->setSingleShot(true);
    dictationTimer_->setInterval(kRuleDictationCapMs);
    connect(dictationTimer_, &QTimer::timeout, this, &CleanupRulesSection::stopDictation);
    reviewRule = [](const RuleReview& review, QWidget* parent) -> std::optional<QString> {
        CleanupRuleDialog dialog(review.rule, review.original, review.notice, review.dictated,
                                 parent);
        if (dialog.exec() != QDialog::Accepted) {
            return std::nullopt;
        }
        return dialog.ruleText();
    };
}

CleanupRulesSection::~CleanupRulesSection() {
    levelSubscription_.disconnect();
    if (recorder_ != nullptr) {
        try {
            recorder_->cleanup();
        } catch (const std::exception& error) {
            kLog.warning("Couldn't release the rule recorder ({})",
                         type_display_name(typeid(error)));
        }
    }
}

// --- Construction ---

void CleanupRulesSection::build(PersonalizePage& page) {
    page_ = &page;
    composerTile_ = new InfoTile(
        QStringLiteral("Teach a new rule"),
        QStringLiteral("Type or dictate an instruction in your own words. Your AI cleanup model "
                       "rewrites it as a clear rule, and you review it before it's saved."),
        ui::designIcon(QStringLiteral("plus-blue.svg")));
    auto* inputRow = new QHBoxLayout;
    inputRow->setContentsMargins(0, 0, 0, 0);
    inputRow->setSpacing(8);
    ruleInput_ = new QLineEdit;
    ruleInput_->setObjectName(QStringLiteral("cleanupRuleInput"));
    ruleInput_->setMinimumHeight(40);
    ruleInput_->setPlaceholderText(QStringLiteral("Try: Always spell my name \"Alex Rivera\""));
    connect(ruleInput_, &QLineEdit::returnPressed, this, &CleanupRulesSection::addRule);
    connect(ruleInput_, &QLineEdit::textEdited, this, [this] { setNotice(QString()); });
    inputRow->addWidget(ruleInput_, 1);
    micButton_ = new Button(QStringLiteral("Dictate"));
    micButton_->setObjectName(QStringLiteral("cleanupRuleDictateButton"));
    // theme.qss swaps the icon (microphone / stop) on this property.
    ui::setStyleProperty(micButton_, ui::prop::recording, false);
    micButton_->setIconSize(QSize(16, 16));
    micButton_->setBaseMinimumSize(108, 40);
    micButton_->setToolTip(QString::fromUtf8(kDictateTip));
    connect(micButton_, &QPushButton::clicked, this, &CleanupRulesSection::toggleDictation);
    inputRow->addWidget(micButton_);
    addButton_ = new PrimaryButton(QStringLiteral("Add rule"));
    addButton_->setObjectName(QStringLiteral("cleanupRuleAddButton"));
    addButton_->setBaseMinimumSize(104, 40);
    connect(addButton_, &QPushButton::clicked, this, &CleanupRulesSection::addRule);
    inputRow->addWidget(addButton_);
    // No spacing between the input row and the activity strip: the strip carries its own gap
    // inside the height it animates, so it slides open from nothing.
    auto* composerColumn = new QVBoxLayout;
    composerColumn->setContentsMargins(0, 0, 0, 0);
    composerColumn->setSpacing(0);
    composerColumn->addLayout(inputRow);
    activity_ = new RuleActivityStrip(kRuleDictationCapMs / 1000);
    composerColumn->addWidget(activity_);
    composerTile_->addBodyLayout(composerColumn);
    // Problems only; progress is narrated by the activity strip.
    status_ = new QLabel(QString());
    status_->setObjectName(QStringLiteral("cleanupRuleStatus"));
    status_->setWordWrap(true);
    status_->hide();
    composerTile_->addBody(status_);
    page.addTileGroup(QString(), {composerTile_}, 1);

    libraryTile_ = new InfoTile(
        QStringLiteral("Your rules"),
        QStringLiteral("Select a rule or double-click it to edit. Every rule applies to AI "
                       "cleanup, and to profiles set to also apply learned rules."),
        ui::designIcon(QStringLiteral("stack-slate.svg")));
    count_ = new QLabel;
    count_->setObjectName(QStringLiteral("cleanupRulesCount"));
    count_->setAlignment(Qt::AlignCenter);
    libraryTile_->addTrailing(count_);

    rulesList_ = new QListWidget;
    rulesList_->setObjectName(QStringLiteral("cleanupRulesList"));
    rulesList_->setWordWrap(true);
    rulesList_->setSpacing(6);
    rulesList_->setMinimumHeight(88);
    connect(rulesList_, &QListWidget::itemSelectionChanged, this,
            &CleanupRulesSection::updateControls);
    connect(rulesList_, &QListWidget::itemDoubleClicked, this, [this] { editRule(); });
    libraryTile_->addBody(rulesList_);
    glow_ = new ItemGlow(rulesList_);

    empty_ = new EmptyState(QStringLiteral("No rules yet"),
                            QStringLiteral("Add your first instruction above to start building a "
                                           "personal cleanup profile."));
    libraryTile_->addBody(empty_);

    auto* buttonRow = new QHBoxLayout;
    buttonRow->setContentsMargins(0, 0, 0, 0);
    buttonRow->setSpacing(8);
    buttonRow->addStretch();
    editButton_ = neutralButton(new Button(QStringLiteral("Edit rule")));
    editButton_->setObjectName(QStringLiteral("cleanupRuleEditButton"));
    editButton_->setBaseMinimumSize(96, 34);
    connect(editButton_, &QPushButton::clicked, this, &CleanupRulesSection::editRule);
    buttonRow->addWidget(editButton_);
    deleteButton_ = neutralButton(new Button(QStringLiteral("Delete")));
    deleteButton_->setObjectName(QStringLiteral("cleanupRuleDeleteButton"));
    deleteButton_->setBaseMinimumSize(88, 34);
    connect(deleteButton_, &QPushButton::clicked, this, &CleanupRulesSection::deleteRule);
    buttonRow->addWidget(deleteButton_);
    libraryTile_->addBodyLayout(buttonRow);
    page.addTileGroup(QString(), {libraryTile_}, 1);
}

void CleanupRulesSection::load(const Json& settings) {
    rulesList_->clear();
    for (const std::string& rule : text::resolve_transcript_cleanup_rules(settings)) {
        rulesList_->addItem(q(rule));
    }
}

QWidget* CleanupRulesSection::control(std::string_view name) const {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"cleanup_rules_composer_tile", composerTile_},
        {"cleanup_rule_input", ruleInput_},
        {"cleanup_rule_mic_btn", micButton_},
        {"cleanup_rule_add_btn", addButton_},
        {"cleanup_rule_activity", activity_},
        {"cleanup_rule_status", status_},
        {"cleanup_rules_library_tile", libraryTile_},
        {"cleanup_rules_count", count_},
        {"cleanup_rules_list", rulesList_},
        {"cleanup_rules_empty", empty_},
        {"cleanup_rule_edit_btn", editButton_},
        {"cleanup_rule_delete_btn", deleteButton_},
    };
    for (const auto& [key, widget] : controls) {
        if (key == name) {
            return widget;
        }
    }
    return nullptr;
}

// --- The library ---

std::vector<std::string> CleanupRulesSection::stagedRules() const {
    std::vector<std::string> rules;
    for (int i = 0; i < rulesList_->count(); ++i) {
        rules.push_back(u(rulesList_->item(i)->text()));
    }
    return rules;
}

void CleanupRulesSection::saveRules() {
    if (hooks_.persistRules) {
        hooks_.persistRules(stagedRules());
    }
}

bool CleanupRulesSection::alreadyListed(const QString& rule) const {
    const QString key = folded(rule);
    for (int i = 0; i < rulesList_->count(); ++i) {
        if (folded(rulesList_->item(i)->text()) == key) {
            return true;
        }
    }
    return false;
}

void CleanupRulesSection::updateControls() {
    const bool enabled = hooks_.cleanupEnabled && hooks_.cleanupEnabled();
    const bool busy = polishing_ || dictationState_ != DictationState::Idle;
    const int ruleCount = rulesList_->count();
    count_->setText(QStringLiteral("%1 / %2").arg(ruleCount).arg(
        static_cast<qulonglong>(text::config::MAX_TRANSCRIPT_CLEANUP_RULES)));
    rulesList_->setVisible(ruleCount > 0);
    empty_->setVisible(ruleCount == 0);
    empty_->setEnabled(enabled);
    ruleInput_->setEnabled(enabled && !busy);
    addButton_->setEnabled(enabled && !busy);
    micButton_->setEnabled(enabled && !polishing_ &&
                           dictationState_ != DictationState::Transcribing);
    const bool hasSelection = !rulesList_->selectedItems().isEmpty();
    editButton_->setEnabled(enabled && hasSelection);
    deleteButton_->setEnabled(enabled && hasSelection);
}

void CleanupRulesSection::setNotice(const QString& text) {
    status_->setText(text);
    status_->setVisible(!text.isEmpty());
}

void CleanupRulesSection::setMicRecording(bool recording) {
    ui::setStyleProperty(micButton_, ui::prop::recording, recording);
    micButton_->setText(recording ? QStringLiteral("Stop") : QStringLiteral("Dictate"));
    micButton_->setToolTip(QString::fromUtf8(recording ? kStopTip : kDictateTip));
}

QString CleanupRulesSection::polishDetail(const std::string& provider, const std::string& model,
                                          const Json& settings) const {
    // The endpoint and model that will rewrite the words.
    QStringList parts;
    const std::string name = llm::profile_display_name(provider, settings);
    for (const std::string* part : {&name, &model}) {
        if (!part->empty()) {
            parts << q(*part);
        }
    }
    return parts.join(QStringLiteral(u" · "));
}

QWidget* CleanupRulesSection::dialogParent() const {
    return page_ != nullptr ? page_->window() : nullptr;
}

void CleanupRulesSection::addRule() {
    polishRule(ruleInput_->text());
}

void CleanupRulesSection::polishRule(const QString& given, bool dictated) {
    const QString raw = given.trimmed();
    if (polishing_ || dictationState_ != DictationState::Idle) {
        return;
    }
    if (raw.isEmpty()) {
        activity_->finish();
        return;
    }
    if (alreadyListed(raw)) {
        activity_->finish();
        setNotice(QStringLiteral("That rule already exists."));
        return;
    }
    if (static_cast<std::size_t>(rulesList_->count()) >=
        text::config::MAX_TRANSCRIPT_CLEANUP_RULES) {
        activity_->finish();
        setNotice(QStringLiteral("Rule limit reached (%1).")
                      .arg(static_cast<qulonglong>(text::config::MAX_TRANSCRIPT_CLEANUP_RULES)));
        return;
    }

    polishing_ = true;
    polishDictated_ = dictated;
    setNotice(QString());

    const Json settings = hooks_.loadSettings ? hooks_.loadSettings() : Json::object();
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    const std::string model = llm::resolve_transcript_cleanup_model(settings);
    const std::string reasoning = llm::resolve_transcript_cleanup_reasoning(settings);
    activity_->showPolishing(polishDetail(provider, model, settings));
    updateControls();
    const std::uint64_t token = polishGate_.next();
    QPointer<CleanupRulesSection> self(this);
    const auto finished = [self, gate = polishGate_](std::uint64_t answered, QString rawText,
                                                     QString polished, QString error) {
        if (self != nullptr && gate.isCurrent(answered)) {
            self->onPolished(answered, rawText, polished, error);
        }
    };
    if (services_.jobs == nullptr || services_.ui == nullptr || !services_.polishRule) {
        // No model can be reached from here: the words are kept as given.
        QTimer::singleShot(0, this, [finished, token, raw] {
            finished(token, raw, raw, QStringLiteral("cleanup unavailable"));
        });
        return;
    }
    services_.jobs->post([ui = services_.ui, polish = services_.polishRule, finished, token, raw,
                          provider, model, reasoning] {
        QString polished;
        QString error;
        try {
            auto [text, failure] = polish(u(raw), provider, model, reasoning);
            polished = q(text);
            error = q(failure.value_or(std::string()));
        } catch (const std::exception& failure) {
            // Type only: the error could quote the rule or the prompt.
            kLog.warning("Polishing a cleanup rule failed ({}); keeping it unpolished",
                         type_display_name(typeid(failure)));
            polished = raw;
            error = q(failure.what());
        }
        if (polished.isEmpty()) {
            polished = raw;
        }
        ui->post(
            [finished, token, raw, polished, error] { finished(token, raw, polished, error); });
    });
}

void CleanupRulesSection::onPolished(std::uint64_t, const QString& raw, const QString& polished,
                                     const QString& error) {
    polishing_ = false;
    const bool dictated = std::exchange(polishDictated_, false);
    activity_->finish();
    updateControls();
    if (hooks_.isVisible && !hooks_.isVisible()) {
        // Settings closed while the model worked. The words are still in the input for next
        // time; a review dialog now would pop up alone.
        return;
    }
    QString notice;
    if (!error.isEmpty()) {
        notice = dictated ? QStringLiteral(u"AI polish unavailable — your words will be saved as "
                                           u"transcribed.")
                          : QStringLiteral(u"AI polish unavailable — your wording will be saved as "
                                           u"written.");
    }
    const std::optional<QString> reviewed =
        reviewRule ? reviewRule(RuleReview{polished, raw, notice, dictated}, dialogParent())
                   : std::nullopt;
    if (!reviewed || reviewed->isEmpty()) {
        return;
    }
    const QString rule = *reviewed;
    if (alreadyListed(rule)) {
        setNotice(QStringLiteral("That rule already exists."));
        return;
    }
    rulesList_->addItem(rule);
    QListWidgetItem* item = rulesList_->item(rulesList_->count() - 1);
    ruleInput_->clear();
    updateControls();
    saveRules();
    glow_->flash(item);
}

void CleanupRulesSection::editRule() {
    const QList<QListWidgetItem*> items = rulesList_->selectedItems();
    if (items.isEmpty()) {
        return;
    }
    QListWidgetItem* item = items.front();
    const std::optional<QString> reviewed =
        reviewRule
            ? reviewRule(RuleReview{item->text(), std::nullopt, QString(), false}, dialogParent())
            : std::nullopt;
    if (reviewed && !reviewed->isEmpty()) {
        item->setText(*reviewed);
        saveRules();
    }
}

void CleanupRulesSection::deleteRule() {
    for (QListWidgetItem* item : rulesList_->selectedItems()) {
        delete rulesList_->takeItem(rulesList_->row(item));
    }
    updateControls();
    saveRules();
}

// --- Dictation ---

void CleanupRulesSection::toggleDictation() {
    if (dictationState_ == DictationState::Recording) {
        stopDictation();
        return;
    }
    if (dictationState_ != DictationState::Idle || polishing_) {
        return;
    }
    // Without a worker to transcribe on, a take could never be stopped and transcribed: it is
    // not started, so no microphone opens for nothing.
    if (!hooks_.transcriber || hooks_.transcriber() == nullptr || !services_.makeRecorder ||
        services_.jobs == nullptr || services_.ui == nullptr) {
        setNotice(QStringLiteral("Dictation is unavailable."));
        return;
    }
    if (services_.meetingActive && services_.meetingActive()) {
        setNotice(QStringLiteral(u"Meeting Mode is active — end the meeting to dictate a rule."));
        return;
    }

    const Json settings = hooks_.settingsSnapshot ? hooks_.settingsSnapshot() : Json::object();
    const std::vector<audio::DeviceKey> priority = appcore::mic::saved_priority(settings);
    if (recorder_ == nullptr || recorderDevice_ != priority) {
        levelSubscription_.disconnect();
        if (recorder_ != nullptr) {
            recorder_->cleanup();
        }
        recorder_ = services_.makeRecorder(priority, dictationPath_);
        if (recorder_ == nullptr) {
            setNotice(QStringLiteral("Dictation is unavailable."));
            return;
        }
        // Levels come from the audio thread, which must not reach back into the window; the
        // GUI executor hands them to the composer's trace.
        const std::uint64_t token = levelGate_.next();
        if (services_.ui != nullptr) {
            QPointer<CleanupRulesSection> self(this);
            levelSubscription_ = recorder_->level.connect(
                directExecutor(), [self, ui = services_.ui, token](float value) {
                    ui->post([self, token, value] {
                        if (self != nullptr) {
                            self->onDictationLevel(token, value);
                        }
                    });
                });
        }
        recorderDevice_ = priority;
    }

    if (!recorder_->start_recording()) {
        const std::string reason = recorder_->last_start_error();
        setNotice(reason.empty() ? QStringLiteral("Couldn't start recording.")
                                 : QStringLiteral("Couldn't start recording: %1.").arg(q(reason)));
        return;
    }
    dictationState_ = DictationState::Recording;
    setNotice(QString());
    setMicRecording(true);
    activity_->showListening(q(appcore::mic::preferred_name(settings)));
    dictationTimer_->start();
    updateControls();
    if (hooks_.setEscapeOverride) {
        QPointer<CleanupRulesSection> self(this);
        hooks_.setEscapeOverride([self] {
            if (self != nullptr) {
                self->cancelDictation();
            }
        });
    }
}

void CleanupRulesSection::onDictationLevel(std::uint64_t token, double level) {
    if (levelGate_.isCurrent(token)) {
        activity_->pushLevel(level);
    }
}

void CleanupRulesSection::cancelDictation() {
    if (dictationState_ != DictationState::Recording) {
        return;
    }
    dictationTimer_->stop();
    if (recorder_ != nullptr) {
        recorder_->cancel_recording();
    }
    dictationState_ = DictationState::Idle;
    setMicRecording(false);
    activity_->finish();
    updateControls();
    if (hooks_.setEscapeOverride) {
        hooks_.setEscapeOverride({});
    }
}

void CleanupRulesSection::stopDictation() {
    if (dictationState_ != DictationState::Recording) {
        return;
    }
    dictationTimer_->stop();
    dictationState_ = DictationState::Transcribing;
    setMicRecording(false);
    activity_->showTranscribing(hooks_.transcribeDetail ? hooks_.transcribeDetail() : QString());
    updateControls();
    if (hooks_.setEscapeOverride) {
        hooks_.setEscapeOverride({});
    }

    const std::shared_ptr<Recorder> recorder = recorder_;
    RuleTranscriber* transcriber = hooks_.transcriber ? hooks_.transcriber() : nullptr;
    const std::uint64_t token = dictationGate_.next();
    QPointer<CleanupRulesSection> self(this);
    const auto finished = [self, gate = dictationGate_](std::uint64_t answered, QString text,
                                                        QString error) {
        if (self != nullptr && gate.isCurrent(answered)) {
            self->onDictationFinished(answered, text, error);
        }
    };
    if (services_.jobs == nullptr || services_.ui == nullptr || transcriber == nullptr ||
        recorder == nullptr) {
        // Nothing can transcribe the take (the transcriber went while it recorded): it is thrown
        // away, so the microphone does not stay open.
        if (recorder != nullptr) {
            try {
                recorder->cancel_recording();
            } catch (const std::exception& error) {
                kLog.warning("Couldn't discard the rule dictation ({})",
                             type_display_name(typeid(error)));
            }
        }
        QTimer::singleShot(0, this, [finished, token] {
            finished(token, QString(), QStringLiteral("Dictation is unavailable."));
        });
        return;
    }
    services_.jobs->post(
        [ui = services_.ui, recorder, transcriber, path = dictationPath_, finished, token] {
            const auto [text, error] = transcribeRuleDictation(
                *recorder,
                [transcriber](const std::filesystem::path& audio) {
                    return transcriber->transcribeClip(audio);
                },
                path);
            ui->post([finished, token, text = q(text).trimmed(), error = q(error)] {
                finished(token, text, error);
            });
        });
}

void CleanupRulesSection::onDictationFinished(std::uint64_t, const QString& text,
                                              const QString& error) {
    dictationState_ = DictationState::Idle;
    updateControls();
    if (!error.isEmpty()) {
        activity_->finish();
        setNotice(error);
        return;
    }
    const QString current = ruleInput_->text().trimmed();
    const QString raw = current.isEmpty() ? text : (current + QLatin1Char(' ') + text).trimmed();
    // The words land in the input first, so a cancelled review (or a Settings window closed
    // mid-polish) keeps them for editing.
    ruleInput_->setText(raw);
    if (hooks_.isVisible && !hooks_.isVisible()) {
        activity_->finish(false);
        return;
    }
    polishRule(raw, true);
}

void CleanupRulesSection::releaseRecorder() {
    dictationTimer_->stop();
    if (dictationState_ == DictationState::Recording) {
        // Settings is reused when it reopens, so a take still recording at close is discarded
        // here rather than left showing "Stop".
        dictationState_ = DictationState::Idle;
        setMicRecording(false);
        activity_->finish(false);
        updateControls();
        if (hooks_.setEscapeOverride) {
            hooks_.setEscapeOverride({});
        }
    }
    levelSubscription_.disconnect();
    if (recorder_ != nullptr) {
        recorder_->cleanup();
        recorder_.reset();
    }
    recorderDevice_.clear();
}

} // namespace fv
