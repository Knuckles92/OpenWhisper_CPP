#pragma once

#include <utility>
#include <vector>

#include <QFrame>
#include <QPoint>
#include <QPointer>
#include <QPushButton>
#include <QString>

class QLabel;
class QTimer;

namespace fv {

// A plain field caption that opens help on hover or keyboard activation, and stays open while
// its links are in use.
//
// The caption paints as text (accent while hovered, focused or pressed). After 450 ms of hover,
// or on Space or Enter, it opens a card (object name "fieldHelpCard", a rounded tool window
// owned by the caption's top-level window) under the caption with a title, the wrapped
// description and one link button per (label, destination). A link emits destinationRequested
// with its destination and closes the card. The card closes when the pointer is outside both the
// caption and the card plus 8 px (checked every 200 ms) unless the card is the active window, on
// Escape, on card deactivation and when the caption hides. Accessible name "Help for <caption>".
// Affinity: GUI thread.
class FieldHelp : public QPushButton {
    Q_OBJECT
public:
    // links are (label, destination key).
    FieldHelp(const QString& caption, const QString& title, const QString& description,
              const std::vector<std::pair<QString, QString>>& links, QWidget* parent = nullptr);
    // Deletes the card, which the top-level window owns once it has shown.
    ~FieldHelp() override;
    FieldHelp(const FieldHelp&) = delete;
    FieldHelp& operator=(const FieldHelp&) = delete;

    QString caption() const { return caption_; }
    QFrame* card() const { return card_; }
    const std::vector<QPushButton*>& links() const { return links_; }

    // Sizes, places and shows the card; nothing while the caption is not visible.
    void showHelp();
    // Hides the card unless the pointer is over the caption or the card (plus 8 px) or the card
    // is the active window: the 200 ms dismiss check.
    void dismissIfOutside();

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

Q_SIGNALS:
    void destinationRequested(const QString& destination);

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void focusInEvent(QFocusEvent* event) override;
    void focusOutEvent(QFocusEvent* event) override;
    void hideEvent(QHideEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // Owns the card from the top-level window so it stays above siblings: parented to this
    // caption (a 20 px button inside a scroll area) the recording controls painted over the hint,
    // and move(mapToGlobal(...)) could land it in the middle of the page.
    void hostCard();
    void sizeCard();
    QPoint cardPosition() const;
    void openForKeyboard();
    void navigate(const QString& destination);
    void closeCard();

    QString caption_;
    // The window that owns the card can delete it first.
    QPointer<QFrame> card_;
    QLabel* body_;
    std::vector<QPushButton*> links_;
    QTimer* showTimer_;
    QTimer* dismissTimer_;
};

} // namespace fv
