#pragma once

#include <QElapsedTimer>
#include <QLabel>
#include <QList>
#include <QPair>
#include <QString>

class QTimer;

namespace fv {

// (text, emphasized) runs. Emphasized runs render bright and bold once locked.
using DecodeSegments = QList<QPair<QString, bool>>;

// A label whose text resolves into place one character at a time. Characters appear left to
// right; the few just ahead of the resolved run cycle through random glyphs in the accent colour
// before locking. The animation starts when the label is actually shown, because the caller
// often has the text before the label is on screen and an animation nobody saw is a wasted one.
// Call text and clear through DecodeLabel, not QLabel. Affinity: GUI thread.
class DecodeLabel : public QLabel {
    Q_OBJECT
    // Redeclared so a property read gets the finished text, as the QLabel one gets the shown.
    Q_PROPERTY(QString text READ text)
public:
    explicit DecodeLabel(QWidget* parent = nullptr);

    // Sets the text to show. animate: reveal it when the label is next visible, or show it at
    // once.
    void setSegments(const DecodeSegments& segments, bool animate = true);
    // Starts (or restarts) the animation now.
    void reveal();
    void clear();
    // The finished text (rich text), however far the reveal has run.
    QString text() const { return finalHtml_; }
    bool isRevealing() const;
    // The rich text of the reveal at progress (0 to 1): round(length * progress) code points
    // locked, half to even, and the next few scrambling. At 1 or more, the finished text.
    QString frameAt(double progress) const;

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    QString plainText() const;
    void tick();
    // The rich text with the first locked code points of the segments resolved and the next
    // scrambleHead scrambling.
    QString render(int locked, int scrambleHead = 0) const;

    DecodeSegments segments_;
    QString finalHtml_;
    bool pendingReveal_ = false;
    QElapsedTimer startedAt_;
    QTimer* timer_;
};

} // namespace fv
