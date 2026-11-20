#pragma once

#include <QObject>
#include <QString>

#include "base/subscription.h"

namespace fv {

class Executor;

namespace agents {
class InstalledAgents;
} // namespace agents

// Brings installed-agent lookups to the GUI thread (ui_qt/utils/agent_lookup.py). Widgets read
// readiness that never starts an agent; a lookup that starts lands later on a worker thread, and
// this relay tells whichever widgets exist then to read again. It lives for the session, so the
// lookup thread never holds or posts to a widget.
// Affinity: GUI thread (construct and destroy it there); lookedUp arrives on it.
class AgentLookupRelay : public QObject {
    Q_OBJECT
public:
    explicit AgentLookupRelay(agents::InstalledAgents& agents, QObject* parent = nullptr);
    ~AgentLookupRelay() override;
    AgentLookupRelay(const AgentLookupRelay&) = delete;
    AgentLookupRelay& operator=(const AgentLookupRelay&) = delete;

Q_SIGNALS:
    // What is known about agentId changed.
    void lookedUp(const QString& agentId);

private:
    // Never deleted: a lookup thread may still post to it after the subscription ends, and once
    // this relay is gone it drops every post.
    Executor* executor_;
    Subscription subscription_;
};

// The session relay over the set agents::InstalledAgents instance, made on first use; null when
// none is set. It is a child of the application, so it goes with it.
// Affinity: GUI thread.
AgentLookupRelay* agentLookupRelay();

} // namespace fv
