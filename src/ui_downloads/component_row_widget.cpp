#include "ui_downloads/component_row_widget.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

// The badge tones only a component row has; the rest come from rowStyle.
constexpr const char* kExtraRules = R"qss(
    QLabel#componentRowBadge[tone="installed"] {
        background-color: rgba(@success-rgb, 0.12);
        color: @success-text-strong;
        border: 1px solid rgba(@success-rgb, 0.28);
    }
    QLabel#componentRowBadge[tone="warning"] {
        background-color: rgba(@warning-rgb, 0.14);
        color: @warning-text;
        border: 1px solid rgba(@warning-rgb, 0.32);
    }
)qss";

QString rowStyleSource() {
    return rowStyle(QStringLiteral("componentRow"), QStringLiteral("componentInstallButton"),
                    QStringLiteral("componentRemoveButton"), QStringLiteral("componentProgress"),
                    QString(), QString(), QString::fromLatin1(kExtraRules));
}

QFont segoe(int pointSize, bool bold = false) {
    QFont font(QStringLiteral("Segoe UI"), pointSize);
    font.setBold(bold);
    return font;
}

} // namespace

ComponentRowWidget::ComponentRowWidget(const QString& componentId, QWidget* parent)
    : DownloadRow(
          componentId, QStringLiteral("componentRow"), rowStyleSource(),
          QStringLiteral("Click to view component details"),
          QStringLiteral("%1 component").arg(componentId),
          QStringLiteral("Open component details. Install and remove actions are separate."),
          parent),
      componentId_(componentId) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(14, 10, 12, 10);
    outer->setSpacing(6);

    auto* row = new QHBoxLayout;
    row->setSpacing(12);

    auto* identity = new QVBoxLayout;
    identity->setSpacing(2);

    nameLabel_ = new ElidingLabel(componentId);
    nameLabel_->setObjectName(QStringLiteral("componentRowName"));
    nameLabel_->setFont(segoe(10, true));
    identity->addWidget(nameLabel_);

    summaryLabel_ = new ElidingLabel(QString());
    summaryLabel_->setObjectName(QStringLiteral("componentRowSummary"));
    summaryLabel_->setFont(segoe(8));
    identity->addWidget(summaryLabel_);

    row->addLayout(identity, 1);

    sizeLabel_ = new ElidingLabel(QString());
    sizeLabel_->setObjectName(QStringLiteral("componentRowSize"));
    sizeLabel_->setFont(segoe(9));
    sizeLabel_->setMinimumWidth(110);
    sizeLabel_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(sizeLabel_);

    badge_ = new QLabel(QString());
    badge_->setObjectName(QStringLiteral("componentRowBadge"));
    badge_->setAlignment(Qt::AlignCenter);
    badge_->setFixedHeight(22);
    row->addWidget(badge_);

    installButton_ = new PrimaryButton(QStringLiteral("Install"));
    installButton_->setObjectName(QStringLiteral("componentInstallButton"));
    compactButton(installButton_, 92);
    connect(installButton_, &QPushButton::clicked, this, [this] { onPrimaryClicked(); });
    row->addWidget(installButton_);

    removeButton_ = new DangerButton(QStringLiteral("Remove"));
    removeButton_->setObjectName(QStringLiteral("componentRemoveButton"));
    compactButton(removeButton_, 80);
    connect(removeButton_, &QPushButton::clicked, this,
            [this] { Q_EMIT removeClicked(componentId_); });
    row->addWidget(removeButton_);

    removeGuard_ = new QTimer(this);
    removeGuard_->setSingleShot(true);
    removeGuard_->setInterval(kRemoveGuardMs);
    connect(removeGuard_, &QTimer::timeout, this, [this] { releaseRemove(); });

    outer->addLayout(row);

    progress_ = new QProgressBar;
    progress_->setObjectName(QStringLiteral("componentProgress"));
    progress_->setTextVisible(false);
    progress_->setRange(0, 100);
    progress_->setValue(0);
    progress_->hide();
    outer->addWidget(progress_);
}

void ComponentRowWidget::onPrimaryClicked() {
    if (installing_) {
        Q_EMIT cancelClicked(componentId_);
    } else {
        Q_EMIT installClicked(componentId_);
    }
}

void ComponentRowWidget::releaseRemove() {
    removeButton_->setEnabled(true);
}

bool ComponentRowWidget::removeGuardActive() const {
    return removeGuard_->isActive();
}

void ComponentRowWidget::updateState(const models::ComponentInfo& info, bool installing) {
    updateState(models::component_row_state(info, installing, installing_), installing);
}

void ComponentRowWidget::updateState(const models::ComponentRowState& state, bool installing) {
    installing_ = installing;
    nameLabel_->setText(qt::toQString(state.name));
    summaryLabel_->setText(qt::toQString(state.summary));
    setBadge(qt::toQString(state.badge_text), qt::toQString(state.badge_tone));

    if (state.progress_visible) {
        progress_->show();
    } else {
        progress_->hide();
        progress_->setValue(0);
    }
    switch (state.remove_guard) {
    case models::RemoveGuard::Keep:
        break;
    case models::RemoveGuard::Start:
        removeButton_->setEnabled(false);
        removeGuard_->start();
        break;
    case models::RemoveGuard::Release:
        if (!removeGuard_->isActive()) {
            removeButton_->setEnabled(true);
        }
        break;
    }

    if (state.size_text) {
        sizeLabel_->setText(qt::toQString(*state.size_text));
    }
    if (state.install_text) {
        installButton_->setText(qt::toQString(*state.install_text));
    }
    if (state.install_enabled) {
        installButton_->setEnabled(*state.install_enabled);
    }
    if (state.install_tooltip) {
        installButton_->setToolTip(qt::toQString(*state.install_tooltip));
    }
    if (state.install_visible) {
        installButton_->setVisible(*state.install_visible);
    }
    removeButton_->setVisible(state.remove_visible);
}

void ComponentRowWidget::setProgress(std::string_view phase, std::int64_t done,
                                     std::int64_t total) {
    if (!installing_) {
        return;
    }
    setBadge(qt::toQString(models::component_phase_badge(phase)), QStringLiteral("downloading"));
    const models::RowProgress progress = models::component_row_progress(phase, done, total);
    if (progress.size_text) {
        sizeLabel_->setText(qt::toQString(*progress.size_text));
    }
    if (progress.indeterminate) {
        // Qt animates a busy bar when the minimum and maximum are both 0.
        progress_->setRange(0, 0);
        return;
    }
    progress_->setRange(0, 100);
    progress_->setValue(progress.percent);
}

} // namespace fv
