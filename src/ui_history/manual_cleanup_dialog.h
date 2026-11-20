#pragma once
#include "agents/drivers.h"
#include "base/cancel.h"
#include <QDialog>
#include <functional>
#include <string>

class QComboBox;
class QLabel;
class QPlainTextEdit;
class QPushButton;
namespace fv {
class Executor;
namespace qt {
class QtUiExecutor;
}
class ManualCleanupDialog final : public QDialog {
public:
    using Runner = std::function<agents::PassOutcome(std::string, std::string, CancelToken)>;
    ManualCleanupDialog(QString transcript, Executor& io, Runner runner, QWidget* parent = nullptr);
    ~ManualCleanupDialog() override;
    std::function<bool(const QString&)> copyText;
    QComboBox* agentChoice() const { return agent_; }
    QPushButton* sendButton() const { return send_; }
    QPlainTextEdit* resultText() const { return result_; }

private:
    void send();
    QString transcript_;
    Executor& io_;
    Runner runner_;
    std::shared_ptr<qt::QtUiExecutor> ui_;
    CancelSource cancel_;
    QComboBox* agent_ = nullptr;
    QPlainTextEdit* result_ = nullptr;
    QPushButton* send_ = nullptr;
    QPushButton* copy_ = nullptr;
    QLabel* status_ = nullptr;
};
agents::PassOutcome run_manual_cleanup(std::string agent, std::string transcript,
                                       CancelToken cancel);
} // namespace fv
