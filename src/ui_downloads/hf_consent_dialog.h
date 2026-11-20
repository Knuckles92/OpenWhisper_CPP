#pragma once

#include <functional>
#include <string>

#include <QDialog>

#include "models/consent_text.h"
#include "models/hf_access.h"

class QLabel;
class QPushButton;
class QScrollArea;
class QShowEvent;
class QVBoxLayout;
class QWidget;

namespace fv {

// Asks before a model download, which can be gigabytes: shown when a requested model is missing
// and the access policy requires consent ("ask") or an explicit override ("never"), and, with
// only a Close button, when HF_HUB_OFFLINE disables downloads from outside the application.
// Nothing here starts a download by default: Cancel (Close) is the default and focused button,
// the size is in the title, which stays in view with the buttons however short the screen is,
// and the long notice scrolls. Affinity: GUI thread.
class HuggingFaceConsentDialog : public QDialog {
    Q_OBJECT
public:
    // The dialog's width; the notice wraps to it.
    static constexpr int kWidth = 520;
    // Kept free of the screen's height for the window frame and panels.
    static constexpr int kScreenMargin = 64;
    // The notice never shrinks below this, however short the screen.
    static constexpr int kMinBodyHeight = 96;

    // `policy` is an hf_access_policy value, which picks the buttons offered; `needs` are the
    // runtimes the model still needs (models::runtime_needs), named in the notice.
    HuggingFaceConsentDialog(std::string modelName, std::string policy, bool envBlocked = false,
                             models::RuntimeNeeds needs = {}, QWidget* parent = nullptr);

    // What the user chose; Cancel until a button says otherwise, and for Close.
    models::ConsentAction resultAction() const { return resultAction_; }

    // The notice (consent_body).
    QString bodyText() const;
    QLabel* titleLabel() const { return titleLabel_; }
    QScrollArea* bodyScroll() const { return bodyScroll_; }
    // Cancel, or Close when downloads are blocked: the default and focused button.
    QPushButton* safeButton() const { return safeButton_; }

    // Replaces the screen's available height, in pixels, that the notice is fitted to.
    void setAvailableHeight(std::function<int()> availableHeight);

protected:
    void showEvent(QShowEvent* event) override;

private:
    // The height of the screen the dialog opens on, less its panels.
    int availableHeight() const;
    // Shows the whole notice when it fits on screen; scrolls it when not.
    void fitBody();
    void finish(models::ConsentAction action);

    std::string modelName_;
    std::string policy_;
    bool envBlocked_;
    models::RuntimeNeeds needs_;
    models::ConsentAction resultAction_ = models::ConsentAction::Cancel;
    std::function<int()> availableHeight_;

    QVBoxLayout* contentLayout_ = nullptr;
    QLabel* titleLabel_ = nullptr;
    QWidget* bodyHost_ = nullptr;
    QScrollArea* bodyScroll_ = nullptr;
    QPushButton* safeButton_ = nullptr;
};

} // namespace fv
