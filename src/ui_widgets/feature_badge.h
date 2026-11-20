#pragma once

#include <QFont>
#include <QString>
#include <QWidget>

class QPushButton;

namespace fv {

// A small "New" or "?" chip that explains a feature. It sits on a button's right side, or on
// its own in a layout beside the feature's controls (pass a null button and a parent).
//
// Hover or click the chip for what its button does. The button's own hover keeps showing its
// shortcut pill above it, so the explanation lives here instead of in the button's tooltip. A
// click on the chip shows the explanation and never reaches the button.
//
// On a button it is frosted glass over the button's fill: a tint of the label colour, so it
// reads as part of the button. On its own it is an accent-tinted pill with accent text.
// Affinity: GUI thread.
class FeatureBadge : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QString text READ text)
public:
    explicit FeatureBadge(QPushButton* button, const QString& explanation,
                          QWidget* parent = nullptr);

    // Rich text, so the tooltip wraps instead of running off one line.
    void setExplanation(const QString& explanation);
    // Says "New" for a feature not tried yet, else "?".
    void setNew(bool isNew);
    QString text() const { return text_; }
    bool isCompact() const { return compact_; }

    QSize sizeHint() const override;

protected:
    bool event(QEvent* event) override;
    bool eventFilter(QObject* obj, QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void paintEvent(QPaintEvent* event) override;

private:
    struct PlacedFor {
        int width = -1;
        int height = -1;
        QString buttonText;
        QString badgeText;
        friend bool operator==(const PlacedFor&, const PlacedFor&) = default;
    };
    struct Colors {
        QColor ink;
        QColor fill;
        QColor edge;
    };

    QSize sizeFor(bool compact) const;
    void place();
    Colors colors() const;

    QPushButton* button_;
    QString text_;
    bool hovered_ = false;
    // In a narrow button "New" shrinks to its star, clear of the label.
    bool compact_ = false;
    bool placed_ = false;
    PlacedFor placedFor_;
    QFont wordFont_;
    QFont markFont_;
};

} // namespace fv
