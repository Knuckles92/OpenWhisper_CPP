#pragma once

#include <QPushButton>
#include <QString>

#include "ui_shell/side_panel_host.h"

namespace fv {

// The narrow tab beside the History sidebar that opens and closes it (history_sidebar.py
// HistoryEdgeTab): "‹" while the panel is closed, "›" while it is open, with a tooltip naming
// the panel and its shortcut. The main window toggles the panel when it is clicked.
// Affinity: GUI thread.
class HistoryEdgeTab : public QPushButton, public SidePanelEdgeTab {
    Q_OBJECT
public:
    static constexpr int kMinimumHeight = 80;

    explicit HistoryEdgeTab(QWidget* parent = nullptr);

    void setExpanded(bool expanded) override;
    // "Ctrl+H" reads " (Ctrl+H)" after the panel name; "" drops it.
    void setShortcutHint(const QString& shortcut) override;
    void setPanelName(const QString& name) override;

    bool isExpanded() const { return expanded_; }

private:
    void updateIcon();

    bool expanded_ = false;
    QString shortcutHint_;
    QString panelName_ = QStringLiteral("History");
};

} // namespace fv
