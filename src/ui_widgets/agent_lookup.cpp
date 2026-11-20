#include "ui_widgets/agent_lookup.h"

#include <memory>
#include <string>

#include <QCoreApplication>
#include <QPointer>

#include "agents/installed_agents.h"
#include "appkit/qt_bridge.h"

namespace fv {

AgentLookupRelay::AgentLookupRelay(agents::InstalledAgents& agents, QObject* parent)
    : QObject(parent), executor_(new qt::QtUiExecutor(this)) {
    subscription_ = agents.add_lookup_listener(*executor_, [this](std::string agentId) {
        Q_EMIT lookedUp(QString::fromStdString(agentId));
    });
}

AgentLookupRelay::~AgentLookupRelay() {
    // Ends the connection first: once disconnect returns no lookedUp is running or starts.
    subscription_.disconnect();
}

AgentLookupRelay* agentLookupRelay() {
    static QPointer<AgentLookupRelay> relay;
    // The instance the relay listens to; held so the listener's source outlives the relay.
    static std::shared_ptr<agents::InstalledAgents> source;
    const auto agents = agents::installed_agents_if_set();
    if (relay.isNull() || source != agents) {
        delete relay.data();
        source = agents;
        relay = agents ? new AgentLookupRelay(*agents, QCoreApplication::instance()) : nullptr;
    }
    return relay.data();
}

} // namespace fv
