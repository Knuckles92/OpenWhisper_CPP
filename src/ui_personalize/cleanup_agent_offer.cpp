#include "ui_personalize/cleanup_agent_offer.h"

#include <exception>
#include <map>
#include <utility>

#include <QAbstractButton>
#include <QApplication>
#include <QPushButton>
#include <QStringList>
#include <QTimer>

#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "llm/cleanup_agent_offer.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "settings/store.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.cleanup_agent_offer"};

QString agentName(const agents::InstalledAgent& agent) {
    return qt::toQString(agent.spec().name);
}

// ask(): the question; the chosen agent's id, or nullopt to keep `current`.
std::optional<std::string> askUser(QWidget* parent,
                                   const std::vector<agents::InstalledAgent>& agents,
                                   const QString& current) {
    QStringList names;
    for (const auto& agent : agents) {
        names << agentName(agent);
    }
    QMessageBox box(parent);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(QStringLiteral("AI cleanup"));
    QString found;
    QString sent;
    if (agents.size() == 1) {
        box.setText(QStringLiteral("Use %1 for AI cleanup?").arg(names.front()));
        found =
            QStringLiteral("%1 is installed and signed in on this computer.").arg(names.front());
        sent = QStringLiteral("%1's model").arg(names.front());
    } else {
        box.setText(QStringLiteral("Use a coding agent for AI cleanup?"));
        found = QStringLiteral("%1 are installed and signed in on this computer.")
                    .arg(names.join(QStringLiteral(" and ")));
        sent = QStringLiteral("the model of the one you pick");
    }
    box.setInformativeText(
        QStringLiteral("%1 AI cleanup can run through the account you already use there, with no "
                       "API key. Your transcript text is sent to %2; no audio is sent.\n\n"
                       "You can change this any time in Settings → AI cleanup.")
            .arg(found, sent));
    std::map<QAbstractButton*, std::string> buttons;
    QPushButton* first = nullptr;
    for (const auto& agent : agents) {
        QPushButton* button =
            box.addButton(QStringLiteral("Use %1").arg(agentName(agent)), QMessageBox::AcceptRole);
        buttons.emplace(button, agent.id);
        if (first == nullptr) {
            first = button;
        }
    }
    QPushButton* keep =
        box.addButton(QStringLiteral("Keep %1").arg(current), QMessageBox::RejectRole);
    box.setDefaultButton(first);
    box.setEscapeButton(keep);
    box.exec();
    const auto chosen = buttons.find(box.clickedButton());
    if (chosen == buttons.end()) {
        return std::nullopt;
    }
    return chosen->second;
}

} // namespace

AgentConsentQuestion agentConsentQuestion(const QString& name) {
    return {
        QStringLiteral("Use %1 for AI cleanup?").arg(name),
        QStringLiteral("While AI cleanup is enabled, OpenWhisper_CPP will send future dictation "
                       "transcripts and cleanup instructions through the %1 CLI to the AI "
                       "provider signed in there. This applies to live dictation you start "
                       "on this computer, including when you use a cleanup profile.")
            .arg(name)};
}

CleanupAgentOffer::CleanupAgentOffer(SettingsStore& settings, Executor& io, Executor& ui,
                                     QWidget* parentWidget, Ports ports, QObject* parent)
    : QObject(parent), settings_(settings), io_(io), ui_(ui), parentWidget_(parentWidget),
      ports_(std::move(ports)) {
    if (!ports_.cached) {
        ports_.cached = []() -> std::optional<agents::ScanResult> {
            const auto agents = agents::installed_agents_if_set();
            return agents ? agents->cached_scan() : std::nullopt;
        };
    }
    if (!ports_.scan) {
        ports_.scan = [] {
            const auto agents = agents::installed_agents_if_set();
            return agents ? agents->scan_agents() : agents::ScanResult{};
        };
    }
    if (!ports_.ask) {
        ports_.ask = askUser;
    }
    if (!ports_.question) {
        ports_.question = [](QWidget* parent, const QString& title, const QString& text,
                             QMessageBox::StandardButtons buttons,
                             QMessageBox::StandardButton fallback) {
            return QMessageBox::question(parent, title, text, buttons, fallback);
        };
    }
}

CleanupAgentOffer::~CleanupAgentOffer() = default;

bool CleanupAgentOffer::due() const {
    try {
        return llm::cleanup_agent_offer::pending(*settings_.snapshot());
    } catch (const std::exception& error) {
        kLog.warning("Could not check the cleanup agent offer: {}", error.what());
        return false;
    }
}

void CleanupAgentOffer::maybeOffer() {
    if (busy_ || !due()) {
        return;
    }
    busy_ = true;
    std::weak_ptr<int> alive = alive_;
    if (auto cached = ports_.cached()) {
        // Queued: the question never opens inside the toggle that caused it.
        QTimer::singleShot(0, this, [this, scan = std::move(*cached)] { onScanned(scan); });
        return;
    }
    // The worker reaches back only through the GUI thread's executor, and only while this
    // object lives (it is destroyed on the GUI thread).
    const auto deliver = [this, alive, &ui = ui_](agents::ScanResult scan) {
        ui.post([this, alive, scan = std::move(scan)] {
            if (alive.lock()) {
                onScanned(scan);
            }
        });
    };
    try {
        io_.post([scan = ports_.scan, deliver] {
            agents::ScanResult result;
            try {
                result = scan();
            } catch (const std::exception& error) {
                kLog.warning("Scanning for a cleanup agent failed: {}", error.what());
            }
            deliver(std::move(result));
        });
    } catch (const std::exception& error) {
        kLog.warning("Scanning for a cleanup agent failed: {}", error.what());
        busy_ = false;
    }
}

void CleanupAgentOffer::onScanned(const agents::ScanResult& scan) {
    try {
        const auto agents = llm::cleanup_agent_offer::offerable(&scan);
        // Rechecked: the scan takes seconds and the user may have moved on.
        if (agents.empty() || !due()) {
            busy_ = false;
            return;
        }
        const Json settings = *settings_.snapshot();
        const QString current = qt::toQString(llm::profile_display_name(
            llm::resolve_transcript_cleanup_provider(settings), settings));
        QWidget* parent = QApplication::activeWindow();
        if (parent == nullptr) {
            parent = parentWidget_.data();
        }
        const QPointer<CleanupAgentOffer> self(this);
        const auto choice = ports_.ask(parent, agents, current);
        if (self == nullptr) {
            return;
        }
        if (!choice) {
            llm::cleanup_agent_offer::decline(settings_);
            busy_ = false;
            return;
        }
        // The consent the AI cleanup settings ask for before dictation reaches this CLI.
        QString name = qt::toQString(*choice);
        for (const auto& agent : agents) {
            if (agent.id == *choice) {
                name = agentName(agent);
            }
        }
        const auto consent = agentConsentQuestion(name);
        const auto answer = ports_.question(parent, consent.title, consent.text,
                                            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (self == nullptr) {
            return;
        }
        if (answer != QMessageBox::Yes) {
            llm::cleanup_agent_offer::decline(settings_);
            busy_ = false;
            return;
        }
        llm::cleanup_agent_offer::accept(*choice, settings_, /*consented=*/true);
        kLog.info("AI cleanup now runs through {}", *choice);
        busy_ = false;
        if (onAccepted) {
            onAccepted();
        }
    } catch (const std::exception& error) {
        kLog.warning("The cleanup agent offer failed: {}", error.what());
        busy_ = false;
    }
}

} // namespace fv
