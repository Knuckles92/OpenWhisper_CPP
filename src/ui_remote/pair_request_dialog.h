#pragma once

#include <chrono>
#include <string>
#include <vector>

#include <QDialog>
#include <QString>

#include "remote/control.h"

class QLabel;
class QTimer;

namespace fv {

class Button;
class WrappedLabel;

// Allow or Deny, beside the number the asking computer shows too
// (ui_qt/dialogs/pair_request_dialog.py). A modeless, always-on-top "Pair a computer" dialog
// with a countdown; closing it denies, so the other computer isn't left waiting. `answered`
// carries the choice once; a request that goes away by itself (canceled there, timed out) is
// closed with dismiss(), which answers nothing. `grants` are what this host lets paired
// computers do beyond transcribing (pairing_grants()), said before the owner allows one.
// Affinity: GUI thread.
class PairRequestDialog final : public QDialog {
    Q_OBJECT
public:
    PairRequestDialog(const remote::PairRequestInfo& request, QWidget* parent,
                      const std::vector<std::string>& grants = {});

    const std::string& requestId() const { return requestId_; }
    QLabel* codeLabel() const { return code_; }
    WrappedLabel* expiryLabel() const { return expiry_; }
    WrappedLabel* grantsLabel() const { return grants_; }
    Button* allowButton() const { return allow_; }
    Button* denyButton() const { return deny_; }

    // Closes without answering: the request is gone already.
    void dismiss();
    // Escape denies, as closing does.
    void reject() override;

Q_SIGNALS:
    void answered(bool allow);

protected:
    void showEvent(QShowEvent* event) override;
    void closeEvent(QCloseEvent* event) override;

private:
    void tick();
    void answer(bool allow);

    std::string requestId_;
    bool done_ = false;
    std::chrono::steady_clock::time_point deadline_;
    QLabel* code_ = nullptr;
    WrappedLabel* expiry_ = nullptr;
    WrappedLabel* grants_ = nullptr;
    Button* deny_ = nullptr;
    Button* allow_ = nullptr;
    QTimer* timer_ = nullptr;
};

} // namespace fv
