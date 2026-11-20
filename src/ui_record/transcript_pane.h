#pragma once

#include <QFrame>
#include <QPointer>

namespace fv {

// The transcript's painted surface, with room for one floating corner action. The corner widget
// is parented to the pane and laid over the text's top-right padding rather than given a row of
// its own, so it costs the preview no height. The Fixed / Raw switch row ends in a stretch, so
// the two never meet when both are shown. Affinity: GUI thread.
class TranscriptPane : public QFrame {
    Q_OBJECT
public:
    // Clears the text edit's vertical scrollbar as well as its padding.
    static constexpr int kCornerInsetX = 14;
    static constexpr int kCornerInsetY = 10;

    explicit TranscriptPane(QWidget* parent = nullptr);

    // Takes widget as a child, raised and placed at the pane's top-right corner.
    void setCornerWidget(QWidget* widget);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    void placeCornerWidget();

    QPointer<QWidget> cornerWidget_;
};

} // namespace fv
