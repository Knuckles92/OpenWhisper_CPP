#pragma once

#include <QLabel>
#include <QString>

namespace fv {

// Single-line label whose width floor ignores how long its text is. A plain QLabel reports the
// full width of its text as its minimum, so one long secondary line raises the minimum width of
// every ancestor up to the window. This caps that floor, elides to whatever width it is given,
// and keeps the full text as a tooltip. Call setText and text through ElidingLabel, not QLabel.
// Affinity: GUI thread.
class ElidingLabel : public QLabel {
    Q_OBJECT
    // Redeclared so a property read gets the full text, as the QLabel one gets the shown one.
    Q_PROPERTY(QString text READ text WRITE setText)
public:
    // Room for a few characters plus the ellipsis, excluding any padding or border the
    // stylesheet draws around the text.
    static constexpr int kMinimumWidth = 56;

    explicit ElidingLabel(const QString& text = {}, Qt::TextElideMode mode = Qt::ElideRight,
                          QWidget* parent = nullptr);

    void setText(const QString& text);
    // The full text, not the elided form being displayed.
    QString text() const { return fullText_; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void resizeEvent(QResizeEvent* event) override;

    // Shows fullText_ elided to the room the label has.
    virtual void applyElide();

    QString fullText_;

private:
    // Horizontal space QLabel keeps around the text, mirroring its sizeHint: a stylesheet
    // padding or border lands in the contents margins and also gives the label a frame, and
    // QLabel then applies an implicit indent of one "x" advance on the aligned side.
    int chromeWidth() const;

    Qt::TextElideMode mode_;
};

} // namespace fv
