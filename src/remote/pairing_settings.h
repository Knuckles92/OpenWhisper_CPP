#pragma once

#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/json.h"
#include "base/paths.h"
#include "credentials/dotenv.h"
#include "credentials/store.h"
#include "remote/protocol.h"
#include "settings/store.h"

// Persisted remote engine state for both roles (services/remote_asr/settings.py). Client: the
// paired host's address, pinned certificate fingerprint and name live in settings
// (remote_engine_client); the device token lives in the OS credential store. Host: whether
// sharing is on, the port, and the paired devices (token digests only).
//
// Pairing, forgetting, and following a moved host each read then write the saved pairing under
// one process-wide lock, so a connection finishing as the host is forgotten never writes it back.
// Affinity: thread-safe; everything that takes a SettingsStore or CredentialStore blocks on
// files or the OS store, so never on the GUI thread.
namespace fv::remote {

// The paired host. Two are equal when they are the same pairing: the addresses aren't compared
// (they follow the host as it moves, and an open window holding the pairing from before must
// still find it the same one), nor is the name, which this computer's owner may change.
struct ClientPairing {
    std::string host;
    int port = DEFAULT_PORT;
    std::string fingerprint;
    // What this computer calls the host: the name it gave when pairing, or the one it was
    // renamed to here.
    std::string host_name;
    std::string device_id;
    std::string paired_at; // "YYYY-MM-DDTHH:MM:SS+00:00"
    // The host's other addresses (LAN, Tailscale), tried when `host` isn't.
    std::vector<std::string> alternates;
    std::string via = "code"; // one of PAIRING_ROUTES
    // The name the host gave when pairing, kept while it goes by another here; empty when it
    // hasn't been renamed.
    std::string paired_name;

    bool renamed() const { return !paired_name.empty(); }
    // What to call the host, given the name it reported just now: the name chosen here when
    // renamed, else the reported one, else host_name.
    std::string name_for(std::string_view reported) const;
    std::string address() const; // format_address(host, port)
    bool over_tailscale() const; // host is a Tailscale address
    // A Tailscale address to fall back on when `host` is a LAN one: the first Tailscale
    // alternate, or "".
    std::string tailscale_fallback() const;

    friend bool operator==(const ClientPairing& a, const ClientPairing& b) {
        return a.fingerprint == b.fingerprint && a.device_id == b.device_id &&
               a.paired_at == b.paired_at && a.via == b.via;
    }
};

// The saved pairing in a settings mapping, or nullopt when there is none or it is unusable (no
// host or fingerprint string, a port int() can't read or outside 1..65535). host_name defaults to
// the host; alternates keep the non-empty strings other than the host; an unknown `via` reads
// "code". py::OverflowError for an infinite port, as Python's int() raises.
std::optional<ClientPairing> load_client_pairing(const Json& settings);
std::optional<ClientPairing> load_client_pairing(const SettingsStore& settings);

// The credential name holding this data directory's device token:
// <env_prefix>REMOTE_ENGINE_TOKEN for the per-user directory, else that name and "_" and the
// first 12 hex digits of SHA-256 of the data root's normcase(realpath()), uppercased. Pairings
// live in each data directory's settings but the OS credential store is per user, so a data
// directory of its own (<env_prefix>DATA_DIR) pairs as a separate device and never overwrites the
// installed app's token.
std::string token_credential(const Paths& paths, const AppIdentity& identity);
std::string token_credential(const std::filesystem::path& data_root,
                             const std::filesystem::path& local_app_dir,
                             const AppIdentity& identity);
// <env_prefix>REMOTE_ENGINE_TOKEN.
std::string unscoped_token_credential(const AppIdentity& identity);
// normcase(realpath(path)) as UTF-8: absolute, links resolved, normalized; on Windows "/" as "\"
// and lowercased as LCMapStringEx(invariant, LCMAP_LOWERCASE) does.
std::string normalized_path(const std::filesystem::path& path);

// The device token: the scoped credential (saved key, then environment, then .env); for a
// scoped name with no value, the unscoped one, which is read but never written or deleted from
// here (it may be the installed app's).
std::optional<std::string> load_client_token(CredentialStore& credentials, const EnvReader& env,
                                             const DotEnv& dotenv, const Paths& paths,
                                             const AppIdentity& identity);

// Stores a fresh pairing: the token first (CredentialStoreError or std::invalid_argument
// propagate, leaving settings untouched, so settings never name a host this computer can't use),
// then the pairing, paired now (UTC, whole seconds), alternates without `host`.
ClientPairing save_client_pairing(SettingsStore& settings, CredentialStore& credentials,
                                  const Paths& paths, const AppIdentity& identity,
                                  std::string_view host, int port, const PairingResult& result,
                                  Clock& clock);

// Writes `pairing` as remote_engine_client: host, port, fingerprint, host_name, device_id,
// paired_at, alternates, via, and paired_name when renamed.
void write_pairing(SettingsStore& settings, const ClientPairing& pairing);
// The dict write_pairing saves.
Json pairing_json(const ClientPairing& pairing);

// Follows the paired host to where it is now; the new pairing, or nullopt when this computer is
// paired with another host (by fingerprint, without regard to case) or nothing changed (nothing
// is written). `listed` are the addresses the host just listed in `ready` and `reached` the one
// that answered on `port`. The address paired with stays first unless it's an IP the host no
// longer has that didn't answer, as when the router gave the host a new one; the host's own LAN
// address (or else the one that answered) takes its place. A Tailscale address stays known while
// the host lists none, since Tailscale keeps it for good and may only be off there for now.
std::optional<ClientPairing> remember_host_addresses(SettingsStore& settings,
                                                     std::string_view fingerprint,
                                                     std::string_view reached, int port,
                                                     std::span<const std::string> listed);
// The same over ready.host.addresses as received, iterated as Python iterates it: a list's
// entries that are non-empty strings, a dict's keys, a string's characters; py::TypeError for
// null, a bool or a number. (The client only passes a list.)
std::optional<ClientPairing> remember_ready_addresses(SettingsStore& settings,
                                                      std::string_view fingerprint,
                                                      std::string_view reached, int port,
                                                      const Json& listed);

// Calls the paired host `name` here (clean_name); an empty name goes back to the one it gave.
// The renamed pairing (written only when the name changed), or nullopt when not paired.
std::optional<ClientPairing> rename_client_host(SettingsStore& settings, std::string_view name);

// Drops the saved pairing, then deletes this data directory's token; a failure to delete it is
// logged ("Could not delete the remote engine token: <class>") and swallowed.
void forget_client_pairing(SettingsStore& settings, CredentialStore& credentials,
                           const Paths& paths, const AppIdentity& identity);

// The host's settings, as read from a settings mapping (SettingsStore::snapshot()).
bool host_enabled(const Json& settings);
// Off by default; pairing alone never grants model administration.
bool host_model_management(const Json& settings);
// Off by default; pairing alone never lets a computer store files here.
bool host_keeps_records(const Json& settings);
// Off by default; pairing alone never lets a computer run this one's MCP.
bool host_manages_mcp(const Json& settings);
bool client_shares_history(const Json& settings);
// Where this computer keeps its records while paired; "local" by default.
std::string records_location(const Json& settings);
int host_port(const Json& settings);
// On unless turned off: the owner's own Tailscale computers pair without a code.
bool host_tailscale_trust(const Json& settings);
// The paired devices, read on their own (the host checks them for every request).
Json load_host_devices(const SettingsStore& settings);
void save_host_devices(SettingsStore& settings, const Json& devices);

} // namespace fv::remote
