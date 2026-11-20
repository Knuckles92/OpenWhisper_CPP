#pragma once

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <QObject>

#include "agents/installed_agents.h"
#include "base/json.h"
#include "ui_setup/guided_setup_dialog.h"

namespace fv {

class Executor;

// guided_setup.py provider_summary: the Clean up step's provider row and privacy sentence from the
// saved configuration. `keySaved` answers resolve_credential for a variable name (whether a key is
// saved under it); it is asked only for an endpoint that needs a key and never probes the network,
// so a saved key reads as configured, never as a tested connection. An installed coding agent
// reads its sign-in from the agent ("Uses your agent's sign-in."), unless the session's last agent
// scan (never a new probe) found it missing or not usable.
ProviderSummary providerSummary(const Json& settings,
                                const std::function<bool(const std::string& env)>& keySaved);
// The same with the agent scan given: nullopt before the first scan.
ProviderSummary providerSummary(const Json& settings,
                                const std::function<bool(const std::string& env)>& keySaved,
                                const std::optional<agents::ScanResult>& scan);

// cleanup_prompts.custom_prompt: the user's saved cleanup prompt, "" when none is saved or it is a
// copy of a built-in one.
std::string customCleanupPrompt(const Json& settings);

// Whether a key is saved under a variable name, for providerSummary on the GUI thread. The
// credential store blocks and never runs on the GUI thread (architecture amendment 15), so the
// lookup runs on `worker` and the answer is kept: saved() answers what the last lookup found
// (not saved before the first one comes back) and starts a lookup for a name it has not looked up
// yet. An answer that differs from the one kept emits changed() on the GUI thread, so the row is
// drawn again. Keys are never logged; only whether one was found.
// Affinity: GUI thread; the lookup runs on `worker`.
class KeyPresence : public QObject {
    Q_OBJECT
public:
    // Blocking: whether a key is saved under `env` (the credential store, the environment and
    // the .env file).
    using Lookup = std::function<bool(const std::string& env)>;

    // `worker` and `ui` outlive this object and the lookups queued on them.
    KeyPresence(Lookup lookup, Executor& worker, Executor& ui, QObject* parent = nullptr);
    ~KeyPresence() override;

    // The last answer for `env`, false before one; looks it up when it never was.
    bool saved(const std::string& env);
    // Looks `env` up again (a key was saved or removed). While a lookup of `env` is running, one
    // more follows it, since that one may have read the store before the change.
    void check(const std::string& env);
    // Looks every name asked for so far up again.
    void recheck();

Q_SIGNALS:
    void changed();

private:
    struct State;
    std::shared_ptr<State> state_;
    std::map<std::string, bool> known_;
    std::set<std::string> pending_;
    // Names asked for again while their lookup was running.
    std::set<std::string> again_;
};

} // namespace fv
