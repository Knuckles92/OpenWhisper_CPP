#include "ui_downloads/hf_consent_dialog.h"

#include <algorithm>
#include <utility>

#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScreen>
#include <QScrollArea>
#include <QScrollBar>
#include <QShowEvent>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "models/hf_access_policy.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"

namespace fv {

HuggingFaceConsentDialog::HuggingFaceConsentDialog(std::string modelName, std::string policy,
                                                   bool envBlocked, models::RuntimeNeeds needs,
                                                   QWidget* parent)
    : QDialog(parent), modelName_(std::move(modelName)), policy_(std::move(policy)),
      envBlocked_(envBlocked), needs_(std::move(needs)) {
    setWindowTitle(qt::toQString(models::kConsentWindowTitle));
    setMinimumWidth(460);
    setModal(true);

    contentLayout_ = new QVBoxLayout(this);
    QVBoxLayout* layout = contentLayout_;
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);

    titleLabel_ = new QLabel(qt::toQString(models::consent_title(modelName_)));
    titleLabel_->setObjectName(QStringLiteral("headerLabel"));
    titleLabel_->setWordWrap(true);
    layout->addWidget(titleLabel_);

    // Publisher, licence, version and the security notice run long: they scroll, so the title
    // and the buttons stay in view on a 640 px screen.
    bodyHost_ = new QWidget;
    bodyHost_->setObjectName(QStringLiteral("consentBody"));
    auto* bodyLayout = new QVBoxLayout(bodyHost_);
    bodyLayout->setContentsMargins(0, 0, 0, 0);
    bodyLayout->setSpacing(12);

    auto* body = new QLabel(bodyText());
    body->setObjectName(QStringLiteral("consentBodyLabel"));
    body->setWordWrap(true);
    bodyLayout->addWidget(body);

    auto* storageNote = new QLabel(qt::toQString(models::consent_storage_note()));
    storageNote->setObjectName(QStringLiteral("infoLabel"));
    storageNote->setWordWrap(true);
    bodyLayout->addWidget(storageNote);

    bodyScroll_ = new QScrollArea;
    bodyScroll_->setObjectName(QStringLiteral("consentBodyScroll"));
    bodyScroll_->setFrameShape(QFrame::NoFrame);
    bodyScroll_->setWidgetResizable(true);
    bodyScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    bodyScroll_->setWidget(bodyHost_);
    layout->addWidget(bodyScroll_, 1);

    layout->addSpacing(8);
    auto* buttonLayout = new QHBoxLayout;
    buttonLayout->setSpacing(8);
    buttonLayout->addStretch();

    if (envBlocked_) {
        auto* closeButton = new Button(QStringLiteral("Close"));
        closeButton->setObjectName(QStringLiteral("consentCloseButton"));
        connect(closeButton, &QPushButton::clicked, this, &QDialog::reject);
        buttonLayout->addWidget(closeButton);
        closeButton->setDefault(true);
        safeButton_ = closeButton;
    } else {
        auto* cancelButton = new Button(QStringLiteral("Cancel"));
        cancelButton->setObjectName(QStringLiteral("consentCancelButton"));
        connect(cancelButton, &QPushButton::clicked, this, &QDialog::reject);
        buttonLayout->addWidget(cancelButton);

        if (policy_ == models::hf_access_policy::kNever) {
            auto* settingsButton = new Button(QStringLiteral("Open Settings"));
            settingsButton->setObjectName(QStringLiteral("consentOpenSettingsButton"));
            connect(settingsButton, &QPushButton::clicked, this,
                    [this] { finish(models::ConsentAction::OpenSettings); });
            buttonLayout->addWidget(settingsButton);
        } else {
            auto* alwaysButton = new Button(QStringLiteral("Always allow"));
            alwaysButton->setObjectName(QStringLiteral("consentAlwaysAllowButton"));
            alwaysButton->setAutoDefault(false);
            connect(alwaysButton, &QPushButton::clicked, this,
                    [this] { finish(models::ConsentAction::AlwaysAllow); });
            buttonLayout->addWidget(alwaysButton);
        }

        auto* downloadButton = new PrimaryButton(QStringLiteral("Download once"));
        ui::setStyleProperty(downloadButton, ui::prop::buttonId,
                             QStringLiteral("consentDownloadOnceButton"));
        downloadButton->setAutoDefault(false);
        connect(downloadButton, &QPushButton::clicked, this,
                [this] { finish(models::ConsentAction::DownloadOnce); });
        buttonLayout->addWidget(downloadButton);
        // Enter or Space on a dialog that just popped up must not start a multi-gigabyte
        // download.
        cancelButton->setDefault(true);
        safeButton_ = cancelButton;
    }

    layout->addLayout(buttonLayout);
    safeButton_->setFocus();
    resize(kWidth, sizeHint().height());
}

QString HuggingFaceConsentDialog::bodyText() const {
    return qt::toQString(models::consent_body(modelName_, policy_, envBlocked_, needs_));
}

void HuggingFaceConsentDialog::setAvailableHeight(std::function<int()> availableHeight) {
    availableHeight_ = std::move(availableHeight);
}

int HuggingFaceConsentDialog::availableHeight() const {
    if (availableHeight_) {
        return availableHeight_();
    }
    const QWidget* owner = parentWidget();
    const QScreen* screen = owner != nullptr ? owner->screen() : nullptr;
    if (screen == nullptr) {
        screen = this->screen();
    }
    if (screen == nullptr) {
        return 10'000;
    }
    return screen->availableGeometry().height();
}

void HuggingFaceConsentDialog::fitBody() {
    QVBoxLayout* layout = contentLayout_;
    const QMargins margins = layout->contentsMargins();
    const int width = std::max(this->width(), minimumWidth());
    const int inner = width - margins.left() - margins.right();
    int content = bodyHost_->heightForWidth(inner);
    if (content < 0) {
        content = bodyHost_->sizeHint().height();
    }
    // Everything but the notice: the title, spacing, buttons and margins.
    bodyScroll_->setFixedHeight(0);
    layout->activate();
    const int outside = layout->hasHeightForWidth() ? layout->totalHeightForWidth(width)
                                                    : layout->totalSizeHint().height();
    const int room = availableHeight() - kScreenMargin - outside;
    const int height = std::max(kMinBodyHeight, std::min(content, room));
    bodyScroll_->setMinimumHeight(height);
    bodyScroll_->setMaximumHeight(std::max(height, content));
    layout->activate();
    resize(width, outside + height);
}

void HuggingFaceConsentDialog::showEvent(QShowEvent* event) {
    if (!event->spontaneous()) {
        // Before QDialog's own showEvent, which places the dialog by its size.
        fitBody();
    }
    QDialog::showEvent(event);
    if (!event->spontaneous()) {
        bodyScroll_->verticalScrollBar()->setValue(0);
        safeButton_->setFocus(Qt::OtherFocusReason);
    }
}

void HuggingFaceConsentDialog::finish(models::ConsentAction action) {
    resultAction_ = action;
    accept();
}

} // namespace fv
