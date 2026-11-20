#pragma once

#include <optional>
#include <vector>

#include <QString>
#include <QWidget>

#include "automation/action_model.h"
#include "ui_widgets/setting_tile.h"

class QBoxLayout;
class QGridLayout;
class QLabel;
class QPushButton;
class QTimer;

// Your saved actions as cards: what each does, how to start it, on or off
// (ui_qt/widgets/action_list.py).
namespace fv {

class Button;
class EmptyState;
class SettingsSwitch;

namespace action_list {
// Cards sit two to a row from this page width (at 100 % font scale).
inline constexpr int kTwoColumnsFrom = 640;
// A chip (object name "actionChip") with its `tone`: "key", "voice", "muted" or "".
QLabel* chip(const QString& text, const QString& tone = {});
} // namespace action_list

// Chips side by side at their own width, or one under another when they don't fit.
// Affinity: GUI thread.
class ChipRow : public QWidget {
    Q_OBJECT
public:
    explicit ChipRow(std::vector<QWidget*> chips, QWidget* parent = nullptr);
    bool isStacked() const;

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    std::vector<QWidget*> chips_;
    QBoxLayout* layout_;
};

// One action (tileId "actionCard"): its kind's tinted icon, name, one-line summary, the shortcut
// chip ("No shortcut" when it has none) and the first spoken name, an on/off switch, Edit and
// Delete. A disabled action carries the `off` property, which dims its text. Cards in a row
// share its height; the chips and links stay on the bottom line. Affinity: GUI thread.
class ActionCard : public InfoTile {
    Q_OBJECT
public:
    explicit ActionCard(const automation::Action& action, QWidget* parent = nullptr);

    const automation::Action& action() const { return action_; }
    SettingsSwitch* switchButton() const { return switch_; }
    QLabel* shortcutChip() const { return shortcutChip_; }
    // Null when the action has no spoken name.
    QLabel* spokenChip() const { return spokenChip_; }
    ChipRow* chips() const { return chips_; }
    QPushButton* editLink() const { return edit_; }
    QPushButton* deleteLink() const { return delete_; }

    // Shows the switch on or off without emitting enabledToggled.
    void setEnabledShown(bool on);
    // Its controls in the order they read: the switch at the top, then Edit and Delete.
    std::vector<QWidget*> focusOrder() const;

Q_SIGNALS:
    void editRequested(const QString& id);
    void deleteRequested(const QString& id);
    void enabledToggled(const QString& id, bool on);

private:
    automation::Action action_;
    SettingsSwitch* switch_;
    QLabel* shortcutChip_;
    QLabel* spokenChip_ = nullptr;
    ChipRow* chips_;
    QPushButton* edit_;
    QPushButton* delete_;
};

// The cards, two to a row when there's room, or an invitation to add one (tileId "actionList").
// Cards in a row share its height. Tab reaches the cards in reading order right after the empty
// state's Add button, since cards are made after the rest of the page and would otherwise come
// last. Affinity: GUI thread.
class ActionList : public QWidget {
    Q_OBJECT
public:
    explicit ActionList(QWidget* parent = nullptr);

    EmptyState* emptyState() const { return empty_; }
    Button* emptyAddButton() const { return emptyAdd_; }
    QWidget* gridBox() const { return gridBox_; }
    QGridLayout* grid() const { return grid_; }
    const std::vector<ActionCard*>& cards() const { return cards_; }
    // The card of `id`, or null.
    ActionCard* card(const QString& id) const;
    // Columns as now laid out (0 before the first arrangement).
    int columns() const { return columns_; }

    // Rebuilds the cards; an unchanged list keeps them, with their place, focus and any glow.
    void setActions(const std::vector<automation::Action>& actions);

Q_SIGNALS:
    void addRequested();
    void editRequested(const QString& id);
    void deleteRequested(const QString& id);
    void enabledToggled(const QString& id, bool on);

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void chainFocus();
    void arrange();

    EmptyState* empty_;
    Button* emptyAdd_;
    QWidget* gridBox_;
    QGridLayout* grid_;
    std::vector<ActionCard*> cards_;
    // The actions the cards were made from; nullopt before the first.
    std::optional<std::vector<automation::Action>> actions_;
    int columns_ = 0;
    QTimer* reflowTimer_ = nullptr;
};

} // namespace fv
