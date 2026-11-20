#pragma once

#include <QString>
#include <QToolButton>

namespace fv {

struct CollapsibleSectionToggleOptions {
    // Shown before the title.
    QString prefix;
    bool expanded = false;
    QString expandTooltip = QStringLiteral("Show section");
    QString collapseTooltip = QStringLiteral("Hide section");
};

// Centred text toggle for expandable and collapsible sections. Shows "{prefix}{title}  {arrow}"
// where the arrow is the state; the whole label is the click target. Affinity: GUI thread.
class CollapsibleSectionToggle : public QToolButton {
    Q_OBJECT
public:
    explicit CollapsibleSectionToggle(const QString& title,
                                      const CollapsibleSectionToggleOptions& options = {},
                                      QWidget* parent = nullptr);

    bool isExpanded() const { return expanded_; }
    void setTitle(const QString& title);
    // Sets the state without necessarily telling anyone: toggledExpanded is emitted only when
    // emitSignal is true and the state changed.
    void setExpanded(bool expanded, bool emitSignal = true);

Q_SIGNALS:
    void toggledExpanded(bool expanded);

private:
    void onToggled(bool checked);
    void updateDisplay();

    QString title_;
    QString prefix_;
    bool expanded_;
    QString expandTooltip_;
    QString collapseTooltip_;
};

} // namespace fv
