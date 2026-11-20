#include "ui_automation/action_panel.h"

#include <algorithm>
#include <exception>
#include <set>
#include <typeinfo>
#include <utility>
#include <vector>

#include <QLabel>
#include <QLineEdit>
#include <QPoint>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "hotkeys/grammar.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "settings/store.h"
#include "ui_automation/action_fields.h"
#include "ui_automation/action_gallery.h"
#include "ui_automation/action_list.h"
#include "ui_automation/action_prompts.h"
#include "ui_automation/action_relay.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.action_panel"};

using automation::Action;

QString q(std::string_view text) {
    return qt::toQString(text);
}

} // namespace

namespace action_panel {

std::string unique_name(std::string_view name, const Json& settings) {
    std::set<std::string> taken;
    for (const Action& action : automation::load_actions(settings)) {
        taken.insert(py::casefold(action.name));
    }
    std::string candidate(name);
    for (int suffix = 2; taken.contains(py::casefold(candidate)); ++suffix) {
        candidate = std::string(name) + " " + std::to_string(suffix);
    }
    return candidate;
}

std::string saved_note(const Action& action) {
    const std::string shortcut =
        hotkeys::format_hotkey_display(action.hotkey, hotkeys::platform_vocabulary());
    if (!shortcut.empty()) {
        return "Saved " + action.name + ". Press " + shortcut + ", speak, then press it again.";
    }
    if (!action.spoken_names.empty()) {
        return "Saved " + action.name + ". With Route by voice, say “" + action.spoken_names[0] +
               ", …”.";
    }
    return "Saved " + action.name + ". Give it a shortcut or a spoken name to use it.";
}

} // namespace action_panel

ActionsPanel::ActionsPanel(SettingsStore& store, ActionRelay& relay, TryAction tryAction,
                           QWidget* parent)
    : QWidget(parent), store_(store), relay_(relay) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionsPanel"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    title_ = new QLabel(QStringLiteral("Your actions"));
    title_->setObjectName(QStringLiteral("settingsTileGroupTitle"));
    addButton_ = compactPrimaryButton(new Button(QStringLiteral("Add action")));
    fitCompactButton(addButton_);
    connect(addButton_, &QPushButton::clicked, this, [this] { addAction(); });
    header_ = new Reflow(title_, addButton_);
    layout->addWidget(header_);
    notice_ = action_fields::hintLabel();
    ui::setStyleProperty(notice_, ui::prop::tileId, QStringLiteral("actionsNotice"));
    notice_->hide();
    layout->addWidget(notice_);

    list_ = new ActionList;
    connect(list_, &ActionList::addRequested, this, [this] { addAction(); });
    connect(list_, &ActionList::editRequested, this, [this](const QString& id) { edit(id); });
    connect(list_, &ActionList::deleteRequested, this,
            [this](const QString& id) { deleteAction(id); });
    connect(list_, &ActionList::enabledToggled, this,
            [this](const QString& id, bool on) { setActionEnabled(id, on); });
    layout->addWidget(list_);

    gallery_ = new PresetGallery;
    connect(gallery_, &PresetGallery::picked, this,
            [this](const automation::Preset& preset) { startFromPreset(preset); });
    connect(gallery_, &PresetGallery::cancelled, this, [this] { showList(); });
    gallery_->hide();
    layout->addWidget(gallery_);

    editor_ = new ActionEditor(store, relay, std::move(tryAction));
    connect(editor_, &ActionEditor::saved, this, [this](const Action& action) { onSaved(action); });
    connect(editor_, &ActionEditor::cancelled, this, [this] { showList(); });
    connect(editor_, &ActionEditor::captureChanged, this, &ActionsPanel::captureChanged);
    connect(editor_, &ActionEditor::destinationRequested, this,
            &ActionsPanel::destinationRequested);
    editor_->hide();
    layout->addWidget(editor_);

    flashTimer_ = new QTimer(this);
    flashTimer_->setSingleShot(true);
    flashTimer_->setInterval(action_panel::kFlashMs);
    connect(flashTimer_, &QTimer::timeout, this, [this] { endFlash(); });
    revealTimer_ = new QTimer(this);
    revealTimer_->setSingleShot(true);
    revealTimer_->setInterval(0);
    connect(revealTimer_, &QTimer::timeout, this, [this] { reveal(); });
    refresh();
}

QString ActionsPanel::modeName() const {
    switch (mode_) {
    case Mode::Gallery:
        return QStringLiteral("gallery");
    case Mode::Editor:
        return QStringLiteral("editor");
    case Mode::List:
        break;
    }
    return QStringLiteral("list");
}

// ---- what shows ----

Json ActionsPanel::settingsNow() const {
    return *store_.snapshot();
}

void ActionsPanel::refresh() {
    refresh(settingsNow());
}

void ActionsPanel::refresh(const Json& settings) {
    const std::vector<Action> all = automation::load_actions(settings);
    std::vector<Action> actions;
    for (const Action& action : all) {
        if (action.id != automation::AGENT_ACTION_ID) {
            actions.push_back(action);
        }
    }
    list_->setActions(actions);
    if (mode_ == Mode::Editor) {
        editor_->refreshChoices(settings);
        followSaved(settings);
    }
    addButton_->setEnabled(all.size() < automation::MAX_ACTIONS);
    // With no actions the empty card's own button invites the first one.
    addButton_->setVisible(mode_ == Mode::List && !actions.empty());
    addButton_->setToolTip(addButton_->isEnabled()
                               ? QString()
                               : QStringLiteral("You can keep up to %1 actions. Delete one to add "
                                                "another.")
                                     .arg(automation::MAX_ACTIONS));
}

void ActionsPanel::followSaved(const Json& settings) {
    // The action being edited changed or went away elsewhere, such as on the agent card.
    const std::optional<Action> before = editor_->savedAction();
    if (!before) {
        return;
    }
    const std::optional<Action> saved = automation::find_action(settings, before->id);
    if (!saved) {
        editor_->discard();
        showMode(Mode::List);
        showNotice(QStringLiteral("%1 was removed.").arg(q(before->name)));
    } else if (!(*saved == *before)) {
        editor_->follow(*saved);
    }
}

void ActionsPanel::showMode(Mode mode) {
    mode_ = mode;
    list_->setVisible(mode == Mode::List);
    gallery_->setVisible(mode == Mode::Gallery);
    editor_->setVisible(mode == Mode::Editor);
    addButton_->setVisible(mode == Mode::List && !list_->cards().empty());
}

void ActionsPanel::showList() {
    showMode(Mode::List);
    refresh();
}

void ActionsPanel::addAction() {
    showNotice(QString());
    showMode(Mode::Gallery);
    scrollTo(gallery_);
}

void ActionsPanel::startFromPreset(const automation::Preset& preset) {
    const Json settings = settingsNow();
    // "What it's for" starts empty: routing sends it to the judge, and a preset's blurb says
    // what the preset does, not what you'd use it for.
    Action action;
    action.id = automation::new_id();
    action.name = action_panel::unique_name(preset.name, settings);
    action.kind = preset.kind;
    action.params = preset.params.is_object() ? preset.params : Json::object();
    action.shaping = preset.shaping;
    openEditor(action, true, q(preset.note));
}

void ActionsPanel::edit(const QString& actionId) {
    if (mode_ == Mode::Editor && !editor_->isNew() && editor_->actionId() == actionId) {
        scrollTo(editor_); // Already open, with whatever you changed.
        return;
    }
    const std::optional<Action> action =
        automation::find_action(settingsNow(), qt::toUtf8(actionId));
    if (!action) {
        refresh();
        return;
    }
    if (leaveEditor()) {
        openEditor(*action, false);
    }
}

bool ActionsPanel::leaveEditor() {
    // Before the editor shows another action: save this one's changes, drop them, or stay.
    if (mode_ != Mode::Editor || !editor_->hasUnsavedChanges()) {
        return true;
    }
    const std::string name = editor_->draft().name;
    const auto answer = action_prompts::ask(
        this, QStringLiteral("Unsaved action"),
        QStringLiteral("Save your changes to %1?")
            .arg(name.empty() ? QStringLiteral("this action") : q(name)),
        QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel, QMessageBox::Save);
    if (answer == QMessageBox::Save) {
        return editor_->save();
    }
    return answer == QMessageBox::Discard;
}

void ActionsPanel::openEditor(const Action& action, bool isNew, const QString& note) {
    showNotice(QString());
    editor_->open(action, isNew, note);
    showMode(Mode::Editor);
    scrollTo(editor_);
    editor_->nameEdit()->setFocus();
}

// ---- changes ----

void ActionsPanel::onSaved(const Action& action) {
    showMode(Mode::List);
    refresh();
    showNotice(q(action_panel::saved_note(action)), QStringLiteral("ok"));
    flash(q(action.id));
    Q_EMIT actionsChanged();
}

void ActionsPanel::setActionEnabled(const QString& actionId, bool on) {
    const std::optional<Action> action =
        automation::find_action(settingsNow(), qt::toUtf8(actionId));
    if (!action) {
        return;
    }
    try {
        Action changed = *action;
        changed.enabled = on;
        store_.mutate([&](Json& settings) { automation::save_action(settings, changed); });
    } catch (const std::exception& refused) {
        // A ValueError says why; a file that could not be written is shown the same way rather
        // than ending the program from a slot.
        if (dynamic_cast<const py::ValueError*>(&refused) == nullptr) {
            kLog.warning("Could not save the action ({})", type_display_name(typeid(refused)));
        }
        showNotice(q(refused.what()), QStringLiteral("error"));
        if (ActionCard* card = list_->card(actionId)) {
            card->setEnabledShown(action->enabled);
        }
        return;
    }
    if (ActionCard* card = list_->card(actionId)) {
        card->setEnabledShown(on);
    }
    showNotice(QString());
    Q_EMIT actionsChanged();
}

void ActionsPanel::deleteAction(const QString& actionId) {
    const std::optional<Action> action =
        automation::find_action(settingsNow(), qt::toUtf8(actionId));
    if (!action) {
        return;
    }
    const auto answer =
        action_prompts::ask(this, QStringLiteral("Delete action?"),
                            QStringLiteral("Delete %1 and its shortcut?").arg(q(action->name)),
                            QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (answer != QMessageBox::Yes) {
        return;
    }
    try {
        const std::string id = action->id;
        store_.mutate([&id](Json& settings) { automation::delete_action(settings, id); });
    } catch (const std::exception& failure) {
        kLog.warning("Could not delete the action ({})", type_display_name(typeid(failure)));
        showNotice(QStringLiteral("Couldn't delete %1: %2").arg(q(action->name), q(failure.what())),
                   QStringLiteral("error"));
        return;
    }
    if (action->kind == automation::ActionKind::WEBHOOK) {
        relay_.forgetSecret(actionId);
    }
    refresh();
    showNotice(QStringLiteral("Deleted %1.").arg(q(action->name)), QString());
    Q_EMIT actionsChanged();
}

// ---- drafts and capture ----

bool ActionsPanel::saveDraft(bool ask) {
    if (mode_ != Mode::Editor) {
        return true;
    }
    return editor_->saveDraft(ask);
}

void ActionsPanel::cancelCapture() {
    editor_->cancelCapture();
}

// ---- small touches ----

void ActionsPanel::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    Q_EMIT shown();
}

void ActionsPanel::showNotice(const QString& text, const QString& tone) {
    notice_->setText(text);
    ui::setStyleProperty(notice_, ui::prop::tone, tone);
    notice_->setVisible(!text.isEmpty());
}

void ActionsPanel::flash(const QString& actionId) {
    endFlash();
    ActionCard* card = list_->card(actionId);
    if (card == nullptr) {
        return;
    }
    flashCardId_ = actionId;
    ui::setStyleProperty(card, ui::prop::searchHit, true);
    flashTimer_->start();
    scrollTo(card);
}

void ActionsPanel::endFlash() {
    ActionCard* card = flashCardId_.isEmpty() ? nullptr : list_->card(flashCardId_);
    flashCardId_.clear();
    if (card != nullptr) {
        ui::setStyleProperty(card, ui::prop::searchHit, false);
    }
}

void ActionsPanel::scrollTo(QWidget* widget) {
    revealTarget_ = widget;
    revealTimer_->start();
}

void ActionsPanel::reveal() {
    QPointer<QWidget> target = revealTarget_;
    revealTarget_.clear();
    if (target == nullptr || !target->isVisible()) {
        return;
    }
    QWidget* parent = parentWidget();
    QScrollArea* area = nullptr;
    while (parent != nullptr && area == nullptr) {
        area = qobject_cast<QScrollArea*>(parent);
        parent = parent->parentWidget();
    }
    if (area == nullptr) {
        return;
    }
    if (target->height() + 2 * action_panel::kRevealMargin <= area->viewport()->height()) {
        area->ensureWidgetVisible(target, 0, action_panel::kRevealMargin);
        return;
    }
    // Too tall to show whole (ensureWidgetVisible would centre it): its top, where the title and
    // the first field are.
    const int top = target->mapTo(area->widget(), QPoint(0, 0)).y();
    area->verticalScrollBar()->setValue(top - action_panel::kRevealMargin);
}

} // namespace fv
