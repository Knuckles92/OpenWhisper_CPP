#include "ui_widgets/update_notice.h"

#include <QSize>

#include "base/identity.h"
#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"

namespace fv {
namespace {

const QString kStyle = QStringLiteral(R"(
        QPushButton#updateNoticeButton {
            background-color: @accent-tint;
            color: @accent-soft;
            border: 1px solid @accent-tint-border;
            border-radius: 11px;
            padding: 2px 10px 2px 8px;
            margin: 0 8px;
            font-size: 12px;
            font-weight: 600;
        }
        QPushButton#updateNoticeButton:hover {
            background-color: @accent-tint-strong;
            border-color: @accent-tint-border-strong;
        }
        QPushButton#updateNoticeButton:pressed {
            background-color: @accent-muted;
        }
    )");

QString product() {
    return QString::fromUtf8(identity().product.data(),
                             static_cast<qsizetype>(identity().product.size()));
}

} // namespace

UpdateNoticeButton::UpdateNoticeButton(QWidget* parent) : QPushButton(parent) {
    setObjectName(QStringLiteral("updateNoticeButton"));
    ui::setLocalStyleSheet(this, kStyle);
    setIcon(ui::tablerIcon(QStringLiteral("download-blue.svg")));
    setIconSize(QSize(14, 14));
    setCursor(Qt::PointingHandCursor);
    hide();
}

void UpdateNoticeButton::showVersion(const QString& version) {
    version_ = version;
    if (version.isEmpty()) {
        hide();
        return;
    }
    syncText();
    setToolTip(
        QStringLiteral("%1 %2 is available. Click to see what's new.").arg(product(), version));
    setAccessibleName(QStringLiteral("Update to %1 %2").arg(product(), version));
    show();
}

void UpdateNoticeButton::setCompact(bool compact) {
    compact_ = compact;
    syncText();
}

void UpdateNoticeButton::syncText() {
    if (!version_.isEmpty()) {
        setText(compact_ ? QStringLiteral("Update") : QStringLiteral("Update to %1").arg(version_));
    }
}

} // namespace fv
