#include "ui_downloads/batch_download_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "models/consent_text.h"
#include "ui_theme/app_icon.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

BatchDownloadDialog::BatchDownloadDialog(const std::vector<std::string>& modelNames,
                                         const QString& destination, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(qt::toQString(models::kBatchWindowTitle));
    setWindowIcon(ui::appIcon());
    setObjectName(QStringLiteral("batchDownloadDialog"));
    setModal(true);
    setMinimumWidth(420);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(22, 18, 22, 16);
    layout->setSpacing(10);

    const std::size_t count = modelNames.size();
    titleLabel_ = new QLabel(qt::toQString(models::batch_download_title(count)));
    titleLabel_->setObjectName(QStringLiteral("downloadsTitle"));
    layout->addWidget(titleLabel_);

    modelListLabel_ = new QLabel(qt::toQString(models::batch_model_list(modelNames)));
    modelListLabel_->setObjectName(QStringLiteral("batchModelList"));
    modelListLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(modelListLabel_);

    totalLabel_ = new QLabel(qt::toQString(models::batch_total_text(modelNames)));
    totalLabel_->setObjectName(QStringLiteral("batchTotalLabel"));
    layout->addWidget(totalLabel_);

    auto* destinationCaption = new QLabel(QStringLiteral("DESTINATION"));
    destinationCaption->setObjectName(QStringLiteral("downloadsEyebrow"));
    layout->addWidget(destinationCaption);
    // Eliding keeps a long path from stretching the window; the full path stays on the tooltip.
    destinationPathLabel_ = new ElidingLabel(destination);
    destinationPathLabel_->setObjectName(QStringLiteral("batchDestPath"));
    destinationPathLabel_->setToolTip(destination);
    destinationPathLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    layout->addWidget(destinationPathLabel_);

    auto* note = new WrappedLabel(qt::toQString(models::batch_note()));
    note->setObjectName(QStringLiteral("downloadsSourceNote"));
    layout->addWidget(note);

    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(8);
    buttons->addStretch();
    auto* cancelButton = new Button(QStringLiteral("Cancel"));
    fitCompactButton(cancelButton, 100);
    connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(cancelButton);
    auto* downloadButton = new PrimaryButton(qt::toQString(models::batch_download_button(count)));
    fitCompactButton(downloadButton, 0);
    connect(downloadButton, &QPushButton::clicked, this, &QDialog::accept);
    buttons->addWidget(downloadButton);
    layout->addLayout(buttons);
}

} // namespace fv
