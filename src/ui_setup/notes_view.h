#pragma once

#include <QTextBrowser>

class QPainter;
class QRectF;

namespace fv {

// Release notes that fade into their card wherever they are clipped. The view is capped at a
// height, so a long changelog is cut mid-line: a fade reads as "there is more", where a sliced
// row of glyphs reads as a rendering fault. The fade is drawn in the card's own colour
// ("surface-sunken", the fill of QFrame#updateNotesCard in theme.qss), at the top once the
// text has scrolled and at the bottom while more follows.
// Affinity: GUI thread.
class NotesView : public QTextBrowser {
    Q_OBJECT
public:
    // Height of a fade band in pixels; a viewport shorter than twice this gets bands of half
    // its height.
    static constexpr int kFadeHeight = 22;

    explicit NotesView(QWidget* parent = nullptr);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static void paintFade(QPainter& painter, const QRectF& rect, bool atTop);
};

} // namespace fv
