#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/env.h"
#include "base/identity.h"
#include "credentials/dotenv.h"

namespace fv {

struct CredentialStatus {
    bool available;
    std::string backend_name;
    std::string reason;
};

// The OS store refused an operation. The message never contains a secret.
class CredentialStoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The OS store accepted a call but never answered.
class CredentialStoreTimeout : public CredentialStoreError {
public:
    using CredentialStoreError::CredentialStoreError;
};

// WinCred now; Keychain and libsecret later. The Windows backend uses keyring's WinVault target
// convention: the target <service> holds the newest key (when its UserName is the name asked
// for), older ones live at <name>@<service>.
// Affinity: called by CredentialStore only, one call at a time.
class CredentialBackend {
public:
    virtual ~CredentialBackend() = default;
    // nullopt when nothing is stored. Throws on any other failure (CredentialStoreTimeout when
    // the store never answered).
    virtual std::optional<std::string> get(std::string_view service, std::string_view name) = 0;
    virtual void set(std::string_view service, std::string_view name, std::string_view value) = 0;
    // false when nothing was stored under the name.
    virtual bool remove(std::string_view service, std::string_view name) = 0;
    // Whether the store can be used now; asked again by refresh() and request_access().
    virtual CredentialStatus probe() = 0;
};

// Caches, serializes backend calls, never logs values. Lookups are cached (Settings resolves
// every provider's key on each redraw), including failed ones, so a dismissed access prompt is
// not shown again until set, remove, refresh or request_access. A change made in the OS store
// while the app runs is seen on the next launch.
// Affinity: thread-safe; blocking, never on the GUI thread.
class CredentialStore {
public:
    CredentialStore(std::shared_ptr<CredentialBackend>, const AppIdentity&);
    CredentialStore(const CredentialStore&) = delete;
    CredentialStore& operator=(const CredentialStore&) = delete;

    // The saved key, or nullopt when none is saved, the store is unusable or the read failed.
    // Throws std::invalid_argument for an empty name.
    std::optional<std::string> get(std::string_view name);
    // Stores validate_api_key(value). Throws std::invalid_argument with user copy for a value
    // that cannot be a key, CredentialStoreError when the store is unusable or refuses.
    void set(std::string_view name, std::string_view value);
    // false when nothing was stored. Throws CredentialStoreError as set does.
    bool remove(std::string_view name);
    CredentialStatus status();

    bool has(std::string_view name);
    // Probes the store again (a failed probe is remembered until then) and forgets every lookup.
    CredentialStatus refresh();
    // Re-probes and re-reads one key on an explicit user action, so the store may show its
    // access prompt. Throws CredentialStoreError when the store is unusable or the read fails.
    void request_access(std::string_view name);

    std::string_view service() const { return service_; }

private:
    CredentialStatus status_locked();
    void require_backend_locked();
    void mark_unresponsive_locked();

    std::shared_ptr<CredentialBackend> backend_;
    std::string service_;
    std::mutex mutex_;
    std::optional<CredentialStatus> status_;
    std::map<std::string, std::optional<std::string>, std::less<>> cache_;
};

enum class CredentialSource { Store, Environment, DotEnv, None };

struct Resolved {
    std::optional<std::string> value;
    CredentialSource source;
};

// Saved key, then the process environment, then .env, as OpenWhisper's resolution order. The
// source is what credential_source() reports for the same state, except that a variable set to
// "" (possible outside Windows) ends the lookup with no value. .env is never copied into the
// process environment (OpenWhisper loads it there for child processes; ProcessEnv does that job).
Resolved resolve_credential(CredentialStore&, const EnvReader&, const DotEnv&, std::string_view);
// "••••" and the last four code points; "••••" alone for "" or a key under 12 code points.
std::string mask_key(std::string_view key);

} // namespace fv
