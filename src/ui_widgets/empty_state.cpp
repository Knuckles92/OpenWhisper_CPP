#include "ui_widgets/empty_state.h"

#include <QVBoxLayout>

#include "ui_widgets/wrapped_label.h"

namespace fv {

EmptyState::EmptyState(const QString& title, const QString& hint, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("emptyState"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(20, 18, 20, 18);
    layout_->setSpacing(4);
    titleLabel_ = new WrappedLabel(QString());
    titleLabel_->setObjectName(QStringLiteral("emptyStateTitle"));
    titleLabel_->setAlignment(Qt::AlignCenter);
    layout_->addWidget(titleLabel_);
    hintLabel_ = new WrappedLabel(QString());
    hintLabel_->setObjectName(QStringLiteral("emptyStateHint"));
    hintLabel_->setAlignment(Qt::AlignCenter);
    layout_->addWidget(hintLabel_);
    setText(title, hint);
}

void EmptyState::setText(const QString& title, const QString& hint) {
    titleLabel_->setText(title);
    hintLabel_->setText(hint);
    hintLabel_->setVisible(!hint.isEmpty());
}

QString EmptyState::text() const {
    QStringList parts;
    for (const QString& part : {titleLabel_->text(), hintLabel_->text()}) {
        if (!part.isEmpty()) {
            parts.push_back(part);
        }
    }
    return parts.join(QLatin1Char(' '));
}

void EmptyState::addAction(QWidget* button, int spacing) {
    if (spacing != 0) {
        layout_->addSpacing(spacing);
    }
    layout_->addWidget(button, 0, Qt::AlignHCenter);
}

} // namespace fv
