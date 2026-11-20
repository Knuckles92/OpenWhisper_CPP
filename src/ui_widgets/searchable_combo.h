#pragma once

#include <QString>

#include "ui_widgets/no_wheel.h"

class QKeyEvent;
class QLabel;

namespace fv {

// Editable combo box whose own dropdown filters as the user types.
//
// A plain editable QComboBox cannot filter its dropdown: keys typed while the dropdown is open
// go to the line edit (appending to the current text), and a QCompleter popup cannot appear
// while the combo's dropdown holds the input grab. This one hides the rows that do not match
// (fuzzy_score) in the combo's own view and highlights the best match, so typing narrows the
// visible dropdown directly whether it was opened by clicking or by typing. The first keystroke
// after opening replaces the text; Up and Down wrap over the visible rows; Enter picks the
// highlighted row and emits activated and textActivated itself when the platform did not;
// Escape closes. Otherwise it behaves like a plain editable QComboBox (clear, addItems,
// setCurrentText, currentText), and values not in the list stay allowed (NoInsert).
//
// It can also carry a badge (object name "searchableComboBadge"): a short marker drawn inside
// the closed combo, left of the arrow, for state that belongs to the shown value itself.
// Affinity: GUI thread.
class SearchableComboBox : public NoWheelComboBox {
    Q_OBJECT
public:
    // The badge's height, and the gap it keeps from the editor text.
    static constexpr int kBadgeHeight = 18;
    static constexpr int kBadgeGap = 8;

    explicit SearchableComboBox(QWidget* parent = nullptr);

    // Shows text (stripped) as the badge; an empty text hides it.
    void setBadge(const QString& text);
    // The badge text, empty when no badge shows.
    QString badgeText() const;
    QLabel* badge() const { return badge_; }

    // A fresh open (mouse or keyboard, not a re-show caused by typing) restores the full list
    // and arms the next keystroke to replace the current text with a new search.
    void showPopup() override;

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void layoutBadge();
    // Up, Down, Enter and Escape while the filtered dropdown is open; true when handled.
    bool handlePopupNavigation(QKeyEvent* event);
    void finishKeyActivation(int row, const QString& text, int activationCount,
                             int textActivationCount);
    void onTextEdited(const QString& text);
    void applyFilter(const QString& text);
    // Highlights a row without changing the combo's chosen value.
    void setHighlightedRow(int row);
    void installPopupKeyFilters();
    bool isPopupWidget(const QObject* object) const;
    void forwardToLineEdit(const QKeyEvent* event);

    // Set while the popup is shown because of typing, so showPopup keeps the active filter.
    bool filtering_ = false;
    // Set once the user has started typing a search, so only the first keystroke replaces.
    bool searchStarted_ = false;
    bool popupFilterInstalled_ = false;
    int activationCount_ = 0;
    int textActivationCount_ = 0;
    QLabel* badge_;
};

} // namespace fv
