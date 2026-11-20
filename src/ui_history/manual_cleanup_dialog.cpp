#include "ui_history/manual_cleanup_dialog.h"
#include "agents/installed_agents.h"
#include "agents/loopback_mcp.h"
#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "net/reactor.h"
#include "ui_history/detail/background.h"
#include <QClipboard>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QGuiApplication>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace fv {
namespace {
constexpr auto kInstruction = "Clean up the transcript for readability while preserving its "
                              "meaning. Return only the cleaned transcript.";
}
agents::PassOutcome run_manual_cleanup(std::string agent, std::string transcript,
                                       CancelToken cancel) {
    try {
        if (cancel.cancelled())
            return {.canceled = true};
        auto installed = agents::installed_agents_if_set();
        if (!installed)
            return {.error = "Installed agents are unavailable."};
        auto selected = installed->resolve_agent(agent);
        if (!selected || !selected->usable())
            return {.error = "The selected agent is unavailable."};
        if (cancel.cancelled())
            return {.canceled = true};
        net::Reactor reactor;
        agents::LoopbackMcpServer server(reactor);
        auto driver = agents::make_driver(*selected, installed->launcher());
        struct Stop {
            agents::Driver& driver;
            agents::LoopbackMcpServer& server;
            ~Stop() {
                try {
                    driver.close();
                } catch (...) {
                }
                server.stop();
            }
        } stop{*driver, server};
        if (cancel.cancelled())
            return {.canceled = true};
        server.start();
        driver->start(server, {{"build", kInstruction}}, Json::array());
        if (cancel.cancelled())
            return {.canceled = true};
        agents::PassRequest request;
        request.system_prompt = kInstruction;
        request.user_prompt = std::move(transcript);
        request.tools = Json::array();
        request.handler = [](std::string_view, const Json&, const Json&) {
            return std::pair<std::string, bool>{"Tools are disabled.", true};
        };
        request.timeout_s = 60.0;
        request.cancel = cancel;
        return driver->run_pass(request);
    } catch (const std::exception&) {
        return {.error = "The agent could not clean this transcript."};
    }
}
ManualCleanupDialog::ManualCleanupDialog(QString transcript, Executor& io, Runner runner,
                                         QWidget* parent)
    : QDialog(parent), transcript_(std::move(transcript)), io_(io), runner_(std::move(runner)),
      ui_(std::make_shared<qt::QtUiExecutor>(this)) {
    setWindowTitle(QStringLiteral("Clean with an installed agent"));
    resize(680, 650);
    auto* layout = new QVBoxLayout(this);
    layout->addWidget(new QLabel(
        QStringLiteral("Choose the agent that will receive this transcript and instruction:"),
        this));
    agent_ = new QComboBox(this);
    agent_->addItem(QStringLiteral("Choose an agent"), QString());
    for (const auto& spec : agents::agent_specs())
        agent_->addItem(qt::toQString(std::string(spec.name)), qt::toQString(std::string(spec.id)));
    layout->addWidget(agent_);
    auto* instruction = new QLabel(qt::toQString(kInstruction), this);
    instruction->setWordWrap(true);
    instruction->setTextFormat(Qt::PlainText);
    layout->addWidget(instruction);
    auto* preview = new QPlainTextEdit(this);
    preview->setReadOnly(true);
    preview->setPlainText(transcript_);
    layout->addWidget(preview);
    send_ = new QPushButton(QStringLiteral("Send to selected agent"), this);
    send_->setAutoDefault(false);
    send_->setDefault(false);
    send_->setEnabled(false);
    connect(agent_, &QComboBox::currentIndexChanged, this,
            [this](int index) { send_->setEnabled(index > 0); });
    connect(send_, &QPushButton::clicked, this, [this] { send(); });
    layout->addWidget(send_);
    status_ = new QLabel(this);
    status_->setTextFormat(Qt::PlainText);
    layout->addWidget(status_);
    result_ = new QPlainTextEdit(this);
    result_->setPlaceholderText(
        QStringLiteral("The result appears here and can be edited before copying."));
    layout->addWidget(result_);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close, this);
    copy_ = buttons->addButton(QStringLiteral("Copy result"), QDialogButtonBox::ActionRole);
    copy_->setEnabled(false);
    copyText = [](const QString& value) {
        if (!QGuiApplication::clipboard())
            return false;
        QGuiApplication::clipboard()->setText(value);
        return true;
    };
    connect(copy_, &QPushButton::clicked, this, [this] {
        if (copyText(result_->toPlainText()))
            status_->setText(QStringLiteral("Copied."));
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    layout->addWidget(buttons);
}
ManualCleanupDialog::~ManualCleanupDialog() {
    cancel_.cancel();
}
void ManualCleanupDialog::send() {
    if (!runner_ || agent_->currentIndex() <= 0 || transcript_.isEmpty())
        return;
    const auto selected = qt::toUtf8(agent_->currentData().toString());
    const auto transcript = qt::toUtf8(transcript_);
    send_->setEnabled(false);
    agent_->setEnabled(false);
    status_->setText(QStringLiteral("Waiting for the agent…"));
    auto token = cancel_.token();
    auto runner = runner_;
    history_detail::runInBackground(
        io_, ui_,
        [runner, selected, transcript, token] {
            try {
                return runner(selected, transcript, token);
            } catch (...) {
                return agents::PassOutcome{.error = "The agent could not clean this transcript."};
            }
        },
        [this, token](agents::PassOutcome outcome) {
            if (token.cancelled())
                return;
            if (outcome.ok && !outcome.text.empty()) {
                result_->setPlainText(qt::toQString(outcome.text));
                copy_->setEnabled(true);
                status_->setText(QStringLiteral("Review and edit the result before copying."));
            } else {
                status_->setText(
                    qt::toQString(outcome.error.empty() ? "No result returned." : outcome.error));
                agent_->setEnabled(true);
                send_->setEnabled(true);
            }
        });
}
} // namespace fv
