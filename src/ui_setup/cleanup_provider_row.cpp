#include "ui_setup/cleanup_provider_row.h"

#include <exception>
#include <optional>
#include <typeinfo>
#include <utility>
#include <vector>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/executor.h"
#include "base/log.h"
#include "llm/cleanup_settings.h"
#include "llm/profiles.h"
#include "text/cleanup_prompts.h"

namespace fv {

namespace {

constexpr log::Logger kLog{"ui_qt.dialogs.guided_setup"};

constexpr char kNoAudio[] = " No audio is sent for cleanup.";

} // namespace

// provider_summary (guided_setup.py:132-179).
ProviderSummary providerSummary(const Json& settings,
                                const std::function<bool(const std::string& env)>& keySaved) {
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    std::optional<agents::ScanResult> scan;
    if (llm::is_installed_cleanup_agent(provider)) {
        if (const auto agents = agents::installed_agents_if_set()) {
            scan = agents->cached_scan();
        }
    }
    return providerSummary(settings, keySaved, scan);
}

ProviderSummary providerSummary(const Json& settings,
                                const std::function<bool(const std::string& env)>& keySaved,
                                const std::optional<agents::ScanResult>& scan) {
    const std::string provider = llm::resolve_transcript_cleanup_provider(settings);
    ProviderSummary summary;
    if (llm::is_installed_cleanup_agent(provider)) {
        const QString name = qt::toQString(agents::agent_spec(provider).name);
        summary.name = name;
        summary.status = QStringLiteral("Uses your agent's sign-in.");
        if (scan) {
            const auto found = scan->find(provider);
            if (found == scan->end() || !found->second || !found->second->usable()) {
                summary.status =
                    QStringLiteral("Install or sign in to this agent in All settings.");
            }
        }
        summary.privacy = QStringLiteral("Transcript text is sent to %1's model when cleanup is "
                                         "on.")
                              .arg(name) +
                          QString::fromLatin1(kNoAudio);
        summary.remote = true;
        return summary;
    }
    const std::optional<llm::TextLLMProfile> profile = llm::get_profile(provider, settings);
    if (!profile) {
        summary.name = QStringLiteral("Provider unavailable");
        summary.status = QStringLiteral("Choose a provider in All settings.");
        summary.privacy = QStringLiteral("The saved cleanup provider is unavailable. Choose a "
                                         "provider before using AI cleanup.");
        summary.remote = true;
        return summary;
    }
    const bool local = profile->is_local();
    const QString name = qt::toQString(profile->name);
    summary.name = name;
    summary.privacy =
        (local
             ? QStringLiteral("Transcript text is sent to %1 on this computer when cleanup is on.")
             : QStringLiteral("Transcript text is sent to %1 when cleanup is on."))
            .arg(name) +
        QString::fromLatin1(kNoAudio);
    if (!profile->api_key_env.empty()) {
        if (keySaved && keySaved(profile->api_key_env)) {
            summary.status = QStringLiteral("API key saved · Connection not tested");
        } else {
            summary.status = QStringLiteral("API key needed");
            summary.needsKey = true;
        }
    } else {
        summary.status = QStringLiteral("No API key needed · Connection not tested");
    }
    summary.remote = !local;
    return summary;
}

std::string customCleanupPrompt(const Json& settings) {
    return text::custom_prompt(settings);
}

// What lookups on the worker reach: gone, or with no owner, once the KeyPresence is.
struct KeyPresence::State {
    State(Lookup lookupFn, Executor& workerExecutor, Executor& uiExecutor)
        : lookup(std::move(lookupFn)), worker(workerExecutor), ui(uiExecutor) {}

    Lookup lookup;
    Executor& worker;
    Executor& ui;
    // Read and cleared on the GUI thread only.
    KeyPresence* owner = nullptr;
};

KeyPresence::KeyPresence(Lookup lookup, Executor& worker, Executor& ui, QObject* parent)
    : QObject(parent), state_(std::make_shared<State>(std::move(lookup), worker, ui)) {
    state_->owner = this;
}

KeyPresence::~KeyPresence() {
    state_->owner = nullptr;
}

bool KeyPresence::saved(const std::string& env) {
    const auto found = known_.find(env);
    if (found == known_.end()) {
        // A first lookup already running answers this question too.
        if (!pending_.contains(env)) {
            check(env);
        }
        return false;
    }
    return found->second;
}

void KeyPresence::check(const std::string& env) {
    if (!pending_.insert(env).second) {
        again_.insert(env);
        return;
    }
    std::weak_ptr<State> weak = state_;
    state_->worker.post([weak, env] {
        const std::shared_ptr<State> state = weak.lock();
        if (state == nullptr) {
            return;
        }
        bool found = false;
        try {
            found = state->lookup && state->lookup(env);
        } catch (const std::exception& error) {
            kLog.warning("Couldn't read whether an API key is saved ({})",
                         type_display_name(typeid(error)));
        }
        state->ui.post([weak, env, found] {
            const std::shared_ptr<State> answered = weak.lock();
            if (answered == nullptr || answered->owner == nullptr) {
                return;
            }
            KeyPresence& self = *answered->owner;
            self.pending_.erase(env);
            const auto known = self.known_.find(env);
            const bool differs = known == self.known_.end() ? found : known->second != found;
            self.known_[env] = found;
            if (self.again_.erase(env) > 0) {
                self.check(env);
            }
            if (differs) {
                Q_EMIT self.changed();
            }
        });
    });
}

void KeyPresence::recheck() {
    std::vector<std::string> names;
    for (const auto& [env, found] : known_) {
        names.push_back(env);
    }
    // A first lookup still running may have read the store before the change.
    for (const std::string& env : pending_) {
        if (!known_.contains(env)) {
            names.push_back(env);
        }
    }
    for (const std::string& env : names) {
        check(env);
    }
}

} // namespace fv
