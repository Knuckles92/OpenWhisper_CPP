#pragma once

#include <QLayout>
#include <QList>

namespace fv {

// Lays children out left to right and wraps them, each at its own width, like words in a
// paragraph. Items on one line are centred on it, so a chip sits level with the taller buttons
// beside it. The layout has no margins and ignores any it is given; its size hint is its
// minimum, the largest item's minimum. Affinity: GUI thread.
class FlowLayout : public QLayout {
    Q_OBJECT
public:
    explicit FlowLayout(QWidget* parent = nullptr, int spacing = 6);
    ~FlowLayout() override;
    FlowLayout(const FlowLayout&) = delete;
    FlowLayout& operator=(const FlowLayout&) = delete;

    void addItem(QLayoutItem* item) override;
    int count() const override;
    QLayoutItem* itemAt(int index) const override;
    QLayoutItem* takeAt(int index) override;
    Qt::Orientations expandingDirections() const override;
    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    void setGeometry(const QRect& rect) override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;

    // The width that shows every item on one line.
    int lineWidth() const;

private:
    // Places the items in rect when move is set; returns the height the lines take.
    int arrange(const QRect& rect, bool move) const;

    QList<QLayoutItem*> items_;
    int gap_;
};

} // namespace fv
