#include "ui_remote/remote_name_dialog.h"

#include <QDialogButtonBox>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

#include "remote/wire.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

RemoteNameDialog::RemoteNameDialog(const QString& title, const QString& text,
                                   const QString& current, const QString& own, QWidget* parent)
    : QDialog(parent) {
    setObjectName(QStringLiteral("remoteRenameDialog"));
    setWindowTitle(title);
    setMinimumWidth(360);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 18, 18, 18);
    layout->setSpacing(12);
    layout->addWidget(new WrappedLabel(text));
    edit_ = new QLineEdit(current);
    edit_->setObjectName(QStringLiteral("remoteRenameEdit"));
    edit_->setAccessibleName(QStringLiteral("Name"));
    edit_->setPlaceholderText(own);
    edit_->setMaxLength(static_cast<int>(remote::MAX_NAME));
    edit_->setMinimumHeight(36);
    edit_->selectAll();
    layout->addWidget(edit_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    rename_ = buttons->addButton(QStringLiteral("Rename"), QDialogButtonBox::AcceptRole);
    rename_->setObjectName(QStringLiteral("primaryButton"));
    rename_->setDefault(true);
    connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}

std::optional<QString> RemoteNameDialog::ask(QWidget* parent, const QString& title,
                                             const QString& text, const QString& current,
                                             const QString& own) {
    RemoteNameDialog dialog(title, text, current, own, parent);
    if (dialog.exec() != QDialog::Accepted) {
        return std::nullopt;
    }
    return dialog.edit()->text();
}

} // namespace fv
