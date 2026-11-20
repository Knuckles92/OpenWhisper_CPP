#include "ui_downloads/required_runtime_dialog.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "models/consent_text.h"
#include "ui_widgets/buttons.h"

namespace fv {

RequiredRuntimeDialog::RequiredRuntimeDialog(const std::string& modelName,
                                             const std::string& componentId,
                                             const models::Host& host, QWidget* parent)
    : QDialog(parent) {
    const models::RuntimeSizes sizes = models::runtime_sizes(componentId, host);
    setWindowTitle(qt::toQString(models::kRequiredRuntimeWindowTitle));
    setMinimumWidth(460);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    titleLabel_ = new QLabel(qt::toQString(models::required_runtime_title(componentId)));
    titleLabel_->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(titleLabel_);
    body_ = new QLabel(
        qt::toQString(models::required_runtime_body(modelName, componentId, sizes.download)));
    body_->setWordWrap(true);
    layout->addWidget(body_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    laterButton_ = new Button(QStringLiteral("Later"));
    connect(laterButton_, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(laterButton_);
    installButton_ = new PrimaryButton(QStringLiteral("Install required runtime"));
    installButton_->setAutoDefault(false);
    connect(installButton_, &QPushButton::clicked, this, &QDialog::accept);
    // Enter on a prompt that just appeared must not start a download.
    laterButton_->setDefault(true);
    laterButton_->setFocus();
    buttons->addWidget(installButton_);
    layout->addLayout(buttons);
}

GpuRuntimeDialog::GpuRuntimeDialog(const std::string& modelName, const std::string& gpuComponent,
                                   const std::string& cpuComponent,
                                   const std::optional<models::NvidiaGpu>& gpu,
                                   const models::Host& host, QWidget* parent)
    : QDialog(parent) {
    setWindowTitle(qt::toQString(models::kGpuRuntimeWindowTitle));
    setMinimumWidth(480);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(12);
    titleLabel_ = new QLabel(qt::toQString(models::gpu_runtime_title(modelName, gpu)));
    titleLabel_->setObjectName(QStringLiteral("headerLabel"));
    layout->addWidget(titleLabel_);
    body_ = new QLabel(qt::toQString(models::gpu_runtime_body(
        modelName, gpuComponent, cpuComponent, gpu, models::runtime_sizes(gpuComponent, host),
        models::runtime_sizes(cpuComponent, host))));
    body_->setWordWrap(true);
    layout->addWidget(body_);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch();
    laterButton_ = new Button(QStringLiteral("Later"));
    connect(laterButton_, &QPushButton::clicked, this, &QDialog::reject);
    buttons->addWidget(laterButton_);
    cpuButton_ = new Button(QStringLiteral("Use the CPU"));
    cpuButton_->setAutoDefault(false);
    connect(cpuButton_, &QPushButton::clicked, this,
            [this, cpuComponent] { choose(cpuComponent); });
    buttons->addWidget(cpuButton_);
    gpuButton_ = new PrimaryButton(QStringLiteral("Use this GPU"));
    gpuButton_->setAutoDefault(false);
    connect(gpuButton_, &QPushButton::clicked, this,
            [this, gpuComponent] { choose(gpuComponent); });
    laterButton_->setDefault(true);
    laterButton_->setFocus();
    buttons->addWidget(gpuButton_);
    layout->addLayout(buttons);
}

void GpuRuntimeDialog::choose(const std::string& componentId) {
    choice_ = componentId;
    accept();
}

} // namespace fv
