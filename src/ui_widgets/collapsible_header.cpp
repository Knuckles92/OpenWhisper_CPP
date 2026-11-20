#include "ui_widgets/collapsible_header.h"

#include "ui_theme/local_style.h"

namespace fv {
namespace {

const QString kToggleStyle =
    QStringLiteral("QToolButton { color: @text-secondary; border: none; font-size: 11px; "
                   "font-weight: 600; }"
                   "QToolButton:hover { color: @text; }");

} // namespace

CollapsibleSectionToggle::CollapsibleSectionToggle(const QString& title,
                                                   const CollapsibleSectionToggleOptions& options,
                                                   QWidget* parent)
    : QToolButton(parent), title_(title), prefix_(options.prefix), expanded_(options.expanded),
      expandTooltip_(options.expandTooltip), collapseTooltip_(options.collapseTooltip) {
    setCheckable(true);
    setChecked(expanded_);
    setToolButtonStyle(Qt::ToolButtonTextOnly);
    ui::setLocalStyleSheet(this, kToggleStyle);
    setCursor(Qt::PointingHandCursor);
    updateDisplay();
    connect(this, &QToolButton::toggled, this, &CollapsibleSectionToggle::onToggled);
}

void CollapsibleSectionToggle::setTitle(const QString& title) {
    title_ = title;
    updateDisplay();
}

void CollapsibleSectionToggle::setExpanded(bool expanded, bool emitSignal) {
    if (expanded == expanded_) {
        return;
    }
    expanded_ = expanded;
    blockSignals(true);
    setChecked(expanded);
    blockSignals(false);
    updateDisplay();

    if (emitSignal) {
        Q_EMIT toggledExpanded(expanded);
    }
}

void CollapsibleSectionToggle::onToggled(bool checked) {
    if (checked == expanded_) {
        return;
    }
    expanded_ = checked;
    updateDisplay();
    Q_EMIT toggledExpanded(checked);
}

void CollapsibleSectionToggle::updateDisplay() {
    const QString arrow = expanded_ ? QStringLiteral(u"▾") : QStringLiteral(u"▸");
    setText(prefix_ + title_ + QStringLiteral("  ") + arrow);
    setToolTip(expanded_ ? collapseTooltip_ : expandTooltip_);
}

} // namespace fv
