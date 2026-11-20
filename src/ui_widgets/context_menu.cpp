#include "ui_widgets/context_menu.h"

#include <QMenu>

#include "ui_theme/local_style.h"

namespace fv {
namespace {

const QString kContextMenuStyleSheet = QStringLiteral(R"(
    QMenu {
        background-color: rgba(@surface-rgb, 0.95);
        color: @text;
        border: 1px solid rgba(@overlay-rgb, 0.1);
        border-radius: 10px;
        padding: 6px;
    }
    QMenu::item {
        padding: 8px 28px 8px 14px;
        border-radius: 6px;
        font-size: 13px;
    }
    QMenu::item:selected {
        background-color: @accent;
        color: @on-accent;
    }
    QMenu::separator {
        background-color: rgba(@overlay-rgb, 0.08);
        height: 1px;
        margin: 4px 8px;
    }
    QMenu::item:disabled {
        color: @text-secondary;
    }
)");

} // namespace

QMenu* contextMenu(QWidget* parent) {
    auto* menu = new QMenu(parent);
    ui::setLocalStyleSheet(menu, kContextMenuStyleSheet);
    return menu;
}

} // namespace fv
