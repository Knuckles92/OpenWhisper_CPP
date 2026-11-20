#pragma once

#include <QString>
#include <QWidget>

class QHBoxLayout;
class QLabel;
class QPropertyAnimation;
class QVBoxLayout;

namespace fv {

class CollapsibleSectionToggle;

// Card container styled by the application theme (object name "card"). Affinity: GUI thread.
class Card : public QWidget {
    Q_OBJECT
public:
    explicit Card(QWidget* parent = nullptr);

    // The card's own layout (Python's Card.layout attribute).
    QVBoxLayout* cardLayout() const { return layout_; }

protected:
    QVBoxLayout* layout_;
};

// Control panel with buttons and controls (object name "controlPanel").
class ControlPanel : public QWidget {
    Q_OBJECT
public:
    explicit ControlPanel(QWidget* parent = nullptr);

    QHBoxLayout* panelLayout() const { return layout_; }

private:
    QHBoxLayout* layout_;
};

// Card with a header section. When collapsible, a chevron toggle is shown in the header and the
// body lives in contentWidget(), which hides on collapse so the card shrinks to its header.
class HeaderCard : public Card {
    Q_OBJECT
public:
    explicit HeaderCard(const QString& title = {}, bool collapsible = false,
                        QWidget* parent = nullptr);

    bool collapsible() const { return collapsible_; }
    QHBoxLayout* headerLayout() const { return headerLayout_; }
    QLabel* titleLabel() const { return titleLabel_; }
    CollapsibleSectionToggle* sectionToggle() const { return sectionToggle_; }
    // The body of a collapsible card; null otherwise.
    QWidget* contentWidget() const { return contentWidget_; }
    QVBoxLayout* contentLayout() const { return contentLayout_; }

    void addContentWidget(QWidget* widget);
    void setTitle(const QString& title);
    // Whether the card body is currently collapsed.
    bool isCollapsed() const { return collapsed_; }
    // Height of the body captured at the last collapse (the resize delta).
    int contentHeight() const { return contentHeight_; }

    // Collapses or expands the body. With emitSignal the card animates (300 ms) and emits
    // toggled; without it (restoring saved state) the change is applied at once and silent, so
    // it does not trigger a window resize.
    void setCollapsed(bool collapsed, bool emitSignal = true);

Q_SIGNALS:
    // The collapsed state changed (true == collapsed).
    void toggled(bool collapsed);

private:
    void onSectionToggled(bool expanded);
    void applyCollapsedImmediate(bool collapsed);
    QPropertyAnimation* contentAnimation();
    int contentNaturalHeight();
    void applyCardSizePolicy(bool collapsed);
    void animateContentVisibility(bool collapsed);

    bool collapsible_;
    bool collapsed_ = false;
    int contentHeight_ = 0;
    QHBoxLayout* headerLayout_;
    QLabel* titleLabel_ = nullptr;
    CollapsibleSectionToggle* sectionToggle_ = nullptr;
    QWidget* contentWidget_ = nullptr;
    QVBoxLayout* contentLayout_ = nullptr;
    QPropertyAnimation* contentAnim_ = nullptr;
};

} // namespace fv
