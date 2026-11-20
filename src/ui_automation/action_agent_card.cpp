#include "ui_automation/action_agent_card.h"

#include <exception>
#include <typeinfo>
#include <utility>

#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "agents/agent_actions.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "automation/action_copy.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "pyport/errors.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/action_relay.h"
#include "ui_hotkeys/profile_hotkey_input.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.action_agent_card"};

namespace ac = automation::action_copy;
using automation::Action;

QString q(std::string_view text) {
    return qt::toQString(text);
}

std::string text(const Json& value) {
    return value.is_string() ? value.get<std::string>() : std::string();
}

} // namespace

namespace action_agent_card {

std::vector<std::string> spoken_for(std::string_view agent_id) {
    if (const auto name = automation::agent_display_name(agent_id)) {
        return {std::string(*name)};
    }
    return {};
}

std::string description_for(std::string_view agent_id) {
    const auto name = automation::agent_display_name(agent_id);
    return "Coding tasks for " + (name ? std::string(*name) : std::string("my coding agent"));
}

} // namespace action_agent_card

using namespace action_agent_card;

AgentHotkeyCard::AgentHotkeyCard(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                                 QWidget* parent)
    : InfoTile(q(TITLE), q(INVITE), ui::designIcon(QStringLiteral("wand-purple.svg")), parent),
      store_(store), relay_(relay), tryAction_(std::move(tryAction)) {
    using namespace action_fields;
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionAgentCard"));
    tintIcon(this, QStringLiteral("wand-purple.svg"), 36);
    ui::setStyleProperty(titleLabel(), ui::prop::tileId, QStringLiteral("actionAgentTitle"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);

    enabledSwitch_ = new SettingsSwitch;
    enabledSwitch_->setAccessibleName(QStringLiteral("Agent hotkey on"));
    connect(enabledSwitch_, &QAbstractButton::toggled, this,
            [this](bool on) { onEnabledToggled(on); });
    addTrailing(enabledSwitch_);

    fields_ = new AgentFields;
    connect(fields_, &AgentFields::changed, this, [this] { onFieldsChanged(); });
    connect(fields_, &AgentFields::stylesRequested, this, &AgentHotkeyCard::stylesRequested);
    addBody(fields_);

    status_ = hintLabel();
    lookAgain_ = pageLink(QStringLiteral("Look again"),
                          QStringLiteral("Check this computer again, for example after installing "
                                         "an agent"));
    connect(lookAgain_, &QPushButton::clicked, this, [this] { startScan(true); });
    statusRow_ = new Reflow(status_, lookAgain_, 200, nullptr, Qt::AlignLeft);
    addBody(statusRow_);

    setupButton_ = compactPrimaryButton(new Button(QStringLiteral("Set it up")));
    setupButton_->setAccessibleName(QStringLiteral("Set up the agent hotkey"));
    connect(setupButton_, &QPushButton::clicked, this, [this] { setUp(); });
    setupRow_ = buttonRow({setupButton_}).box;
    addBody(setupRow_);

    shortcutBox_ = new QWidget;
    ui::setStyleProperty(shortcutBox_, ui::prop::tileId, QStringLiteral("actionRow"));
    auto* shortcutLayout = new QVBoxLayout(shortcutBox_);
    shortcutLayout->setContentsMargins(0, 4, 0, 0);
    shortcutLayout->setSpacing(6);
    shortcut_ = new ShortcutField(QStringLiteral("Agent hotkey shortcut"));
    connect(shortcut_, &ShortcutField::captured, this,
            [this](const QString& hotkey) { onShortcutCaptured(hotkey); });
    connect(shortcut_, &ShortcutField::captureChanged, this, &AgentHotkeyCard::captureChanged);
    shortcutLayout->addWidget(fieldLabel(QStringLiteral("Shortcut"), shortcut_->input()));
    shortcutLayout->addWidget(shortcut_);
    addBody(shortcutBox_);

    message_ = hintLabel();
    ui::setStyleProperty(message_, ui::prop::tileId, QStringLiteral("actionAgentMessage"));
    message_->hide();
    addBody(message_);

    tryButton_ = neutralButton(new Button(QStringLiteral("Try it")));
    tryButton_->setToolTip(
        QStringLiteral("Sends “%1” to the agent, as if you'd said it").arg(q(AGENT_SAMPLE)));
    connect(tryButton_, &QPushButton::clicked, this, [this] { tryIt(); });
    moreButton_ = neutralButton(new Button(QStringLiteral("Customize")));
    moreButton_->setAccessibleName(QStringLiteral("Customize the agent hotkey in the full editor"));
    moreButton_->setToolTip(
        QStringLiteral("Spoken names, how your words are prepared, and the rest"));
    connect(moreButton_, &QPushButton::clicked, this, [this] {
        if (action_) {
            Q_EMIT editRequested(*action_);
        }
    });
    removeButton_ = pageLink(QStringLiteral("Remove"),
                             QStringLiteral("Remove the agent hotkey and its shortcut"));
    ui::setStyleProperty(removeButton_, ui::prop::tone, QStringLiteral("danger"));
    connect(removeButton_, &QPushButton::clicked, this, [this] { remove(); });
    footer_ = new Reflow(buttonRow({tryButton_, moreButton_}).box, removeButton_, std::nullopt,
                         nullptr, Qt::AlignLeft);
    addBody(footer_);

    connect(&relay_, &ActionRelay::scanDone, this, [this](int token) { onScanDone(token); });
    connect(
        &relay_, &ActionRelay::tryDone, this,
        [this](int token, const automation::ActionResult& result) { onTryDone(token, result); });
    fields_->setAvailable({}, false);
    showAvailable(agents::available(), false);
}

// ---- state ----

void AgentHotkeyCard::load(const Json& settings) {
    fields_->setAppAwareness(keys::app_context_enabled.read(settings));
    if (shortcut_->input()->isCapturing()) {
        return;
    }
    action_ = automation::find_action(settings, automation::AGENT_ACTION_ID);
    if (action_ && action_->kind != automation::ActionKind::AGENT) {
        action_.reset(); // An id clash from a hand-edited file; leave it alone.
    }
    render();
}

void AgentHotkeyCard::render() {
    const bool configured = action_.has_value();
    fields_->setCompact(!configured);
    shortcutBox_->setVisible(configured);
    footer_->setVisible(configured);
    enabledSwitch_->setVisible(configured);
    setupRow_->setVisible(!configured);
    if (configured) {
        fields_->setParams(action_->params);
        shortcut_->setHotkey(q(action_->hotkey));
        {
            const QSignalBlocker blocker(enabledSwitch_);
            enabledSwitch_->setChecked(action_->enabled);
        }
        ui::setStyleProperty(this, ui::prop::checked, action_->enabled);
        setDescription(q(ac::summary(*action_)) + QStringLiteral("."));
    } else {
        fields_->setParams(Json{{"agent", qt::toUtf8(unsetAgent())}});
        ui::setStyleProperty(this, ui::prop::checked, false);
        setDescription(q(INVITE));
    }
    syncStatus();
}

QString AgentHotkeyCard::unsetAgent() const {
    // The agent to offer before setup: the one you picked, unless a scan didn't find it.
    const std::optional<std::map<std::string, bool>> available = fields_->chooser()->available();
    if (!picked_.isEmpty()) {
        if (!available) {
            return picked_;
        }
        const auto it = available->find(qt::toUtf8(picked_));
        if (it != available->end() && it->second) {
            return picked_;
        }
    }
    const std::vector<QString> found = fields_->chooser()->found();
    return found.empty() ? q(automation::AGENTS[0]) : found.front();
}

void AgentHotkeyCard::syncStatus() {
    const bool known = fields_->chooser()->available().has_value();
    const bool found = !fields_->chooser()->found().empty();
    const bool configured = action_.has_value();
    QString shown;
    if (!known) {
        shown = q(LOOKING);
    } else if (!found) {
        shown = q(NONE_FOUND);
    }
    status_->setText(shown);
    statusRow_->setVisible(!shown.isEmpty());
    lookAgain_->setVisible(known && !found);
    setupButton_->setEnabled(!configured && found);
}

// ---- finding agents ----

void AgentHotkeyCard::ensureScanned() {
    if (scanned_ || fields_->chooser()->available().has_value()) {
        return;
    }
    startScan(false);
}

void AgentHotkeyCard::startScan(bool refresh) {
    scanned_ = true;
    scanToken_ = relay_.nextToken();
    fields_->setAvailable({}, false);
    syncStatus();
    relay_.startAgentScan(scanToken_, refresh);
}

void AgentHotkeyCard::onScanDone(int token) {
    if (token != scanToken_) {
        return;
    }
    showAvailable(agents::available(), true);
}

void AgentHotkeyCard::showAvailable(const std::map<std::string, bool>& available, bool animate) {
    // An empty answer before any scan means "not looked yet", after one "none found".
    fields_->setAvailable(available, !available.empty() || scanned_, animate);
    if (!action_) {
        fields_->chooser()->setCurrent(unsetAgent());
    }
    syncStatus();
}

// ---- changes ----

bool AgentHotkeyCard::save(const Action& action) {
    try {
        store_.mutate([&action](Json& settings) { automation::save_action(settings, action); });
    } catch (const py::ValueError& refused) {
        showMessage(q(refused.what()), QStringLiteral("error"));
        return false;
    } catch (const std::exception& failure) {
        // The settings file couldn't be written.
        kLog.warning("Could not save the agent hotkey ({})", type_display_name(typeid(failure)));
        showMessage(QStringLiteral("Couldn't save the agent hotkey: %1").arg(q(failure.what())),
                    QStringLiteral("error"));
        return false;
    }
    action_ = action;
    Q_EMIT changed();
    return true;
}

void AgentHotkeyCard::setUp() {
    const std::string agent = qt::toUtf8(fields_->chooser()->current());
    Action action;
    action.id = std::string(automation::AGENT_ACTION_ID);
    action.name = std::string(TITLE);
    action.kind = std::string(automation::ActionKind::AGENT);
    action.params = Json{{"agent", agent},
                         {"mode", automation::AgentMode::HANDOFF},
                         {"surface", automation::AgentSurface::AUTO}};
    action.spoken_names = spoken_for(agent);
    action.description = description_for(agent);
    if (!save(action)) {
        // A spoken name another action already uses: set up without it.
        action.spoken_names.clear();
        if (!save(action)) {
            return;
        }
    }
    render();
    showMessage(QStringLiteral("Now press the shortcut you'd like to use."), QString());
    shortcut_->beginCapture();
}

void AgentHotkeyCard::onFieldsChanged() {
    if (!action_) {
        // Before setup only the agent can change; remember it until then.
        picked_ = fields_->chooser()->current();
        return;
    }
    const Json params = fields_->params();
    Action action = *action_;
    action.params = params;
    const std::string before = text(action_->param("agent"));
    const std::string after =
        params.is_object() && params.contains("agent") ? text(params.at("agent")) : std::string();
    if (before != after) {
        // The name you say and what it's for follow the agent, unless you changed them.
        if (action.spoken_names == spoken_for(before)) {
            action.spoken_names = spoken_for(after);
        }
        if (action.description == description_for(before)) {
            action.description = description_for(after);
        }
    }
    if (save(action)) {
        setDescription(q(ac::summary(action)) + QStringLiteral("."));
        showMessage(QString(), QString());
    } else {
        render();
    }
}

void AgentHotkeyCard::onShortcutCaptured(const QString& hotkey) {
    if (!action_) {
        return;
    }
    Action action = *action_;
    action.hotkey = qt::toUtf8(hotkey);
    if (save(action)) {
        shortcut_->setHotkey(hotkey);
        const QString shortcut =
            q(hotkeys::format_hotkey_display(qt::toUtf8(hotkey), hotkeys::platform_vocabulary()));
        showMessage(
            shortcut.isEmpty()
                ? QStringLiteral("Shortcut removed. Route by voice and the API can still "
                                 "use it.")
                : QStringLiteral("Press %1, say what you need, and press it again.").arg(shortcut),
            QStringLiteral("ok"));
    } else {
        shortcut_->restore();
    }
}

void AgentHotkeyCard::onEnabledToggled(bool on) {
    if (!action_) {
        return;
    }
    Action action = *action_;
    action.enabled = on;
    if (save(action)) {
        ui::setStyleProperty(this, ui::prop::checked, on);
        showMessage(QString(), QString());
    } else {
        const QSignalBlocker blocker(enabledSwitch_);
        enabledSwitch_->setChecked(!on);
    }
}

void AgentHotkeyCard::remove() {
    if (!action_) {
        return;
    }
    const auto answer =
        action_prompts::ask(this, QStringLiteral("Remove the agent hotkey?"),
                            QStringLiteral("Remove the agent hotkey and its shortcut?"),
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    try {
        store_.mutate([](Json& settings) {
            automation::delete_action(settings, automation::AGENT_ACTION_ID);
        });
    } catch (const std::exception& failure) {
        kLog.warning("Could not remove the agent hotkey ({})", type_display_name(typeid(failure)));
        showMessage(QStringLiteral("Couldn't remove the agent hotkey: %1").arg(q(failure.what())),
                    QStringLiteral("error"));
        return;
    }
    shortcut_->cancelCapture();
    picked_ = q(text(action_->param("agent")));
    action_.reset();
    render();
    showMessage(QString(), QString());
    Q_EMIT changed();
}

void AgentHotkeyCard::cancelCapture() {
    shortcut_->cancelCapture();
}

// ---- trying ----

void AgentHotkeyCard::tryIt() {
    if (!action_) {
        return;
    }
    tryToken_ = relay_.nextToken();
    tryButton_->setEnabled(false);
    showMessage(QStringLiteral("Trying…"), QString());
    if (tryAction_) {
        tryAction_(*action_, q(AGENT_SAMPLE), relay_.tryCallback(tryToken_));
    }
}

void AgentHotkeyCard::onTryDone(int token, const automation::ActionResult& result) {
    if (token != tryToken_) {
        return;
    }
    tryToken_ = 0;
    tryButton_->setEnabled(true);
    // A quick answer comes back with the answer itself, as in the editor.
    showMessage(q(ac::try_outcome(result)),
                result.ok ? QStringLiteral("ok") : QStringLiteral("error"));
}

// ---- messages ----

void AgentHotkeyCard::showMessage(const QString& text, const QString& tone) {
    message_->setText(text);
    ui::setStyleProperty(message_, ui::prop::tone, tone);
    message_->setVisible(!text.isEmpty());
}

void AgentHotkeyCard::showEvent(QShowEvent* event) {
    InfoTile::showEvent(event);
    ensureScanned();
}

} // namespace fv
