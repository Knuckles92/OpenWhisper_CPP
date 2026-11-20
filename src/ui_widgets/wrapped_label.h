#pragma once

#include <QLabel>
#include <QString>

namespace fv {

// _with_break_points: lets a long unbroken value wrap, with a zero-width space after each
// backslash and every 8 characters of a run of 16 or more with no space, hyphen or slash to
// wrap at (hashes, long names, path segments). Counted in code points.
QString withBreakPoints(const QString& text);

// Word-wrapped label whose size hints match the text at its own width. QLabel derives the size
// hint of wrapped text from a heuristic width, so its height rarely matches what the text needs
// once a layout has given it a width; reporting the height for the current width keeps the
// enclosing layout's minimum honest.
//
// breakLongWords is for values that are not prose (a commit hash, a model folder path): QLabel
// cannot wrap a word wider than itself, so such runs get invisible break points while text(),
// selectedText() and anything copied still return the original text. Call setText and text
// through WrappedLabel, not QLabel. Affinity: GUI thread.
class WrappedLabel : public QLabel {
    Q_OBJECT
    // Redeclared so a property read gets the text as set, as the QLabel one gets the shown one.
    Q_PROPERTY(QString text READ text WRITE setText)
public:
    explicit WrappedLabel(const QString& text = {}, QWidget* parent = nullptr,
                          bool breakLongWords = false);

    void setText(const QString& text);
    QString text() const;
    QString selectedText() const;

    bool hasHeightForWidth() const override { return false; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    void cleanClipboard();

    bool breakLongWords_;
    QString plain_;
};

} // namespace fv
