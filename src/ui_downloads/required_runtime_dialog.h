#pragma once

#include <optional>
#include <string>

#include <QDialog>

#include "models/catalog.h"
#include "models/gpu_probe.h"

class QLabel;
class QPushButton;

namespace fv {

// Explains the second download an optional speech model needs: the runtime that runs it.
// `exec() == Accepted` means install it; Later (the default and focused button, so Enter on a
// prompt that just appeared starts nothing) rejects. Affinity: GUI thread.
class RequiredRuntimeDialog : public QDialog {
    Q_OBJECT
public:
    // `host` says which platform's archives the download size is summed over.
    RequiredRuntimeDialog(const std::string& modelName, const std::string& componentId,
                          const models::Host& host, QWidget* parent = nullptr);

    QLabel* titleLabel() const { return titleLabel_; }
    QLabel* body() const { return body_; }
    QPushButton* laterButton() const { return laterButton_; }
    QPushButton* installButton() const { return installButton_; }

private:
    QLabel* titleLabel_ = nullptr;
    QLabel* body_ = nullptr;
    QPushButton* laterButton_ = nullptr;
    QPushButton* installButton_ = nullptr;
};

// First use of Parakeet on Auto with an NVIDIA GPU: install the GPU runtime ("Use this GPU", the
// recommended choice), the CPU one, or neither (Later). choice() is the component id to install,
// nullopt for Later. Later is the default button: Enter on a prompt that just appeared must not
// start a download. Affinity: GUI thread.
class GpuRuntimeDialog : public QDialog {
    Q_OBJECT
public:
    // `gpu` is the card the offer names (nullopt reads "this GPU").
    GpuRuntimeDialog(const std::string& modelName, const std::string& gpuComponent,
                     const std::string& cpuComponent, const std::optional<models::NvidiaGpu>& gpu,
                     const models::Host& host, QWidget* parent = nullptr);

    const std::optional<std::string>& choice() const { return choice_; }

    QLabel* titleLabel() const { return titleLabel_; }
    QLabel* body() const { return body_; }
    QPushButton* laterButton() const { return laterButton_; }
    QPushButton* cpuButton() const { return cpuButton_; }
    QPushButton* gpuButton() const { return gpuButton_; }

private:
    void choose(const std::string& componentId);

    std::optional<std::string> choice_;
    QLabel* titleLabel_ = nullptr;
    QLabel* body_ = nullptr;
    QPushButton* laterButton_ = nullptr;
    QPushButton* cpuButton_ = nullptr;
    QPushButton* gpuButton_ = nullptr;
};

} // namespace fv
