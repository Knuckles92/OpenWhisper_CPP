#include "ui_automation/hooks_editors.h"

#include <algorithm>
#include <utility>

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>

#include "automation/catalog.h"
#include "automation/secrets.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/text.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/hooks_format.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/segmented_bar.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.hooks_panel"};

namespace hooks = automation::hooks;
using hooks_panel::fieldLabel;
using hooks_panel::hint;
using hooks_panel::smallButton;

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string text(const QString& value) {
    return qt::toUtf8(value);
}

const Clock& clockOf(const HookServices& services) {
    return services.clock != nullptr ? *services.clock : system_clock();
}

// A row of an editor's inline field: caption, the field, and the room after it (object name
// "hooksInlineField").
QWidget* inlineField(const QString& caption, QWidget* field) {
    auto* limit = new QWidget;
    limit->setObjectName(QStringLiteral("hooksInlineField"));
    auto* row = new QHBoxLayout(limit);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(10);
    row->addWidget(fieldLabel(caption, field));
    row->addWidget(field);
    row->addStretch(1);
    return limit;
}

// The part of an editor under its fields that exists for a saved item: a rule, a heading, the
// test button with its chip and error line, and the recent list.
struct Extras {
    QWidget* box = nullptr;
    QVBoxLayout* layout = nullptr;
};

Extras makeExtras() {
    Extras extras;
    extras.box = new QWidget;
    extras.box->setObjectName(QStringLiteral("hooksExtras"));
    extras.layout = new QVBoxLayout(extras.box);
    extras.layout->setContentsMargins(0, 6, 0, 0);
    extras.layout->setSpacing(6);
    return extras;
}

std::vector<std::string> splitLines(const QString& plain) {
    std::vector<std::string> lines;
    for (const QString& line : plain.split(QLatin1Char('\n'))) {
        const std::string stripped = py::strip(text(line));
        if (!stripped.empty()) {
            lines.push_back(stripped);
        }
    }
    return lines;
}

QString joinLines(const std::vector<std::string>& lines) {
    QStringList joined;
    for (const std::string& line : lines) {
        joined << q(line);
    }
    return joined.join(QLatin1Char('\n'));
}

} // namespace

// ---- the common editor ----

HookEditor::HookEditor(const QString& noun, const QString& namePlaceholder,
                       const HookServices& services, QWidget* parent)
    : QFrame(parent), services_(services), noun_(noun), requests_(this) {
    setObjectName(QStringLiteral("hooksEditor"));
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(16, 14, 16, 14);
    column_->setSpacing(6);
    nameEdit_ = new QLineEdit;
    nameEdit_->setMaxLength(static_cast<int>(hooks::NAME_MAX_CHARS));
    nameEdit_->setPlaceholderText(namePlaceholder);
    nameEdit_->setAccessibleName(
        QStringLiteral("%1%2 name").arg(noun.left(1).toUpper(), noun.mid(1)));
    column_->addWidget(fieldLabel(QStringLiteral("Name"), nameEdit_));
    column_->addWidget(nameEdit_);
}

void HookEditor::finish() {
    message_ = new WrappedLabel(QString());
    message_->setObjectName(QStringLiteral("hooksMessage"));
    message_->setTextFormat(Qt::PlainText);
    message_->hide();
    column_->addSpacing(4);
    column_->addWidget(message_);
    deleteButton_ = smallButton(QStringLiteral("Delete"));
    deleteButton_->setAccessibleName(QStringLiteral("Delete this %1").arg(noun_));
    connect(deleteButton_, &QPushButton::clicked, this, [this] { Q_EMIT deleteRequested(); });
    cancelButton_ = smallButton(QStringLiteral("Cancel"));
    cancelButton_->setToolTip(QStringLiteral("Close without saving"));
    connect(cancelButton_, &QPushButton::clicked, this, [this] { Q_EMIT cancelRequested(); });
    saveButton_ = smallButton(QStringLiteral("Save %1").arg(noun_), true);
    connect(saveButton_, &QPushButton::clicked, this, [this] { Q_EMIT saveRequested(); });
    footer_ = new AdaptiveButtons({deleteButton_, nullptr, cancelButton_, saveButton_});
    column_->addWidget(footer_);
    // Read while the editor stands alone, so its chain is its own: the fields were added top to
    // bottom. The section places them in the page.
    tabStops_ = hooks_panel::tabStops(this);
    connect(nameEdit_, &QLineEdit::textChanged, this, [this] { draftEdited(); });
}

void HookEditor::draftEdited() {
    // "Saved." stays true only until the fields differ from what was saved.
    if (message_->property("tone").toString() == QStringLiteral("ok") && !message_->isHidden() &&
        hasUnsavedChanges()) {
        setMessage(QString());
    }
}

void HookEditor::begin(bool saved) {
    startOver();
    saved_ = saved;
}

void HookEditor::end() {
    deleteButton_->setVisible(saved_);
    setMessage(QString());
    refreshLive();
}

void HookEditor::startOver() {
    // One editor serves every item, and a test or secret still on its way was asked for what it
    // showed before (or before a save changed it), so its answer is dropped rather than shown,
    // or copied, here.
    requests_.forget();
    showIdle();
}

void HookEditor::forgetShown() {
    startOver();
    saved_ = false;
}

void HookEditor::setMessage(const QString& text, bool error) {
    message_->setText(text);
    message_->setVisible(!text.isEmpty());
    ui::setStyleProperty(message_, ui::prop::tone,
                         error ? QStringLiteral("error") : QStringLiteral("ok"));
}

QLabel* HookEditor::sectionLabel(const QString& label) const {
    auto* widget = new QLabel(label);
    widget->setObjectName(QStringLiteral("hooksSectionLabel"));
    return widget;
}

QFrame* HookEditor::divider() const {
    auto* line = new QFrame;
    line->setObjectName(QStringLiteral("hooksDivider"));
    line->setFixedHeight(1);
    return line;
}

// ---- program fields ----

void ProgramFields::buildProgramFields(QWidget* host, QVBoxLayout* layout, const QString& noun) {
    programEdit_ = new QLineEdit;
    programEdit_->setPlaceholderText(QStringLiteral("Choose a program on this computer"));
    programEdit_->setAccessibleName(QStringLiteral("%1 program").arg(noun));
    chooseButton_ = smallButton(QStringLiteral("Choose…"));
    chooseButton_->setAccessibleName(QStringLiteral("Choose a program"));
    QObject::connect(chooseButton_, &QPushButton::clicked, host,
                     [this, host] { chooseProgram(host); });
    programLabel_ = fieldLabel(QStringLiteral("Program"), programEdit_);
    layout->addWidget(programLabel_);
    programEdit_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    programRow_ = new AdaptiveButtons({programEdit_, chooseButton_});
    layout->addWidget(programRow_);
    argsEdit_ = new QPlainTextEdit;
    argsEdit_->setObjectName(QStringLiteral("hooksArgs"));
    argsEdit_->setTabChangesFocus(true);
    argsEdit_->setPlaceholderText(QStringLiteral("One per line, e.g. --quiet"));
    argsEdit_->setAccessibleName(QStringLiteral("Program arguments, one per line"));
    argsEdit_->setFixedHeight(ui::scaled(64));
    argsLabel_ = fieldLabel(QStringLiteral("Arguments (optional)"), argsEdit_);
    layout->addWidget(argsLabel_);
    layout->addWidget(argsEdit_);
}

std::vector<std::string> ProgramFields::args() const {
    return splitLines(argsEdit_->toPlainText());
}

void ProgramFields::chooseProgram(QWidget* parent) {
    const QString current = programEdit_->text().trimmed();
    QString start = QDir::homePath();
    if (!current.isEmpty()) {
        start = QFileInfo(current).path();
        if (start == QStringLiteral(".")) {
            start.clear();
        }
    }
    const QString path =
        action_fields::chooseProgram()(parent, QStringLiteral("Choose a program"), start);
    if (!path.isEmpty()) {
        programEdit_->setText(QDir::toNativeSeparators(QFileInfo(path).absoluteFilePath()));
    }
}

// ---- webhooks ----

WebhookEditor::WebhookEditor(const HookServices& services, QWidget* parent)
    : HookEditor(QStringLiteral("webhook"), QStringLiteral("e.g. Home automation"), services,
                 parent) {
    urlEdit_ = new QLineEdit;
    urlEdit_->setMaxLength(static_cast<int>(hooks::URL_MAX_CHARS));
    urlEdit_->setPlaceholderText(q("https://example.com/" + py::lower(identity().product)));
    urlEdit_->setAccessibleName(QStringLiteral("Webhook address"));
    connect(urlEdit_, &QLineEdit::textChanged, this, [this] { updateNote(); });
    column()->addSpacing(4);
    column()->addWidget(fieldLabel(QStringLiteral("Web address"), urlEdit_));
    column()->addWidget(urlEdit_);
    urlNote_ = hint();
    ui::setStyleProperty(urlNote_, ui::prop::tone, QStringLiteral("warning"));
    urlNote_->hide();
    column()->addWidget(urlNote_);
    events_ = new EventChooser;
    column()->addSpacing(4);
    column()->addWidget(fieldLabel(QStringLiteral("Events to send"), events_));
    column()->addWidget(events_);

    Extras extras = makeExtras();
    extras_ = extras.box;
    QVBoxLayout* box = extras.layout;
    box->addWidget(divider());
    box->addWidget(sectionLabel(QStringLiteral("Signing secret")));
    box->addWidget(hint(QStringLiteral("Each delivery is signed. Your receiver can check the "
                                       "webhook-signature header with this secret.")));
    copySecret_ = smallButton(QStringLiteral("Copy secret"));
    connect(copySecret_, &QPushButton::clicked, this, [this] { copySecret(); });
    rotateSecret_ = smallButton(QStringLiteral("Make a new one"));
    rotateSecret_->setToolTip(QStringLiteral("Replace the secret; receivers need the new one"));
    connect(rotateSecret_, &QPushButton::clicked, this, [this] { rotateSecret(); });
    secretChip_ = new StatusChip;
    box->addWidget(new AdaptiveButtons({copySecret_, rotateSecret_, secretChip_, nullptr}));
    box->addWidget(secretChip_->errorLine());
    box->addSpacing(4);
    box->addWidget(divider());
    box->addWidget(sectionLabel(QStringLiteral("Test and recent deliveries")));
    testButton_ = smallButton(QStringLiteral("Send test"));
    testButton_->setToolTip(QStringLiteral("Send a test event to this address now"));
    connect(testButton_, &QPushButton::clicked, this, [this] { sendTest(); });
    testChip_ = new StatusChip;
    box->addWidget(new AdaptiveButtons({testButton_, testChip_, nullptr}));
    box->addWidget(testChip_->errorLine());
    recent_ = new RecentList(QStringLiteral("No deliveries yet."));
    box->addWidget(recent_);
    column()->addWidget(extras_);

    finish();
    connect(urlEdit_, &QLineEdit::textChanged, this, [this] { draftEdited(); });
    connect(events_, &EventChooser::changed, this, [this] { draftEdited(); });
}

void WebhookEditor::updateNote() {
    const QString note = q(hooks_format::unencrypted_note(text(urlEdit_->text())));
    urlNote_->setText(note);
    urlNote_->setVisible(!note.isEmpty());
}

QString WebhookEditor::itemId() const {
    return source_ ? q(source_->id) : QString();
}

bool WebhookEditor::hasUnsavedChanges() const {
    return source_ && (!baseline_ || !(draft() == *baseline_));
}

hooks::Webhook WebhookEditor::draft() const {
    hooks::Webhook hook;
    hook.id = source_->id;
    hook.name = py::strip(text(nameEdit()->text()));
    hook.url = py::strip(text(urlEdit_->text()));
    hook.events = events_->events();
    hook.enabled = source_->enabled;
    return hook;
}

void WebhookEditor::showItem(const hooks::Webhook& hook, bool saved) {
    begin(saved);
    source_ = hook;
    nameEdit()->setText(q(hook.name));
    urlEdit_->setText(q(hook.url));
    events_->setEvents(hook.events);
    extras_->setVisible(saved);
    updateNote();
    // Compared through the editor, so text it trims never reads as an edit.
    baseline_ = draft();
    end();
}

void WebhookEditor::setSourceEnabled(bool enabled) {
    // The row's switch changed the saved item; keep the draft in step.
    if (source_) {
        source_->enabled = enabled;
        if (baseline_) {
            baseline_->enabled = enabled;
        }
    }
}

void WebhookEditor::clear() {
    forgetShown();
    source_.reset();
    baseline_.reset();
}

void WebhookEditor::showIdle() {
    const bool secrets = services().webhooks != nullptr && services().io != nullptr;
    copySecret_->setEnabled(secrets);
    rotateSecret_->setEnabled(secrets);
    testButton_->setEnabled(services().webhooks != nullptr);
    secretChip_->showResult(QString(), QStringLiteral("muted"));
    testChip_->showResult(QString(), QStringLiteral("muted"));
}

void WebhookEditor::refreshLive() {
    if (!isSaved() || !source_) {
        return;
    }
    std::vector<RecentRow> rows;
    if (services().webhooks != nullptr) {
        try {
            for (const automation::DeliveryRecord& record :
                 services().webhooks->deliveries(source_->id)) {
                const hooks_format::Outcome outcome = hooks_format::delivery_result(record);
                rows.push_back({q(hooks_format::clock(record.at, clockOf(services()))),
                                q(hooks_format::event_label(record.event_type)), q(outcome.text),
                                outcome.ok});
            }
        } catch (const std::exception&) {
            // Polled by a timer: debug only, the editor just shows no rows.
            kLog.debug("Could not read recent webhook deliveries");
            rows.clear();
        }
    }
    recent_->setRows(rows);
}

void WebhookEditor::copySecret() {
    automation::WebhookControl* engine = services().webhooks;
    if (engine == nullptr || services().io == nullptr) {
        return;
    }
    const std::string hookId = text(itemId());
    copySecret_->setEnabled(false);
    secretChip_->showResult(QStringLiteral("Getting the secret…"), QStringLiteral("busy"));
    requests().inBackground<std::string>(
        *services().io, [engine, hookId] { return engine->ensure_secret(hookId); },
        [this](std::optional<std::string> secret, QString error) {
            copySecret_->setEnabled(true);
            if (!error.isEmpty() || !secret || secret->empty()) {
                secretChip_->showError(error.isEmpty() ? QStringLiteral("No secret yet.") : error);
                return;
            }
            action_prompts::copyText()(q(*secret));
            secretChip_->showResult(QStringLiteral("Copied"), QStringLiteral("ok"));
        });
}

void WebhookEditor::rotateSecret() {
    automation::WebhookControl* engine = services().webhooks;
    if (engine == nullptr || services().io == nullptr) {
        return;
    }
    const auto answer = action_prompts::ask(
        this, QStringLiteral("Make a new signing secret?"),
        QStringLiteral("Your receiver will reject deliveries until you give it the new secret."),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    const std::string hookId = text(itemId());
    rotateSecret_->setEnabled(false);
    secretChip_->showResult(QStringLiteral("Making a new secret…"), QStringLiteral("busy"));
    requests().inBackground<std::string>(
        *services().io, [engine, hookId] { return engine->rotate_secret(hookId); },
        [this](std::optional<std::string> secret, QString error) {
            rotateSecret_->setEnabled(true);
            if (!error.isEmpty() || !secret || secret->empty()) {
                secretChip_->showError(
                    error.isEmpty() ? QStringLiteral("Couldn't make a new secret.") : error);
                return;
            }
            action_prompts::copyText()(q(*secret));
            secretChip_->showResult(QStringLiteral("New secret copied"), QStringLiteral("ok"));
        });
}

void WebhookEditor::sendTest() {
    automation::WebhookControl* engine = services().webhooks;
    if (engine == nullptr) {
        return;
    }
    if (hasUnsavedChanges() && !(saveFirst && saveFirst())) {
        return;
    }
    const std::string hookId = text(itemId());
    testButton_->setEnabled(false);
    testChip_->showResult(QStringLiteral("Sending…"), QStringLiteral("busy"));
    requests().whenDone<automation::DeliveryRecord>(
        [engine, hookId](std::function<void(automation::DeliveryRecord)> report) {
            engine->send_test(hookId, std::move(report));
        },
        [this](std::optional<automation::DeliveryRecord> record, QString error) {
            testButton_->setEnabled(true);
            if (!error.isEmpty() || !record) {
                testChip_->showError(error);
            } else {
                const hooks_format::Outcome outcome = hooks_format::delivery_result(*record);
                // A status code fits the chip; no status means a sentence saying why.
                if (!record->status) {
                    testChip_->showError(q(outcome.text));
                } else {
                    testChip_->showResult(q(outcome.text), outcome.ok ? QStringLiteral("ok")
                                                                      : QStringLiteral("error"));
                }
            }
            refreshLive();
        });
}

// ---- programs ----

ProgramEditor::ProgramEditor(const HookServices& services, QWidget* parent)
    : HookEditor(QStringLiteral("program"), QStringLiteral("e.g. Log my dictations"), services,
                 parent) {
    column()->addSpacing(4);
    buildProgramFields(this, column(), QStringLiteral("Hook"));
    events_ = new EventChooser;
    column()->addSpacing(4);
    column()->addWidget(fieldLabel(QStringLiteral("Events"), events_));
    column()->addWidget(events_);
    column()->addSpacing(4);
    timeout_ = new NoWheelSpinBox;
    timeout_->setRange(1, static_cast<int>(hooks::PROGRAM_TIMEOUT_MAX_S));
    timeout_->setSuffix(QStringLiteral(" s"));
    timeout_->setAccessibleName(QStringLiteral("Time limit in seconds"));
    column()->addWidget(inlineField(QStringLiteral("Time limit"), timeout_));
    column()->addWidget(
        hint(QStringLiteral("Each event arrives as JSON on the program's input. It runs directly, "
                            "never through a shell, and is stopped with anything it started after "
                            "the time limit.")));

    Extras extras = makeExtras();
    extras_ = extras.box;
    QVBoxLayout* box = extras.layout;
    box->addWidget(divider());
    box->addWidget(sectionLabel(QStringLiteral("Test and recent runs")));
    testButton_ = smallButton(QStringLiteral("Run test"));
    testButton_->setToolTip(QStringLiteral("Run the program now with a test event"));
    connect(testButton_, &QPushButton::clicked, this, [this] { runTest(); });
    testChip_ = new StatusChip;
    box->addWidget(new AdaptiveButtons({testButton_, testChip_, nullptr}));
    box->addWidget(testChip_->errorLine());
    recent_ = new RecentList(QStringLiteral("No runs yet."));
    box->addWidget(recent_);
    column()->addWidget(extras_);

    finish();
    connect(programEdit_, &QLineEdit::textChanged, this, [this] { draftEdited(); });
    connect(argsEdit_, &QPlainTextEdit::textChanged, this, [this] { draftEdited(); });
    connect(events_, &EventChooser::changed, this, [this] { draftEdited(); });
    connect(timeout_, &QSpinBox::valueChanged, this, [this] { draftEdited(); });
}

QString ProgramEditor::itemId() const {
    return source_ ? q(source_->id) : QString();
}

bool ProgramEditor::hasUnsavedChanges() const {
    return source_ && (!baseline_ || !(draft() == *baseline_));
}

hooks::Program ProgramEditor::draft() const {
    hooks::Program program;
    program.id = source_->id;
    program.name = py::strip(text(nameEdit()->text()));
    program.program = py::strip(text(programEdit_->text()));
    program.args = args();
    program.events = events_->events();
    program.enabled = source_->enabled;
    program.timeout_s = timeout_->value();
    return program;
}

void ProgramEditor::showItem(const hooks::Program& program, bool saved) {
    begin(saved);
    source_ = program;
    nameEdit()->setText(q(program.name));
    programEdit_->setText(q(program.program));
    argsEdit_->setPlainText(joinLines(program.args));
    events_->setEvents(program.events);
    timeout_->setValue(static_cast<int>(program.timeout_s));
    extras_->setVisible(saved);
    baseline_ = draft();
    end();
}

void ProgramEditor::setSourceEnabled(bool enabled) {
    if (source_) {
        source_->enabled = enabled;
        if (baseline_) {
            baseline_->enabled = enabled;
        }
    }
}

void ProgramEditor::clear() {
    forgetShown();
    source_.reset();
    baseline_.reset();
}

void ProgramEditor::showIdle() {
    testButton_->setEnabled(services().programs != nullptr);
    testChip_->showResult(QString(), QStringLiteral("muted"));
}

void ProgramEditor::refreshLive() {
    if (!isSaved() || !source_) {
        return;
    }
    std::vector<RecentRow> rows;
    if (services().programs != nullptr) {
        try {
            for (const automation::RunRecord& record : services().programs->runs(source_->id)) {
                const hooks_format::Outcome outcome = hooks_format::run_result(record);
                rows.push_back({q(hooks_format::clock(record.at, clockOf(services()))),
                                record.ok ? QStringLiteral("Ran") : QStringLiteral("Failed"),
                                q(outcome.text), outcome.ok});
            }
        } catch (const std::exception&) {
            // Polled by a timer: debug only, the editor just shows no rows.
            kLog.debug("Could not read recent program runs");
            rows.clear();
        }
    }
    recent_->setRows(rows);
}

void ProgramEditor::runTest() {
    automation::ProgramControl* engine = services().programs;
    if (engine == nullptr) {
        return;
    }
    if (hasUnsavedChanges() && !(saveFirst && saveFirst())) {
        return;
    }
    const std::string programId = text(itemId());
    testButton_->setEnabled(false);
    testChip_->showResult(QStringLiteral("Running…"), QStringLiteral("busy"));
    requests().whenDone<automation::RunRecord>(
        [engine, programId](std::function<void(automation::RunRecord)> report) {
            engine->run_test(programId, std::move(report));
        },
        [this](std::optional<automation::RunRecord> record, QString error) {
            testButton_->setEnabled(true);
            if (!error.isEmpty() || !record) {
                testChip_->showError(error);
            } else {
                const hooks_format::Outcome outcome = hooks_format::run_result(*record);
                // An exit code fits the chip; no exit code means a sentence saying why.
                if (!record->exit_code) {
                    testChip_->showError(q(outcome.text));
                } else {
                    testChip_->showResult(q(outcome.text), outcome.ok ? QStringLiteral("ok")
                                                                      : QStringLiteral("error"));
                }
            }
            refreshLive();
        });
}

// ---- text filters ----

FilterEditor::FilterEditor(const HookServices& services, QWidget* parent)
    : HookEditor(QStringLiteral("filter"), QStringLiteral("e.g. Fix product names"), services,
                 parent) {
    column()->addSpacing(4);
    kindBar_ = new SegmentedBar(
        {{QStringLiteral("Web address"), QString()}, {QStringLiteral("Program"), QString()}}, true);
    kindBar_->setAccessibleName(QStringLiteral("Filter kind"));
    connect(kindBar_, &SegmentedBar::currentIndexChanged, this, [this] { kindChanged(); });
    column()->addWidget(fieldLabel(QStringLiteral("What changes the words"), kindBar_));
    column()->addWidget(kindBar_);
    urlEdit_ = new QLineEdit;
    urlEdit_->setMaxLength(static_cast<int>(hooks::URL_MAX_CHARS));
    urlEdit_->setPlaceholderText(QStringLiteral("http://127.0.0.1:8080/filter"));
    urlEdit_->setAccessibleName(QStringLiteral("Filter address"));
    connect(urlEdit_, &QLineEdit::textChanged, this, [this] { updateNote(); });
    urlLabel_ = fieldLabel(QStringLiteral("Web address"), urlEdit_);
    column()->addWidget(urlLabel_);
    column()->addWidget(urlEdit_);
    urlNote_ = hint();
    ui::setStyleProperty(urlNote_, ui::prop::tone, QStringLiteral("warning"));
    urlNote_->hide();
    column()->addWidget(urlNote_);
    buildProgramFields(this, column(), QStringLiteral("Filter"));
    column()->addSpacing(4);
    stageBar_ =
        new SegmentedBar({{QStringLiteral("Before AI cleanup"), QStringLiteral("Words as heard")},
                          {QStringLiteral("After AI cleanup"), QStringLiteral("Final words")}});
    stageBar_->setAccessibleName(QStringLiteral("When the filter runs"));
    column()->addWidget(fieldLabel(QStringLiteral("When it runs"), stageBar_));
    column()->addWidget(stageBar_);
    column()->addSpacing(4);
    timeout_ = new NoWheelSpinBox;
    timeout_->setRange(static_cast<int>(hooks::FILTER_TIMEOUT_MIN_MS),
                       static_cast<int>(hooks::FILTER_TIMEOUT_MAX_MS));
    timeout_->setSingleStep(100);
    timeout_->setSuffix(QStringLiteral(" ms"));
    timeout_->setAccessibleName(QStringLiteral("Time limit in milliseconds"));
    column()->addWidget(inlineField(QStringLiteral("Time limit"), timeout_));
    replyHint_ = hint();
    column()->addWidget(replyHint_);
    kindChanged();

    finish();
    connect(kindBar_, &SegmentedBar::currentIndexChanged, this, [this] { draftEdited(); });
    connect(urlEdit_, &QLineEdit::textChanged, this, [this] { draftEdited(); });
    connect(programEdit_, &QLineEdit::textChanged, this, [this] { draftEdited(); });
    connect(argsEdit_, &QPlainTextEdit::textChanged, this, [this] { draftEdited(); });
    connect(stageBar_, &SegmentedBar::currentIndexChanged, this, [this] { draftEdited(); });
    connect(timeout_, &QSpinBox::valueChanged, this, [this] { draftEdited(); });
}

std::string FilterEditor::kind() const {
    return std::string(hooks::FILTER_KINDS[std::max(0, kindBar_->currentIndex())]);
}

void FilterEditor::kindChanged() {
    const bool http = kind() == hooks::FILTER_HTTP;
    for (QWidget* widget : {static_cast<QWidget*>(urlLabel_), static_cast<QWidget*>(urlEdit_)}) {
        widget->setVisible(http);
    }
    for (QWidget* widget :
         {static_cast<QWidget*>(programLabel_), static_cast<QWidget*>(programRow_),
          static_cast<QWidget*>(argsLabel_), static_cast<QWidget*>(argsEdit_)}) {
        widget->setVisible(!http);
    }
    updateNote();
    replyHint_->setText(
        http ? q(std::string(identity().product) +
                 " posts the words as JSON. Reply {\"text\": \"…\"} to change them, {} to keep "
                 "them, or {\"cancel\": true} to paste nothing. A slow or odd reply keeps your "
                 "words.")
             : QStringLiteral("The words arrive as JSON on the program's input. Print {\"text\": "
                              "\"…\"} to change them, {} to keep them, or {\"cancel\": true} to "
                              "paste nothing. A slow or odd reply keeps your words."));
}

void FilterEditor::updateNote() {
    const QString note = kind() == hooks::FILTER_HTTP
                             ? q(hooks_format::unencrypted_note(text(urlEdit_->text())))
                             : QString();
    urlNote_->setText(note);
    urlNote_->setVisible(!note.isEmpty());
}

QString FilterEditor::itemId() const {
    return source_ ? q(source_->id) : QString();
}

bool FilterEditor::hasUnsavedChanges() const {
    return source_ && (!baseline_ || !(draft() == *baseline_));
}

hooks::TextFilter FilterEditor::draft() const {
    const bool http = kind() == hooks::FILTER_HTTP;
    hooks::TextFilter filter;
    filter.id = source_->id;
    filter.name = py::strip(text(nameEdit()->text()));
    filter.kind = kind();
    filter.stage = std::string(hooks::STAGES[std::max(0, stageBar_->currentIndex())]);
    filter.url = http ? py::strip(text(urlEdit_->text())) : std::string();
    filter.program = http ? std::string() : py::strip(text(programEdit_->text()));
    filter.args = http ? std::vector<std::string>() : args();
    filter.timeout_ms = timeout_->value();
    filter.enabled = source_->enabled;
    return filter;
}

void FilterEditor::showItem(const hooks::TextFilter& filter, bool saved) {
    begin(saved);
    source_ = filter;
    nameEdit()->setText(q(filter.name));
    const auto kindIt = std::find(std::begin(hooks::FILTER_KINDS), std::end(hooks::FILTER_KINDS),
                                  std::string_view(filter.kind));
    kindBar_->setCurrentIndex(static_cast<int>(kindIt - std::begin(hooks::FILTER_KINDS)));
    kindChanged();
    urlEdit_->setText(q(filter.url));
    programEdit_->setText(q(filter.program));
    argsEdit_->setPlainText(joinLines(filter.args));
    const auto stageIt = std::find(std::begin(hooks::STAGES), std::end(hooks::STAGES),
                                   std::string_view(filter.stage));
    stageBar_->setCurrentIndex(static_cast<int>(stageIt - std::begin(hooks::STAGES)));
    timeout_->setValue(static_cast<int>(filter.timeout_ms));
    baseline_ = draft();
    end();
}

void FilterEditor::setSourceEnabled(bool enabled) {
    if (source_) {
        source_->enabled = enabled;
        if (baseline_) {
            baseline_->enabled = enabled;
        }
    }
}

void FilterEditor::clear() {
    forgetShown();
    source_.reset();
    baseline_.reset();
}

} // namespace fv
