#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>

#include <QObject>
#include <QString>

#include "appkit/qt_bridge.h"
#include "automation/action_result.h"

namespace fv {

class CredentialStore;
class Executor;

namespace agents {
class InstalledAgents;
}

// The blocking work ActionRelay runs off the GUI thread. Each may throw; the relay turns that
// into its answer.
struct ActionRelayWork {
    // Refreshes the installed-agent scan that InstalledAgents::cached_scan() reads.
    std::function<void(bool refresh)> scanAgents;
    // A webhook action's signing secret, made if missing (automation::ensure_action_secret).
    std::function<std::string(const std::string& actionId)> ensureSecret;
    // Forgets a webhook action's signing secret (automation::delete_action_secret).
    std::function<void(const std::string& actionId)> deleteSecret;
};

// The app's work: `agents`' scan and `store`'s action secrets. `store` must outlive every task
// the relay queued.
ActionRelayWork actionRelayWork(std::shared_ptr<agents::InstalledAgents> agents,
                                CredentialStore& store);

// Brings agent scans, "Try it" results and signing secrets back to the Actions page
// (ui_qt/widgets/action_relay.py). One relay lives for the session, so a worker never holds or
// emits on a page widget: the blocking calls run on `io`, and their answers are emitted from
// this object on its own (GUI) thread, or not at all once it is gone. Each request carries a
// token from nextToken(); a widget answers only its own.
//
// Several widgets scan and try through one relay (the agent card, the editor, the Walkthrough
// tab), so scanDone and tryDone carry every token and each receiver matches its own. Secret
// answers carry their token too, and each requester (an editor in Settings, another in the
// Walkthrough tab) has a qt::TokenGate of its own: the answer to an earlier fetchSecret of the
// same requester than its newest is dropped, and another requester's fetch cuts off nothing.
// Affinity: GUI thread (tryCallback's function may be called from any thread).
class ActionRelay : public QObject {
    Q_OBJECT
public:
    // Shared with the workers: whether the relay still exists, published under a lock its
    // destructor takes, so a worker never posts to a destroyed relay.
    struct Lifeline;

    // `io` outlives the relay.
    ActionRelay(Executor& io, ActionRelayWork work, QObject* parent = nullptr);
    // Answers still on their way are dropped; tasks already queued on `io` still run.
    ~ActionRelay() override;
    ActionRelay(const ActionRelay&) = delete;
    ActionRelay& operator=(const ActionRelay&) = delete;

    // Monotonic, from 1.
    int nextToken();
    // Refreshes the scan off the GUI thread; scanDone(token) follows, also when the scan threw
    // (it is logged, and the cached scan then says what was found before).
    void startAgentScan(int token, bool refresh = false);
    // A webhook action's signing secret, made if missing (`renew`: the old one is deleted
    // first), off the GUI thread; secretDone(token, secret, error) follows unless `requester`
    // asked a newer fetchSecret since (null is one shared requester). error is the failure's
    // message, or its type when it has none.
    void fetchSecret(int token, const QString& actionId, bool renew = false,
                     const QObject* requester = nullptr);
    // Removes a webhook action's signing secret off the GUI thread; nothing waits for it, and a
    // failure is only logged (an orphaned secret signs nothing).
    void forgetSecret(const QString& actionId);
    // The `done` to hand ActionTester::test: tryDone(token, result) follows on the GUI thread.
    // Safe from any thread.
    std::function<void(automation::ActionResult)> tryCallback(int token);

Q_SIGNALS:
    // A scan for installed agents finished; read the cached scan now.
    void scanDone(int token);
    void tryDone(int token, const fv::automation::ActionResult& result);
    // A webhook action's signing secret, or why not.
    void secretDone(int token, const QString& secret, const QString& error);

private:
    Executor& io_;
    ActionRelayWork work_;
    std::shared_ptr<Lifeline> lifeline_;
    int tokens_ = 0;
    // Per requester; an entry goes when its requester is destroyed.
    std::map<const QObject*, qt::TokenGate> secretGates_;
};

} // namespace fv
