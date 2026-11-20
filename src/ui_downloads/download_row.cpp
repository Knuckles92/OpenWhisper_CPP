#include "ui_downloads/download_row.h"

#include <QAbstractButton>
#include <QFont>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>

#include "ui_theme/local_style.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"

namespace fv {
namespace {

// Child labels set an explicit transparent background: the global QWidget{background-color}
// rule otherwise paints window-coloured rectangles over the row's fill. The placeholders in
// braces are replaced by rowStyle; no other brace here is one.
constexpr const char* kRowStyle = R"qss(
    QFrame#{row} {
        background-color: @slate-surface;
        border: 1px solid @slate-border;
        border-radius: 12px;
    }
    QFrame#{row}:hover {
        background-color: @slate-surface-hover;
        border: 1px solid @slate-border-strong;
    }
    QFrame#{row}:focus {
        border: 1px solid @accent-tint-border;
        outline: none;
    }{state_rules}
    QFrame#{row}[selected="true"],
    QFrame#{row}[selected="true"]:hover {
        background-color: @accent-tint-strong;
        border: 1px solid @accent-tint-border-strong;
    }
    QLabel#{row}Name {
        color: @slate-text;
        background-color: transparent;
        border: none;
        font-weight: 600;
    }
    QLabel#{row}Summary {
        color: @slate-text-3;
        background-color: transparent;
        border: none;
    }
    QLabel#{row}Size {
        color: @slate-text-2;
        background-color: transparent;
        border: none;
    }
    QLabel#{row}Badge {
        background-color: rgba(@slate-text-3-rgb, 0.12);
        color: @slate-text-2;
        border: 1px solid rgba(@slate-text-3-rgb, 0.28);
        border-radius: 6px;
        padding: 2px 8px;
        font-size: 10px;
        font-weight: 600;
    }
    QLabel#{row}Badge[tone="downloading"] {
        background-color: rgba(@accent-rgb, 0.14);
        color: @accent-soft;
        border: 1px solid rgba(@accent-rgb, 0.28);
    }
    QPushButton#{primary},{secondary}
    QPushButton#{remove} {
        border-radius: 7px;
        padding: 4px 10px;
        font-size: 11px;
        font-weight: 600;
        min-height: 28px;
        max-height: 28px;
    }
    QPushButton#{primary} {
        background-color: rgba(@accent-rgb, 0.18);
        color: @accent-soft;
        border: 1px solid rgba(@accent-rgb, 0.32);
    }
    QPushButton#{primary}:hover {
        background-color: rgba(@accent-rgb, 0.28);
        border: 1px solid rgba(@accent-rgb, 0.5);
    }
    QPushButton#{primary}:disabled {
        background-color: @slate-raised;
        color: @slate-text-disabled;
        border: 1px solid @slate-border-subtle;
    }
    QPushButton#{remove} {
        background-color: transparent;
        color: @danger-text-soft;
        border: 1px solid @slate-border-strong;
    }
    QPushButton#{remove}:hover {
        background-color: rgba(@danger-rgb, 0.14);
        border: 1px solid rgba(@danger-rgb, 0.45);
    }
    QProgressBar#{progress} {
        background-color: @slate-raised;
        border: none;
        border-radius: 3px;
        min-height: 6px;
        max-height: 6px;
        text-align: center;
        color: transparent;
    }
    QProgressBar#{progress}::chunk {
        background-color: @accent;
        border-radius: 3px;
    }
)qss";

QFont segoe(int pointSize) {
    return QFont(QStringLiteral("Segoe UI"), pointSize);
}

} // namespace

QString rowStyle(const QString& row, const QString& primary, const QString& remove,
                 const QString& progress, const QString& secondary, const QString& stateRules,
                 const QString& extra) {
    QString style = QString::fromLatin1(kRowStyle);
    style.replace(QStringLiteral("{row}"), row);
    style.replace(QStringLiteral("{primary}"), primary);
    style.replace(QStringLiteral("{secondary}"),
                  secondary.isEmpty() ? QString()
                                      : QStringLiteral("\n    QPushButton#%1,").arg(secondary));
    style.replace(QStringLiteral("{remove}"), remove);
    style.replace(QStringLiteral("{progress}"), progress);
    style.replace(QStringLiteral("{state_rules}"), stateRules);
    return style + extra;
}

DownloadRow::DownloadRow(const QString& rowId, const QString& objectName, const QString& style,
                         const QString& tooltip, const QString& accessibleName,
                         const QString& accessibleDescription, QWidget* parent)
    : QFrame(parent), rowId_(rowId) {
    setObjectName(objectName);
    ui::setLocalStyleSheet(this, style);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setToolTip(tooltip);
    setAccessibleName(accessibleName);
    setAccessibleDescription(accessibleDescription);
}

void DownloadRow::compactButton(Button* button, int width) {
    button->setBaseMinimumSize(width, kButtonHeight);
    button->setMinimumWidth(width);
    button->setMaximumWidth(width);
    button->setMinimumHeight(kButtonHeight);
    button->setMaximumHeight(kButtonHeight);
    button->setFont(segoe(10));
}

void DownloadRow::setBadge(const QString& text, const QString& tone) {
    badge_->setText(text);
    ui::setStyleProperty(badge_, ui::prop::tone, tone);
}

bool DownloadRow::isActionChild(QWidget* widget) {
    while (widget != nullptr) {
        if (qobject_cast<QAbstractButton*>(widget) != nullptr) {
            return true;
        }
        widget = widget->parentWidget();
    }
    return false;
}

void DownloadRow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QWidget* child = childAt(event->position().toPoint());
        if (!isActionChild(child)) {
            Q_EMIT detailsRequested(rowId_);
            event->accept();
            return;
        }
    }
    QFrame::mouseReleaseEvent(event);
}

void DownloadRow::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
        event->key() == Qt::Key_Space) {
        Q_EMIT detailsRequested(rowId_);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

} // namespace fv
