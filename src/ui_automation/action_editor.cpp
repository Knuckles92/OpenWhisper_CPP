#include "ui_automation/action_editor.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <utility>
#include <vector>

#include <QLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QStringList>

#include "agents/agent_actions.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/action_copy.h"
#include "automation/hooks_model.h"
#include "base/identity.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/action_relay.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.action_editor"};

namespace ac = automation::action_copy;
using automation::Action;
using automation::ActionKind;

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

// What the note box says for a kind without fields of its own (_KIND_NOTES); "" for the others.
QString kindNoteFor(std::string_view kind) {
    const QString product = q(identity().product);
    if (kind == ActionKind::CHANNEL) {
        return QStringLiteral("Needs a Claude Code session running the %1 channel. With none "
                              "listening, your words are copied instead.")
            .arg(product);
    }
    if (kind == ActionKind::CLIPBOARD) {
        return QStringLiteral(
            "Your words are copied, ready to paste anywhere. Nothing is pasted for you.");
    }
    if (kind == ActionKind::SCRIPTS) {
        return QStringLiteral("Nothing happens here: programs following %1's live events see "
                              "your words and act on them.")
            .arg(product);
    }
    return {};
}

bool hasKindNote(std::string_view kind) {
    return kind == ActionKind::CHANNEL || kind == ActionKind::CLIPBOARD ||
           kind == ActionKind::SCRIPTS;
}

// Action(id, "", kind): a blank action of `kind`, whose param() gives the kind's defaults.
Action blank(const std::string& id, std::string_view kind) {
    Action action;
    action.id = id;
    action.kind = std::string(kind);
    return action;
}

// int(timeout_s or 30). load_actions has already made a stored value a whole number (anything
// else reads as the default); 0 is 30 seconds. A number far outside the box's 1 to 300 seconds,
// which a hand-edited file can hold, is clamped before the cast (a double beyond int's range has
// no int), and the box clamps it to its range after.
int timeoutSeconds(const Json& value) {
    if (!value.is_number()) {
        return 30;
    }
    const double seconds = value.get<double>();
    if (seconds == 0) {
        return 30;
    }
    return static_cast<int>(std::clamp(std::trunc(seconds), -1.0e6, 1.0e6));
}

QString joinNames(const std::vector<std::string>& names) {
    QStringList joined;
    for (const std::string& name : names) {
        joined << q(name);
    }
    return joined.join(QStringLiteral(", "));
}

} // namespace

ActionEditor::ActionEditor(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                           QWidget* parent)
    : InfoTile(QStringLiteral("New action"), QString(),
               ui::designIcon(QStringLiteral("bolt-green.svg")), parent),
      store_(store), relay_(relay), tryAction_(std::move(tryAction)) {
    using namespace action_fields;
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionEditor"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setBodyIndented(false);

    nameEdit_ = inputField(new QLineEdit);
    nameEdit_->setMaxLength(static_cast<int>(automation::NAME_MAX_CHARS));
    nameEdit_->setPlaceholderText(QStringLiteral("e.g. Ask Claude"));
    addBody(stack({fieldLabel(QStringLiteral("Name"), nameEdit_), nameEdit_}));

    // -- open a link --
    templateEdit_ = inputField(new QLineEdit);
    templateEdit_->setMaxLength(static_cast<int>(automation::TEMPLATE_MAX_CHARS));
    templateEdit_->setPlaceholderText(QStringLiteral("https://www.perplexity.ai/search?q={text}"));
    templateEdit_->setAccessibleName(QStringLiteral("Link"));
    connect(templateEdit_, &QLineEdit::textChanged, this, [this] { updatePreview(); });
    templateHint_ = hintLabel(
        QStringLiteral("{text} becomes your words. Links only fill things in; you send."));
    preview_ = new WrappedLabel(QString(), nullptr, true);
    preview_->setObjectName(QStringLiteral("actionLinkPreview"));
    preview_->setTextFormat(Qt::PlainText);
    presetNote_ = hintLabel();
    ui::setStyleProperty(presetNote_, ui::prop::tone, QStringLiteral("warning"));
    linkBox_ = stack({fieldLabel(QStringLiteral("Link"), templateEdit_), templateEdit_,
                      templateHint_, preview_, presetNote_});
    addBody(linkBox_);

    // -- an agent --
    agentFields_ = new AgentFields;
    connect(agentFields_, &AgentFields::stylesRequested, this,
            [this] { Q_EMIT destinationRequested(QStringLiteral("styles")); });
    agentBox_ = stack({agentFields_});
    addBody(agentBox_);

    // -- a webhook --
    urlEdit_ = inputField(new QLineEdit);
    urlEdit_->setPlaceholderText(
        QStringLiteral("https://example.com/hooks/%1").arg(q(identity().product).toLower()));
    urlEdit_->setAccessibleName(QStringLiteral("Address"));
    connect(urlEdit_, &QLineEdit::textChanged, this, [this] { updateUrlNote(); });
    urlNote_ = hintLabel();
    ui::setStyleProperty(urlNote_, ui::prop::tone, QStringLiteral("warning"));
    replyCombo_ = new OutputCombo(QStringLiteral("Where the reply goes"));
    // Short labels under "Signing secret", so they fit a narrow window.
    copySecretButton_ = neutralButton(new Button(QStringLiteral("Copy")));
    copySecretButton_->setAccessibleName(QStringLiteral("Copy signing secret"));
    connect(copySecretButton_, &QPushButton::clicked, this, [this] { copySecret(); });
    newSecretButton_ = neutralButton(new Button(QStringLiteral("New")));
    newSecretButton_->setAccessibleName(QStringLiteral("Make a new signing secret"));
    connect(newSecretButton_, &QPushButton::clicked, this, [this] { newSecret(); });
    secretHint_ = hintLabel(QStringLiteral("Your service checks each request with it."));
    secretHint_->setToolTip(QStringLiteral("Requests are signed the Standard Webhooks way: "
                                           "webhook-id, webhook-timestamp and webhook-signature "
                                           "headers."));
    webhookBox_ = stack({
        fieldLabel(QStringLiteral("Address"), urlEdit_),
        urlEdit_,
        urlNote_,
        new Columns(stack({fieldLabel(QStringLiteral("Reply goes to"), replyCombo_), replyCombo_}),
                    stack({fieldLabel(QStringLiteral("Signing secret")),
                           buttonRow({copySecretButton_, newSecretButton_}).box, secretHint_}),
                    240),
    });
    addBody(webhookBox_);

    // -- a program --
    programField_ = new PathField(QStringLiteral("Program"), false,
                                  QStringLiteral("Choose a program or script"));
    argsEdit_ = inputField(new QPlainTextEdit);
    argsEdit_->setObjectName(QStringLiteral("actionArgsEdit"));
    argsEdit_->setAccessibleName(QStringLiteral("Arguments, one per line"));
    argsEdit_->setPlaceholderText(QStringLiteral("--flag\nvalue"));
    argsEdit_->setTabChangesFocus(true);
    argsEdit_->setFixedHeight(68);
    programOutput_ = new OutputCombo(QStringLiteral("Where the output goes"));
    timeoutSpin_ = inputField(new NoWheelSpinBox);
    timeoutSpin_->setRange(1, 300);
    timeoutSpin_->setSuffix(QStringLiteral(" s"));
    timeoutSpin_->setAccessibleName(QStringLiteral("Time limit in seconds"));
    timeoutSpin_->setMinimumHeight(36);
    timeoutSpin_->setMaximumWidth(160);
    QWidget* outputColumn =
        stack({fieldLabel(QStringLiteral("Output goes to"), programOutput_), programOutput_});
    QWidget* timeoutColumn =
        stack({fieldLabel(QStringLiteral("Time limit"), timeoutSpin_), timeoutSpin_});
    auto* outputRow = new Columns(outputColumn, timeoutColumn, 200);
    programBox_ = stack({
        fieldLabel(QStringLiteral("Program")),
        programField_,
        hintLabel(QStringLiteral("Your words arrive on its input. It never runs through a shell.")),
        fieldLabel(QStringLiteral("Arguments (one per line)"), argsEdit_),
        argsEdit_,
        outputRow,
    });
    addBody(programBox_);

    // -- channel, clipboard, scripts --
    kindNote_ = hintLabel();
    mcpLink_ = pageLink(QStringLiteral("Set up the channel"),
                        QStringLiteral("Settings → Automate → API & MCP"));
    connect(mcpLink_, &QPushButton::clicked, this,
            [this] { Q_EMIT destinationRequested(QStringLiteral("mcp")); });
    noteBox_ = stack({kindNote_, plainRow({mcpLink_}, 8, true)});
    addBody(noteBox_);

    // -- for every kind --
    shaping_ = new ShapingField;
    addBody(sectionHeading(QStringLiteral("Your words")));
    addBody(shaping_);

    shortcut_ = new ShortcutField(QStringLiteral("Action shortcut"));
    connect(shortcut_, &ShortcutField::captured, this,
            [this](const QString& hotkey) { onShortcutCaptured(hotkey); });
    connect(shortcut_, &ShortcutField::captureChanged, this, &ActionEditor::captureChanged);
    spokenEdit_ = inputField(new QLineEdit);
    spokenEdit_->setPlaceholderText(QStringLiteral("e.g. Claude, ask Claude"));
    spokenEdit_->setAccessibleName(QStringLiteral("Spoken names, separated by commas"));
    descriptionEdit_ = inputField(new QLineEdit);
    descriptionEdit_->setMaxLength(static_cast<int>(automation::DESCRIPTION_MAX_CHARS));
    descriptionEdit_->setPlaceholderText(
        QStringLiteral("What it's for, e.g. Questions for Claude"));
    descriptionEdit_->setAccessibleName(QStringLiteral("What it's for"));
    addBody(sectionHeading(QStringLiteral("How you start it")));
    startColumns_ = new Columns(
        stack({fieldLabel(QStringLiteral("Shortcut (optional)"), shortcut_->input()), shortcut_,
               hintLabel(QStringLiteral("Press it once to start, speak, then press it again."))}),
        stack({fieldLabel(QStringLiteral("Spoken names"), spokenEdit_), spokenEdit_,
               hintLabel(QStringLiteral("With Route by voice, say one first: “Claude, …”"))}));
    addBody(startColumns_);
    QWidget* purpose =
        stack({fieldLabel(QStringLiteral("What it's for"), descriptionEdit_), descriptionEdit_,
               hintLabel(QStringLiteral("Route by voice picks the right action "
                                        "more reliably when it says what it's "
                                        "for."))});
    purpose->layout()->setContentsMargins(0, 6, 0, 0);
    addBody(purpose);

    sampleEdit_ = inputField(new QLineEdit(q(ac::SAMPLE_WORDS)));
    sampleEdit_->setAccessibleName(QStringLiteral("Words to try it with"));
    sampleEdit_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    tryButton_ = neutralButton(new Button(QStringLiteral("Try it")));
    fitCompactButton(tryButton_);
    connect(tryButton_, &QPushButton::clicked, this, [this] { tryIt(); });
    auto* tryRow = new Reflow(sampleEdit_, tryButton_, 180, nullptr, Qt::AlignLeft);
    tryResult_ = hintLabel();
    tryResult_->setObjectName(QStringLiteral("actionTryResult"));
    tryResult_->hide();
    addBody(sectionHeading(QStringLiteral("Try it")));
    addBody(stack({tryRow, tryResult_}));

    message_ = hintLabel();
    ui::setStyleProperty(message_, ui::prop::tone, QStringLiteral("error"));
    message_->hide();
    addBody(message_);
    cancelButton_ = neutralButton(new Button(QStringLiteral("Cancel")));
    connect(cancelButton_, &QPushButton::clicked, this, [this] { cancel(); });
    saveButton_ = compactPrimaryButton(new Button(QStringLiteral("Save action")));
    connect(saveButton_, &QPushButton::clicked, this, [this] { save(); });
    addBody(buttonRow({cancelButton_, saveButton_}, RowAlign::Right).box);

    connect(
        &relay_, &ActionRelay::tryDone, this,
        [this](int token, const automation::ActionResult& result) { onTryDone(token, result); });
    connect(&relay_, &ActionRelay::scanDone, this, [this](int token) { onScanDone(token); });
    connect(&relay_, &ActionRelay::secretDone, this,
            [this](int token, const QString& secret, const QString& error) {
                onSecretDone(token, secret, error);
            });
}

Action ActionEditor::ontoSaved(const Action& saved, const Action& baseline, const Action& draft) {
    Action result = saved;
    // Every Action field but params, in the dataclass's order.
    if (draft.id != baseline.id) {
        result.id = draft.id;
    }
    if (draft.name != baseline.name) {
        result.name = draft.name;
    }
    if (draft.kind != baseline.kind) {
        result.kind = draft.kind;
    }
    if (draft.hotkey != baseline.hotkey) {
        result.hotkey = draft.hotkey;
    }
    if (draft.shaping != baseline.shaping) {
        result.shaping = draft.shaping;
    }
    if (draft.spoken_names != baseline.spoken_names) {
        result.spoken_names = draft.spoken_names;
    }
    if (draft.description != baseline.description) {
        result.description = draft.description;
    }
    if (draft.enabled != baseline.enabled) {
        result.enabled = draft.enabled;
    }
    Json params = saved.params.is_object() ? saved.params : Json::object();
    if (draft.params.is_object()) {
        for (const auto& [key, value] : draft.params.items()) {
            const bool known = baseline.params.is_object() && baseline.params.contains(key);
            if (!known || baseline.params.at(key) != value) {
                params[key] = value;
            }
        }
    }
    result.params = std::move(params);
    return result;
}

QString ActionEditor::actionId() const {
    return action_ ? q(action_->id) : QString();
}

std::optional<Action> ActionEditor::savedAction() const {
    if (isNew_) {
        return std::nullopt;
    }
    return action_;
}

Json ActionEditor::settingsNow() const {
    return *store_.snapshot();
}

void ActionEditor::open(const Action& action, bool isNew, const QString& note) {
    shortcut_->cancelCapture();
    dropUnsavedSecret();
    action_ = action;
    isNew_ = isNew;
    secretToken_ = 0;
    setSecretBusy(false);
    titleLabel()->setText(isNew ? QStringLiteral("New action")
                                : QStringLiteral("Edit %1").arg(q(action.name)));
    setDescription(q(ac::label_for(ac::KIND_BLURBS(), action.kind)));
    action_fields::tintIcon(this, q(ac::label_for(ac::KIND_ICONS(), action.kind)), 32, false);
    presetNote_->setText(note);
    presetNote_->setVisible(!note.isEmpty());
    fill(action);
    for (QLineEdit* edit :
         {nameEdit_, templateEdit_, urlEdit_, spokenEdit_, descriptionEdit_, sampleEdit_}) {
        edit->setCursorPosition(0);
    }
    tryResult_->hide();
    tryButton_->setEnabled(true);
    tryToken_ = 0;
    showMessage(QString());
    // The draft as opened: untouched, there is nothing to keep.
    baseline_ = draft();
}

void ActionEditor::follow(const Action& saved) {
    const Action edited = ontoSaved(saved, *baseline_, draft());
    action_ = saved;
    fill(saved);
    baseline_ = draft();
    if (!(edited == saved)) {
        fill(edited);
    }
}

void ActionEditor::fill(const Action& action) {
    const Json settings = settingsNow();
    nameEdit_->setText(q(action.name));
    const std::string& kind = action.kind;
    // Every kind's fields are filled, with the kind's defaults where they don't apply, so no box
    // keeps the last action's values.
    const Action link =
        kind == ActionKind::OPEN_LINK ? action : blank(action.id, ActionKind::OPEN_LINK);
    const Action hook =
        kind == ActionKind::WEBHOOK ? action : blank(action.id, ActionKind::WEBHOOK);
    const Action program =
        kind == ActionKind::PROGRAM ? action : blank(action.id, ActionKind::PROGRAM);
    templateEdit_->setText(q(text(link.param("template"))));
    agentFields_->setParams(kind == ActionKind::AGENT ? action.params : Json::object());
    agentFields_->setAppAwareness(keys::app_context_enabled.read(settings));
    if (kind == ActionKind::AGENT) {
        showAgents(agents::available());
    }
    urlEdit_->setText(q(text(hook.param("url"))));
    replyCombo_->setOutput(q(text(hook.param("output"))));
    programField_->setPath(q(text(program.param("program"))));
    QStringList args;
    if (const Json value = program.param("args"); value.is_array()) {
        for (const Json& arg : value) {
            args << q(text(arg));
        }
    }
    argsEdit_->setPlainText(args.join(QLatin1Char('\n')));
    programOutput_->setOutput(q(text(program.param("output"))));
    timeoutSpin_->setValue(timeoutSeconds(program.param("timeout_s")));
    kindNote_->setText(kindNoteFor(kind));
    mcpLink_->setVisible(kind == ActionKind::CHANNEL);
    shaping_->setChoices(settings);
    shaping_->setValue(q(action.shaping));
    spokenEdit_->setText(joinNames(action.spoken_names));
    descriptionEdit_->setText(q(action.description));
    shortcut_->setHotkey(q(action.hotkey));
    linkBox_->setVisible(kind == ActionKind::OPEN_LINK);
    agentBox_->setVisible(kind == ActionKind::AGENT);
    webhookBox_->setVisible(kind == ActionKind::WEBHOOK);
    programBox_->setVisible(kind == ActionKind::PROGRAM);
    noteBox_->setVisible(hasKindNote(kind));
    updatePreview();
    updateUrlNote();
}

void ActionEditor::showAgents(const std::map<std::string, bool>& found) {
    // What the last scan found, or look once when nothing has been scanned yet.
    if (!found.empty() || agentsScanned_) {
        agentFields_->setAvailable(found, true);
        return;
    }
    agentsScanned_ = true;
    scanToken_ = relay_.nextToken();
    agentFields_->setAvailable({}, false);
    relay_.startAgentScan(scanToken_);
}

void ActionEditor::onScanDone(int token) {
    if (token != scanToken_) {
        return;
    }
    agentFields_->setAvailable(agents::available(), true, true);
}

void ActionEditor::refreshChoices(const Json& settings) {
    shaping_->setChoices(settings);
    agentFields_->setAppAwareness(keys::app_context_enabled.read(settings));
}

Action ActionEditor::draft() const {
    Action action = *action_;
    const std::string& kind = action.kind;
    Json params = Json::object();
    if (kind == ActionKind::OPEN_LINK) {
        params["template"] = py::strip(qt::toUtf8(templateEdit_->text()));
    } else if (kind == ActionKind::AGENT) {
        params = agentFields_->params();
    } else if (kind == ActionKind::WEBHOOK) {
        params["url"] = py::strip(qt::toUtf8(urlEdit_->text()));
        params["output"] = qt::toUtf8(replyCombo_->output());
    } else if (kind == ActionKind::PROGRAM) {
        params["program"] = qt::toUtf8(programField_->path());
        params["args"] = ac::parse_arguments(qt::toUtf8(argsEdit_->toPlainText()));
        params["output"] = qt::toUtf8(programOutput_->output());
        params["timeout_s"] = timeoutSpin_->value();
    }
    // Only the kind's own parameters, in PARAMS order.
    Json kept = Json::object();
    for (const auto& [key, _default] : automation::PARAMS(kind).items()) {
        if (params.is_object() && params.contains(key)) {
            kept[key] = params.at(key);
        }
    }
    action.name = py::strip(qt::toUtf8(nameEdit_->text()));
    action.params = std::move(kept);
    action.hotkey = qt::toUtf8(shortcut_->hotkey());
    action.shaping = qt::toUtf8(shaping_->value());
    action.spoken_names = ac::parse_spoken_names(qt::toUtf8(spokenEdit_->text()));
    action.description = py::strip(qt::toUtf8(descriptionEdit_->text()));
    return action;
}

bool ActionEditor::hasUnsavedChanges() const {
    return action_ && baseline_ && !(draft() == *baseline_);
}

// ---- live hints ----

void ActionEditor::updatePreview() {
    const std::string templ = py::strip(qt::toUtf8(templateEdit_->text()));
    if (templ.empty()) {
        preview_->setText(QString());
        preview_->hide();
        return;
    }
    QString shown;
    QString tone;
    try {
        const QString sample = sampleEdit_->text();
        const std::string link = automation::expand_link(
            templ, sample.isEmpty() ? std::string(ac::SAMPLE_WORDS) : qt::toUtf8(sample));
        shown = QStringLiteral("Opens ") + q(link);
        if (templ.find("{text}") == std::string::npos) {
            shown = QStringLiteral("Add {text} where your words should go.");
            tone = QStringLiteral("warning");
        }
    } catch (const py::ValueError& refused) {
        shown = q(refused.what());
        tone = QStringLiteral("warning");
    }
    preview_->setText(shown);
    ui::setStyleProperty(preview_, ui::prop::tone, tone);
    preview_->show();
}

void ActionEditor::updateUrlNote() {
    const std::string url = py::strip(qt::toUtf8(urlEdit_->text()));
    const bool unencrypted = q(url).toLower().startsWith(QStringLiteral("http://")) &&
                             !automation::hooks::url_is_private(url);
    urlNote_->setText(unencrypted ? QStringLiteral("Your words travel unencrypted to this address.")
                                  : QString());
    urlNote_->setVisible(unencrypted);
}

void ActionEditor::showMessage(const QString& text, const QString& tone) {
    message_->setText(text);
    ui::setStyleProperty(message_, ui::prop::tone, tone);
    message_->setVisible(!text.isEmpty());
}

// ---- shortcut ----

void ActionEditor::onShortcutCaptured(const QString& hotkey) {
    const std::string conflict = automation::action_hotkey_conflict(
        qt::toUtf8(hotkey), settingsNow(), action_ ? action_->id : std::string());
    if (!conflict.empty()) {
        shortcut_->restore();
        showMessage(QStringLiteral("That shortcut is already used by %1. Choose another.")
                        .arg(q(conflict)));
        return;
    }
    shortcut_->setHotkey(hotkey);
    showMessage(QString());
}

void ActionEditor::cancelCapture() {
    shortcut_->cancelCapture();
}

// ---- signing secret ----

void ActionEditor::copySecret() {
    fetchSecret(false);
}

void ActionEditor::newSecret() {
    if (!isNew_) {
        // The service already checks requests with the old one.
        const auto answer = action_prompts::ask(
            this, QStringLiteral("Make a new signing secret?"),
            QStringLiteral(
                "Your service will need the new secret before it accepts requests again."),
            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
        if (answer != QMessageBox::Yes) {
            return;
        }
    }
    fetchSecret(true);
}

void ActionEditor::fetchSecret(bool renew) {
    secretToken_ = relay_.nextToken();
    secretRenew_ = renew;
    if (isNew_) {
        secretMade_ = true;
    }
    setSecretBusy(true);
    showMessage(renew ? QStringLiteral("Making a new signing secret…")
                      : QStringLiteral("Getting the signing secret…"),
                QString());
    relay_.fetchSecret(secretToken_, actionId(), renew, this);
}

void ActionEditor::setSecretBusy(bool busy) {
    copySecretButton_->setEnabled(!busy);
    newSecretButton_->setEnabled(!busy);
}

void ActionEditor::onSecretDone(int token, const QString& secret, const QString& error) {
    if (token != secretToken_ || token == 0) {
        return;
    }
    secretToken_ = 0;
    setSecretBusy(false);
    if (secret.isEmpty()) {
        showMessage(QStringLiteral("Couldn't get the signing secret: %1")
                        .arg(error.isEmpty() ? QStringLiteral("nothing was saved") : error));
        return;
    }
    if (const action_prompts::CopyText& copy = action_prompts::copyText()) {
        copy(secret);
    }
    showMessage(secretRenew_
                    ? QStringLiteral("New signing secret copied. Update your service with it.")
                    : QStringLiteral("Signing secret copied. Paste it into your service."),
                QStringLiteral("ok"));
}

void ActionEditor::dropUnsavedSecret() {
    // A new action that's never saved leaves no secret behind.
    if (isNew_ && secretMade_ && action_) {
        relay_.forgetSecret(q(action_->id));
    }
    secretMade_ = false;
}

// ---- trying ----

void ActionEditor::tryIt() {
    if (!action_) {
        return;
    }
    Action probe = draft();
    if (probe.name.empty()) {
        probe.name = "Untitled action";
    }
    probe.hotkey.clear();
    probe.spoken_names.clear();
    try {
        automation::validate_action(probe, settingsNow());
    } catch (const py::ValueError& refused) {
        showTryResult(q(refused.what()), QStringLiteral("error"));
        return;
    }
    tryToken_ = relay_.nextToken();
    tryButton_->setEnabled(false);
    showTryResult(QStringLiteral("Trying…"), QString());
    const QString sample = q(py::strip(qt::toUtf8(sampleEdit_->text())));
    if (tryAction_) {
        tryAction_(probe, sample.isEmpty() ? q(ac::SAMPLE_WORDS) : sample,
                   relay_.tryCallback(tryToken_));
    }
}

void ActionEditor::onTryDone(int token, const automation::ActionResult& result) {
    if (token != tryToken_ || tryToken_ == 0) {
        return;
    }
    tryToken_ = 0;
    tryButton_->setEnabled(true);
    showTryResult(q(ac::try_outcome(result)),
                  result.ok ? QStringLiteral("ok") : QStringLiteral("error"));
}

void ActionEditor::showTryResult(const QString& text, const QString& tone) {
    tryResult_->setText(text);
    ui::setStyleProperty(tryResult_, ui::prop::tone, tone);
    tryResult_->setVisible(!text.isEmpty());
}

// ---- saving ----

bool ActionEditor::save() {
    if (!action_) {
        return true;
    }
    shortcut_->cancelCapture();
    const Action current = draft();
    const Action baseline = *baseline_;
    const bool isNew = isNew_;
    Action written = current;
    try {
        store_.mutate([&](Json& settings) {
            Action action = current;
            if (!isNew) {
                // Your edits, made to the action as saved now, not as it was opened.
                const std::optional<Action> saved = automation::find_action(settings, current.id);
                if (!saved) {
                    throw py::ValueError(baseline.name +
                                         " no longer exists, so there's nothing to save these "
                                         "changes to.");
                }
                action = ontoSaved(*saved, baseline, current);
            }
            automation::save_action(settings, action);
            written = action;
        });
    } catch (const py::ValueError& refused) {
        showMessage(q(refused.what()));
        return false;
    } catch (const std::exception& failure) {
        kLog.warning("Could not save the action ({})", type_display_name(typeid(failure)));
        showMessage(QStringLiteral("Couldn't save the action: %1").arg(q(failure.what())));
        return false;
    }
    secretMade_ = false;
    isNew_ = false;
    std::optional<Action> stored = automation::find_action(settingsNow(), current.id);
    action_ = stored ? *stored : written;
    baseline_ = draft();
    Q_EMIT saved(*action_);
    return true;
}

void ActionEditor::cancel() {
    discard();
    Q_EMIT cancelled();
}

void ActionEditor::discard() {
    shortcut_->cancelCapture();
    dropUnsavedSecret();
    action_.reset();
    baseline_.reset();
}

bool ActionEditor::saveDraft(bool ask) {
    if (!hasUnsavedChanges() || save() || !ask) {
        return true;
    }
    const auto answer = action_prompts::ask(
        this, QStringLiteral("Unsaved action"),
        message_->text() + QStringLiteral(" Discard your changes to this action?"),
        QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Discard) {
        return false;
    }
    cancel();
    return true;
}

} // namespace fv
