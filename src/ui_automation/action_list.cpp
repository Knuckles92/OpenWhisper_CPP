#include "ui_automation/action_list.h"

#include <algorithm>

#include <QBoxLayout>
#include <QEvent>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "automation/action_copy.h"
#include "hotkeys/grammar.h"
#include "pyport/number.h"
#include "ui_automation/action_fields.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/settings_switch.h"

namespace fv {
namespace {

namespace copy = automation::action_copy;

QString kindIcon(const std::string& kind) {
    return qt::toQString(copy::label_for(copy::KIND_ICONS(), kind));
}

} // namespace

QLabel* action_list::chip(const QString& text, const QString& tone) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("actionChip"));
    ui::setStyleProperty(label, ui::prop::tone, tone);
    return label;
}

// ---- ChipRow ----

ChipRow::ChipRow(std::vector<QWidget*> chips, QWidget* parent)
    : QWidget(parent), chips_(std::move(chips)) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionRow"));
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(6);
    for (QWidget* item : chips_) {
        layout_->addWidget(item, 0, Qt::AlignLeft);
    }
    layout_->addStretch(1);
}

bool ChipRow::isStacked() const {
    return layout_->direction() == QBoxLayout::TopToBottom;
}

void ChipRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    int needed = 0;
    for (const QWidget* item : chips_) {
        needed += item->sizeHint().width();
    }
    needed += layout_->spacing() * (static_cast<int>(chips_.size()) - 1);
    const QBoxLayout::Direction direction =
        width() < needed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
    }
}

// ---- ActionCard ----

ActionCard::ActionCard(const automation::Action& action, QWidget* parent)
    : InfoTile(qt::toQString(action.name), qt::toQString(copy::summary(action)),
               ui::designIcon(kindIcon(action.kind)), parent),
      action_(action) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionCard"));
    action_fields::tintIcon(this, kindIcon(action.kind));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    const QString name = qt::toQString(action.name);
    switch_ = new SettingsSwitch;
    switch_->setAccessibleName(name + QStringLiteral(" on"));
    switch_->setChecked(action.enabled);
    connect(switch_, &SettingsSwitch::toggled, this,
            [this](bool on) { Q_EMIT enabledToggled(qt::toQString(action_.id), on); });
    addTrailing(switch_);

    const QString shortcut = qt::toQString(
        hotkeys::format_hotkey_display(action.hotkey, hotkeys::platform_vocabulary()));
    shortcutChip_ =
        action_list::chip(shortcut.isEmpty() ? QStringLiteral("No shortcut") : shortcut,
                          shortcut.isEmpty() ? QStringLiteral("muted") : QStringLiteral("key"));
    shortcutChip_->setAccessibleName(shortcut.isEmpty() ? QStringLiteral("No shortcut")
                                                        : QStringLiteral("Shortcut ") + shortcut);
    const QString spoken = qt::toQString(copy::spoken_hint(action));
    std::vector<QWidget*> chips{shortcutChip_};
    if (!spoken.isEmpty()) {
        spokenChip_ = action_list::chip(spoken, QStringLiteral("voice"));
        chips.push_back(spokenChip_);
    }
    chips_ = new ChipRow(std::move(chips));

    auto* links = new QWidget;
    ui::setStyleProperty(links, ui::prop::tileId, QStringLiteral("actionRow"));
    auto* linkRow = new QHBoxLayout(links);
    linkRow->setContentsMargins(0, 0, 0, 0);
    linkRow->setSpacing(14);
    edit_ = action_fields::pageLink(QStringLiteral("Edit"), QStringLiteral("Change ") + name);
    edit_->setAccessibleName(QStringLiteral("Edit ") + name);
    connect(edit_, &QPushButton::clicked, this,
            [this] { Q_EMIT editRequested(qt::toQString(action_.id)); });
    delete_ = action_fields::pageLink(QStringLiteral("Delete"), QStringLiteral("Delete ") + name);
    delete_->setAccessibleName(QStringLiteral("Delete ") + name);
    ui::setStyleProperty(delete_, ui::prop::tone, QStringLiteral("danger"));
    connect(delete_, &QPushButton::clicked, this,
            [this] { Q_EMIT deleteRequested(qt::toQString(action_.id)); });
    linkRow->addWidget(edit_);
    linkRow->addWidget(delete_);
    // Cards in a row share its height; the chips and links stay on the bottom line.
    static_cast<QBoxLayout*>(layout())->insertStretch(1, 1);
    addBody(new Reflow(chips_, links, std::nullopt, nullptr, Qt::AlignLeft));
    ui::setStyleProperty(this, ui::prop::off, !action.enabled);
}

void ActionCard::setEnabledShown(bool on) {
    {
        const QSignalBlocker blocker(switch_);
        switch_->setChecked(on);
    }
    ui::setStyleProperty(this, ui::prop::off, !on);
}

std::vector<QWidget*> ActionCard::focusOrder() const {
    return {switch_, edit_, delete_};
}

// ---- ActionList ----

ActionList::ActionList(QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionList"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    empty_ = new EmptyState(QStringLiteral("No actions yet"),
                            QStringLiteral("Send what you say to a chat, a webhook or your own "
                                           "program, with its own shortcut or by name."));
    emptyAdd_ = compactPrimaryButton(new Button(QStringLiteral("Add an action")));
    fitCompactButton(emptyAdd_);
    connect(emptyAdd_, &QPushButton::clicked, this, &ActionList::addRequested);
    empty_->addAction(emptyAdd_);
    layout->addWidget(empty_);

    gridBox_ = new QWidget;
    ui::setStyleProperty(gridBox_, ui::prop::tileId, QStringLiteral("actionRow"));
    grid_ = new QGridLayout(gridBox_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setHorizontalSpacing(12);
    grid_->setVerticalSpacing(12);
    layout->addWidget(gridBox_);
    reflowTimer_ = new QTimer(this);
    reflowTimer_->setSingleShot(true);
    connect(reflowTimer_, &QTimer::timeout, this, &ActionList::arrange);
    setActions({});
}

ActionCard* ActionList::card(const QString& id) const {
    for (ActionCard* card : cards_) {
        if (qt::toQString(card->action().id) == id) {
            return card;
        }
    }
    return nullptr;
}

void ActionList::setActions(const std::vector<automation::Action>& actions) {
    if (actions_ && *actions_ == actions) {
        // Nothing changed: the cards keep their place, their focus and any glow.
        return;
    }
    actions_ = actions;
    for (ActionCard* card : cards_) {
        grid_->removeWidget(card);
        card->hide();
        card->deleteLater();
    }
    cards_.clear();
    for (const automation::Action& action : actions) {
        auto* card = new ActionCard(action);
        connect(card, &ActionCard::editRequested, this, &ActionList::editRequested);
        connect(card, &ActionCard::deleteRequested, this, &ActionList::deleteRequested);
        connect(card, &ActionCard::enabledToggled, this, &ActionList::enabledToggled);
        cards_.push_back(card);
    }
    empty_->setVisible(cards_.empty());
    gridBox_->setVisible(!cards_.empty());
    columns_ = 0;
    arrange();
    chainFocus();
}

void ActionList::chainFocus() {
    QWidget* previous = emptyAdd_;
    for (ActionCard* card : cards_) {
        for (QWidget* widget : card->focusOrder()) {
            QWidget::setTabOrder(previous, widget);
            previous = widget;
        }
    }
}

void ActionList::arrange() {
    const int from = static_cast<int>(
        py::round_half_even(action_list::kTwoColumnsFrom * ui::currentUiFontScale()));
    int columns = width() >= from ? 2 : 1;
    columns = std::min(columns, std::max(1, static_cast<int>(cards_.size())));
    const bool placed = std::all_of(cards_.begin(), cards_.end(),
                                    [this](ActionCard* card) { return grid_->indexOf(card) >= 0; });
    if (columns == columns_ && placed) {
        return;
    }
    columns_ = columns;
    for (ActionCard* card : cards_) {
        grid_->removeWidget(card);
    }
    for (std::size_t index = 0; index < cards_.size(); ++index) {
        const int at = static_cast<int>(index);
        grid_->addWidget(cards_[index], at / columns, at % columns);
        cards_[index]->show();
    }
    for (int column = 0; column < 2; ++column) {
        grid_->setColumnStretch(column, column < columns ? 1 : 0);
    }
}

void ActionList::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrange();
}

void ActionList::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        reflowTimer_ != nullptr) {
        columns_ = 0;
        reflowTimer_->start(0);
    }
}

} // namespace fv
