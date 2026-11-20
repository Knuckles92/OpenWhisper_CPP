#include "ui_record/compact_record_controller.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "ui_widgets/buttons.h"

namespace fv {

CompactRecordController::CompactRecordController(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("compactRecordController"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 18, 12, 18);
    layout->setSpacing(14);
    layout->addStretch();

    statusLabel_ = new QLabel(QStringLiteral("Ready to record"));
    statusLabel_->setObjectName(QStringLiteral("statusLabel"));
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setFont(QFont(QStringLiteral("Segoe UI"), 13));
    layout->addWidget(statusLabel_);

    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->setContentsMargins(0, 0, 0, 0);
    buttonLayout->setSpacing(8);

    recordButton_ = new SuccessButton(QStringLiteral("Record"));
    stopButton_ = new DangerButton(QStringLiteral("Stop"));
    cancelButton_ = new WarningButton(QStringLiteral("Cancel"));
    for (Button* button : {static_cast<Button*>(recordButton_), static_cast<Button*>(stopButton_),
                           static_cast<Button*>(cancelButton_)}) {
        button->setBaseMinimumSize(88, 44);
        buttonLayout->addWidget(button, 1);
    }

    stopButton_->setActive(false);
    cancelButton_->setActive(false);

    connect(recordButton_, &QPushButton::clicked, this, &CompactRecordController::recordRequested);
    connect(stopButton_, &QPushButton::clicked, this, &CompactRecordController::stopRequested);
    connect(cancelButton_, &QPushButton::clicked, this, &CompactRecordController::cancelRequested);

    layout->addLayout(buttonLayout);
    layout->addStretch();
}

void CompactRecordController::setRecordingState(bool recording) {
    recordButton_->setActive(!recording);
    recordButton_->setText(recording ? QStringLiteral("Recording") : QStringLiteral("Record"));
    stopButton_->setActive(recording);
    cancelButton_->setActive(recording);
}

void CompactRecordController::setStatus(const QString& statusText) {
    statusLabel_->setText(statusText);
}

void CompactRecordController::updateHotkeys(const QString& recordKey, const QString& cancelKey) {
    recordButton_->setHotkey(recordKey);
    stopButton_->setHotkey(recordKey);
    cancelButton_->setHotkey(cancelKey);
}

} // namespace fv
