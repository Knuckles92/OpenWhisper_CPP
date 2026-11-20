#include "ui_history/history_edge_tab.h"

#include "ui_theme/local_style.h"
#include "uimath/window_geometry.h"

namespace fv {
namespace {

const QString kEdgeTabStyle = QStringLiteral(R"qss(
            QPushButton#historyEdgeTab {
                background-color: @surface;
                color: @text-secondary;
                border: 1px solid @border;
                border-right: none;
                border-top-left-radius: 8px;
                border-bottom-left-radius: 8px;
                border-top-right-radius: 0px;
                border-bottom-right-radius: 0px;
                font-size: 16px;
                font-weight: bold;
                padding: 0px;
            }
            QPushButton#historyEdgeTab:hover {
                background-color: @surface-hover;
                color: @text;
            }
            QPushButton#historyEdgeTab:pressed {
                background-color: @bg;
            }
        )qss");

} // namespace

HistoryEdgeTab::HistoryEdgeTab(QWidget* parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("historyEdgeTab"));
    setCursor(Qt::PointingHandCursor);
    setFixedWidth(window_size::kHistoryEdgeTabWidth);
    setMinimumHeight(kMinimumHeight);
    updateIcon();
    ui::setLocalStyleSheet(this, kEdgeTabStyle);
}

void HistoryEdgeTab::setExpanded(bool expanded) {
    expanded_ = expanded;
    updateIcon();
}

void HistoryEdgeTab::setShortcutHint(const QString& shortcut) {
    shortcutHint_ = shortcut.isEmpty() ? QString() : QStringLiteral(" (%1)").arg(shortcut);
    updateIcon();
}

void HistoryEdgeTab::setPanelName(const QString& name) {
    panelName_ = name;
    updateIcon();
}

void HistoryEdgeTab::updateIcon() {
    if (expanded_) {
        setText(QStringLiteral("›"));
        setToolTip(QStringLiteral("Close %1%2").arg(panelName_, shortcutHint_));
    } else {
        setText(QStringLiteral("‹"));
        setToolTip(QStringLiteral("Open %1%2").arg(panelName_, shortcutHint_));
    }
}

} // namespace fv
