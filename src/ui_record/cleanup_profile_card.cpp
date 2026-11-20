#include "ui_record/cleanup_profile_card.h"

#include <algorithm>

#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "ui_widgets/engine_field.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

CleanupProfileCard::CleanupProfileCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("engineCard"));
    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(14, 10, 14, 10);
    body->setSpacing(6);

    auto* row = new QHBoxLayout;
    auto* label = new QLabel(QStringLiteral("Cleanup profile"));
    label->setObjectName(QStringLiteral("engineResolvedLabel"));
    combo_ = engineCombo({});
    combo_->setAccessibleName(QStringLiteral("Quick Record cleanup profile"));
    label->setBuddy(combo_);
    row->addWidget(label);
    row->addWidget(combo_, 1);
    manageButton_ = new QPushButton(QStringLiteral(u"Manage…"));
    manageButton_->setToolTip(
        QStringLiteral("Create cleanup profiles and assign recording shortcuts"));
    connect(manageButton_, &QPushButton::clicked, this, &CleanupProfileCard::manageRequested);
    row->addWidget(manageButton_);
    body->addLayout(row);

    hint_ = new WrappedLabel;
    hint_->setTextFormat(Qt::PlainText);
    hint_->setObjectName(QStringLiteral("infoLabel"));
    body->addWidget(hint_);

    connect(combo_, &QComboBox::currentIndexChanged, this, [this](int) { Q_EMIT profileChosen(); });
}

void CleanupProfileCard::setProfiles(const std::vector<text::CleanupProfile>& profiles,
                                     const QString& selectedId) {
    const QSignalBlocker blocker(combo_);
    combo_->clear();
    combo_->addItem(QString::fromLatin1(kStandardName), QString());
    for (const text::CleanupProfile& profile : profiles) {
        combo_->addItem(QString::fromStdString(profile.name), QString::fromStdString(profile.id));
    }
    combo_->setCurrentIndex(std::max(0, combo_->findData(selectedId)));
}

QString CleanupProfileCard::selectedId() const {
    return combo_->currentData().toString();
}

} // namespace fv
