#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QMessageBox>
#include <QObject>
#include <QPointer>
#include <QString>

#include "agents/installed_agents.h"

class QWidget;

namespace fv {

class Executor;
class SettingsStore;

// The question AI cleanup's settings ask before dictation goes through an installed agent's CLI
// (fv_cleanup_agent_consent): its title and text for the agent named `name`.
struct AgentConsentQuestion {
    QString title;
    QString text;
};
AgentConsentQuestion agentConsentQuestion(const QString& name);

// Asks once whether AI cleanup should run through Claude Code or Codex
// (ui_qt/cleanup_agent_offer.py). llm::cleanup_agent_offer decides when the question is due and
// saves the answer; this looks for the agents off the GUI thread and shows it.
//
// FioriVoce asks one more question than OpenWhisper: choosing an agent in the offer shows the
// same consent question the AI cleanup settings show for that runner (agentConsentQuestion).
// Yes there selects the agent with consent recorded; No counts as declining the offer.
// Affinity: GUI thread.
class CleanupAgentOffer : public QObject {
    Q_OBJECT
public:
    // QMessageBox::question's shape, as CleanupSettings::Question.
    using Question = std::function<QMessageBox::StandardButton(
        QWidget* parent, const QString& title, const QString& text,
        QMessageBox::StandardButtons buttons, QMessageBox::StandardButton defaultButton)>;
    // Each empty member takes the default named beside it.
    struct Ports {
        // The last agent scan, or nullopt before the first; never blocks
        // (InstalledAgents::cached_scan).
        std::function<std::optional<agents::ScanResult>()> cached;
        // A scan of this computer; blocking, run on `io` (InstalledAgents::scan_agents).
        std::function<agents::ScanResult()> scan;
        // The offer itself: the chosen agent's id, or nullopt to keep `current` (a QMessageBox).
        std::function<std::optional<std::string>(QWidget* parent,
                                                 const std::vector<agents::InstalledAgent>& agents,
                                                 const QString& current)>
            ask;
        // The consent question (QMessageBox::question).
        Question question;
    };

    // `settings` and `io` must outlive the offer; `ui` brings the scan back to the GUI thread.
    CleanupAgentOffer(SettingsStore& settings, Executor& io, Executor& ui, QWidget* parentWidget,
                      Ports ports = {}, QObject* parent = nullptr);
    ~CleanupAgentOffer() override;

    // Makes the offer when it is due; safe to call on every cleanup change. The question never
    // opens inside the call (the toggle that caused it).
    void maybeOffer();

    // Called after the runner was switched, to refresh the views that name it.
    std::function<void()> onAccepted;

private:
    bool due() const;
    void onScanned(const agents::ScanResult& scan);

    SettingsStore& settings_;
    Executor& io_;
    Executor& ui_;
    QPointer<QWidget> parentWidget_;
    Ports ports_;
    bool busy_ = false;
    std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

} // namespace fv
