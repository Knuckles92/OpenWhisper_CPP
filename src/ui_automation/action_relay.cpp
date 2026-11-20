#include "ui_automation/action_relay.h"

#include <cstdlib>
#include <exception>
#include <mutex>
#include <typeinfo>
#include <utility>

#if defined(__GNUG__)
#include <cxxabi.h>
#endif

#include <QMetaObject>

#include "agents/installed_agents.h"
#include "automation/secrets.h"
#include "base/executor.h"
#include "base/log.h"
#include "pyport/errors.h"

namespace fv {

struct ActionRelay::Lifeline {
    std::mutex mutex;
    ActionRelay* relay = nullptr; // null once the relay's destruction has begun

    // Queues fn(relay) to the relay's thread while it exists. The lock is held across the post,
    // and the destructor takes it before ~QObject removes the relay's posted events, so a post
    // that saw the relay alive is either removed with it or runs while it still exists.
    template <class Fn> void deliver(const std::shared_ptr<Lifeline>& self, Fn&& fn) {
        std::lock_guard lock(mutex);
        if (relay == nullptr) {
            return;
        }
        QMetaObject::invokeMethod(
            relay,
            [self, fn = std::forward<Fn>(fn)]() mutable {
                if (self->relay != nullptr) {
                    fn(*self->relay);
                }
            },
            Qt::QueuedConnection);
    }
};

namespace {

constexpr log::Logger kLog{"ui_qt.widgets.action_relay"};

// type(exc).__name__: the Python class a pyport error stands in for, else the C++ class name
// without its namespaces (demangled where the compiler mangles type_info names).
std::string typeName(const std::exception& error) {
    if (const auto* python = dynamic_cast<const py::PyError*>(&error)) {
        return std::string(python->type_name());
    }
    std::string name = typeid(error).name();
#if defined(__GNUG__)
    int status = 0;
    char* demangled = abi::__cxa_demangle(name.c_str(), nullptr, nullptr, &status);
    if (status == 0 && demangled != nullptr) {
        name = demangled;
    }
    std::free(demangled);
#endif
    for (const std::string_view prefix : {"class ", "struct "}) {
        if (name.starts_with(prefix)) {
            name.erase(0, prefix.size());
        }
    }
    if (const auto colons = name.rfind("::"); colons != std::string::npos) {
        name.erase(0, colons + 2);
    }
    return name;
}

// The exception in flight: (its type's name, str(exc) or the type's name).
std::pair<std::string, std::string> describeCurrent() {
    try {
        throw;
    } catch (const std::exception& error) {
        std::string type = typeName(error);
        std::string text = error.what();
        return {type, text.empty() ? type : std::move(text)};
    } catch (...) {
        return {"Exception", "Exception"};
    }
}

} // namespace

ActionRelayWork actionRelayWork(std::shared_ptr<agents::InstalledAgents> agents,
                                CredentialStore& store) {
    ActionRelayWork work;
    work.scanAgents = [agents = std::move(agents)](bool refresh) {
        agents->scan_agents(refresh);
    };
    work.ensureSecret = [&store](const std::string& actionId) {
        return automation::ensure_action_secret(store, actionId);
    };
    work.deleteSecret = [&store](const std::string& actionId) {
        automation::delete_action_secret(store, actionId);
    };
    return work;
}

ActionRelay::ActionRelay(Executor& io, ActionRelayWork work, QObject* parent)
    : QObject(parent), io_(io), work_(std::move(work)), lifeline_(std::make_shared<Lifeline>()) {
    lifeline_->relay = this;
}

ActionRelay::~ActionRelay() {
    std::lock_guard lock(lifeline_->mutex);
    lifeline_->relay = nullptr;
}

int ActionRelay::nextToken() {
    return ++tokens_;
}

void ActionRelay::startAgentScan(int token, bool refresh) {
    // Bound here: the worker must not reach back into the relay.
    io_.post([scan = work_.scanAgents, line = lifeline_, token, refresh] {
        try {
            if (scan) {
                scan(refresh);
            }
        } catch (...) {
            kLog.warning("Scanning for installed agents failed: {}", describeCurrent().first);
        }
        line->deliver(line, [token](ActionRelay& relay) { Q_EMIT relay.scanDone(token); });
    });
}

void ActionRelay::fetchSecret(int token, const QString& actionId, bool renew,
                              const QObject* requester) {
    auto found = secretGates_.find(requester);
    if (found == secretGates_.end()) {
        found = secretGates_.emplace(requester, qt::TokenGate()).first;
        if (requester != nullptr) {
            connect(requester, &QObject::destroyed, this,
                    [this, requester] { secretGates_.erase(requester); });
        }
    }
    qt::TokenGate gate = found->second; // copies share the newest token
    const std::uint64_t gateToken = gate.next();
    io_.post([ensure = work_.ensureSecret, remove = work_.deleteSecret, gate, line = lifeline_,
              id = qt::toUtf8(actionId), token, renew, gateToken] {
        std::string secret;
        std::string error;
        try {
            if (renew && remove) {
                remove(id);
            }
            secret = ensure ? ensure(id) : std::string();
        } catch (...) {
            auto [type, text] = describeCurrent();
            kLog.warning("Reading an action's signing secret failed: {}", type);
            secret.clear();
            error = std::move(text);
        }
        if (!gate.isCurrent(gateToken)) {
            return;
        }
        line->deliver(line, [gate, gateToken, token, secret = qt::toQString(secret),
                             error = qt::toQString(error)](ActionRelay& relay) {
            // A newer fetch by the same requester asked since this one was queued wins.
            if (gate.isCurrent(gateToken)) {
                Q_EMIT relay.secretDone(token, secret, error);
            }
        });
    });
}

void ActionRelay::forgetSecret(const QString& actionId) {
    io_.post([remove = work_.deleteSecret, id = qt::toUtf8(actionId)] {
        try {
            if (remove) {
                remove(id);
            }
        } catch (...) {
            // An orphaned secret signs nothing; the next delete or restore drops it.
            kLog.warning("Removing an action's signing secret failed: {}", describeCurrent().first);
        }
    });
}

std::function<void(automation::ActionResult)> ActionRelay::tryCallback(int token) {
    return [line = lifeline_, token](automation::ActionResult result) {
        line->deliver(line, [token, result = std::move(result)](ActionRelay& relay) {
            Q_EMIT relay.tryDone(token, result);
        });
    };
}

} // namespace fv
