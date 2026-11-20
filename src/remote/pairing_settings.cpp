#include "remote/pairing_settings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <mutex>
#include <set>
#include <stdexcept>
#include <system_error>
#include <utility>

#include "base/hash.h"
#include "base/log.h"
#include "base/utf8.h"
#include "credentials/resolve.h"
#include "net/address.h"
#include "net/tailscale.h"
#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"
#include "remote/detail/normcase.h"
#include "remote/detail/pyvalue.h"
#include "settings/keys.h"

namespace fv::remote {

namespace {

constexpr log::Logger kLog("services.remote_asr.settings");

std::recursive_mutex& pairing_lock() {
    static std::recursive_mutex mutex;
    return mutex;
}

const Json& member(const Json& object, std::string_view key) {
    static const Json none;
    if (!object.is_object()) {
        return none;
    }
    const auto found = object.find(std::string(key));
    return found == object.end() ? none : *found;
}

bool is_ip(std::string_view text) {
    return net::parse_address(text).has_value();
}

bool contains(const std::vector<std::string>& values, std::string_view value) {
    return std::find(values.begin(), values.end(), value) != values.end();
}

void add_unique(std::vector<std::string>& out, const std::string& value) {
    if (!contains(out, value)) {
        out.push_back(value);
    }
}

std::string exception_class(const std::exception& error) {
    if (dynamic_cast<const CredentialStoreTimeout*>(&error) != nullptr) {
        return "CredentialStoreTimeout";
    }
    if (dynamic_cast<const CredentialStoreError*>(&error) != nullptr) {
        return "CredentialStoreError";
    }
    if (dynamic_cast<const std::invalid_argument*>(&error) != nullptr) {
        return "ValueError";
    }
    return "Exception";
}

std::optional<ClientPairing> remember_locked(SettingsStore& settings, std::string_view fingerprint,
                                             std::string_view reached, int port,
                                             const std::vector<std::string>& listed) {
    std::optional<ClientPairing> moved;
    std::string from;
    {
        std::lock_guard lock(pairing_lock());
        const auto pairing = load_client_pairing(settings);
        if (!pairing || py::upper(pairing->fingerprint) != py::upper(fingerprint)) {
            return std::nullopt;
        }
        std::string host = pairing->host;
        int host_port = pairing->port;
        if (reached == host) {
            host_port = port;
        } else if (is_ip(host) && !contains(listed, host)) {
            host.clear();
            for (const auto& address : listed) {
                if (!net::is_tailscale_address(address)) {
                    host = address;
                    break;
                }
            }
            if (host.empty()) {
                host = std::string(reached);
            }
            host_port = port;
        }
        std::vector<std::string> known_tailnet;
        for (const auto& address : pairing->alternates) {
            if (net::is_tailscale_address(address)) {
                known_tailnet.push_back(address);
            }
        }
        if (std::any_of(listed.begin(), listed.end(),
                        [](const std::string& a) { return net::is_tailscale_address(a); })) {
            known_tailnet.clear();
        }
        std::vector<std::string> candidates = listed;
        candidates.emplace_back(reached);
        candidates.insert(candidates.end(), known_tailnet.begin(), known_tailnet.end());
        std::vector<std::string> alternates;
        for (const auto& address : candidates) {
            if (!address.empty() && address != host) {
                add_unique(alternates, address);
            }
        }
        if (host == pairing->host && host_port == pairing->port &&
            alternates == pairing->alternates) {
            return std::nullopt;
        }
        moved = *pairing;
        moved->host = host;
        moved->port = host_port;
        moved->alternates = std::move(alternates);
        write_pairing(settings, *moved);
        from = pairing->host;
    }
    if (moved->host != from) {
        kLog.info("The paired host {} moved from {} to {}", moved->host_name, from, moved->host);
    }
    return moved;
}

} // namespace

std::string ClientPairing::name_for(std::string_view reported) const {
    if (renamed()) {
        return host_name;
    }
    return reported.empty() ? host_name : std::string(reported);
}

std::string ClientPairing::address() const {
    return format_address(host, port);
}

bool ClientPairing::over_tailscale() const {
    return net::is_tailscale_address(host);
}

std::string ClientPairing::tailscale_fallback() const {
    if (over_tailscale()) {
        return "";
    }
    for (const auto& address : alternates) {
        if (net::is_tailscale_address(address)) {
            return address;
        }
    }
    return "";
}

std::optional<ClientPairing> load_client_pairing(const Json& settings) {
    const Json raw = keys::remote_engine_client.read(settings);
    if (!raw.is_object()) {
        return std::nullopt;
    }
    const Json& host = member(raw, "host");
    const Json& fingerprint = member(raw, "fingerprint");
    std::int64_t port = DEFAULT_PORT;
    if (raw.contains("port")) {
        const Json& value = raw["port"];
        const auto converted = py::py_int_checked(value);
        if (!converted) {
            if (converted.error() == py::IntError::Overflow && value.is_number_float() &&
                std::isinf(value.get<double>())) {
                throw py::OverflowError("cannot convert float infinity to integer");
            }
            // Type and Value are what Python catches; a finite value past int64 is out of range.
            return std::nullopt;
        }
        port = *converted;
    }
    if (!host.is_string() || host.get_ref<const std::string&>().empty() ||
        !fingerprint.is_string() || fingerprint.get_ref<const std::string&>().empty()) {
        return std::nullopt;
    }
    if (port < 1 || port > 65535) {
        return std::nullopt;
    }
    ClientPairing pairing;
    pairing.host = host.get<std::string>();
    pairing.port = static_cast<int>(port);
    pairing.fingerprint = fingerprint.get<std::string>();
    const std::string host_name = detail::py_str_or_empty(member(raw, "host_name"));
    pairing.host_name = host_name.empty() ? pairing.host : host_name;
    pairing.device_id = detail::py_str_or_empty(member(raw, "device_id"));
    pairing.paired_at = detail::py_str_or_empty(member(raw, "paired_at"));
    const Json& alternates = member(raw, "alternates");
    if (alternates.is_array()) {
        for (const auto& address : alternates) {
            if (address.is_string() && !address.get_ref<const std::string&>().empty() &&
                address.get_ref<const std::string&>() != pairing.host) {
                pairing.alternates.push_back(address.get<std::string>());
            }
        }
    }
    const Json& via = member(raw, "via");
    pairing.via = "code";
    if (via.is_string()) {
        const auto& text = via.get_ref<const std::string&>();
        if (std::find(PAIRING_ROUTES.begin(), PAIRING_ROUTES.end(), text) != PAIRING_ROUTES.end()) {
            pairing.via = text;
        }
    }
    pairing.paired_name = detail::py_str_or_empty(member(raw, "paired_name"));
    return pairing;
}

std::optional<ClientPairing> load_client_pairing(const SettingsStore& settings) {
    return load_client_pairing(*settings.snapshot());
}

std::string normalized_path(const std::filesystem::path& path) {
    std::error_code error;
    std::filesystem::path absolute = std::filesystem::absolute(path, error);
    if (error) {
        absolute = path;
    }
    std::filesystem::path real = std::filesystem::weakly_canonical(absolute, error);
    if (error) {
        real = absolute.lexically_normal();
    }
    real.make_preferred();
    std::string text = path_to_utf8(real);
    const std::size_t root = path_to_utf8(real.root_path()).size();
    while (text.size() > root && (text.back() == '\\' || text.back() == '/')) {
        text.pop_back();
    }
    return detail::normcase(text);
}

std::string unscoped_token_credential(const AppIdentity& identity) {
    return std::string(identity.env_prefix) + std::string(TOKEN_CREDENTIAL_STEM);
}

std::string token_credential(const std::filesystem::path& data_root,
                             const std::filesystem::path& local_app_dir,
                             const AppIdentity& identity) {
    const std::string root = normalized_path(data_root);
    const std::string name = unscoped_token_credential(identity);
    if (root == normalized_path(local_app_dir)) {
        return name;
    }
    std::string suffix = sha256_hex(root).substr(0, 12);
    std::transform(suffix.begin(), suffix.end(), suffix.begin(), [](char ch) {
        return ch >= 'a' && ch <= 'f' ? static_cast<char>(ch - 32) : ch;
    });
    return name + "_" + suffix;
}

std::string token_credential(const Paths& paths, const AppIdentity& identity) {
    return token_credential(paths.data_root(), paths.local_app_dir(), identity);
}

std::optional<std::string> load_client_token(CredentialStore& credentials, const EnvReader& env,
                                             const DotEnv& dotenv, const Paths& paths,
                                             const AppIdentity& identity) {
    const std::string name = token_credential(paths, identity);
    const std::string unscoped = unscoped_token_credential(identity);
    auto token = resolve_credential(credentials, env, dotenv, name).value;
    if ((token && !token->empty()) || name == unscoped) {
        return token;
    }
    // A data directory paired before tokens were scoped saved under the shared name. Read it
    // until the next pairing saves a scoped one; never write or delete it from here, since it may
    // be the installed app's.
    return resolve_credential(credentials, env, dotenv, unscoped).value;
}

Json pairing_json(const ClientPairing& pairing) {
    Json saved = Json::object();
    saved["host"] = pairing.host;
    saved["port"] = pairing.port;
    saved["fingerprint"] = pairing.fingerprint;
    saved["host_name"] = pairing.host_name;
    saved["device_id"] = pairing.device_id;
    saved["paired_at"] = pairing.paired_at;
    saved["alternates"] = pairing.alternates;
    saved["via"] = pairing.via;
    if (!pairing.paired_name.empty()) {
        saved["paired_name"] = pairing.paired_name;
    }
    return saved;
}

void write_pairing(SettingsStore& settings, const ClientPairing& pairing) {
    settings.save_setting(keys::remote_engine_client.key(), pairing_json(pairing));
}

ClientPairing save_client_pairing(SettingsStore& settings, CredentialStore& credentials,
                                  const Paths& paths, const AppIdentity& identity,
                                  std::string_view host, int port, const PairingResult& result,
                                  Clock& clock) {
    credentials.set(token_credential(paths, identity), result.token);
    ClientPairing pairing;
    pairing.host = std::string(host);
    pairing.port = port;
    pairing.fingerprint = result.fingerprint;
    pairing.host_name = result.host_name;
    pairing.device_id = result.device_id;
    pairing.paired_at = std::format("{:%Y-%m-%dT%H:%M:%S}+00:00",
                                    std::chrono::floor<std::chrono::seconds>(clock.utc()));
    for (const auto& address : result.alternates) {
        if (address != host) {
            pairing.alternates.push_back(address);
        }
    }
    pairing.via = result.via;
    write_pairing(settings, pairing);
    return pairing;
}

std::optional<ClientPairing> remember_host_addresses(SettingsStore& settings,
                                                     std::string_view fingerprint,
                                                     std::string_view reached, int port,
                                                     std::span<const std::string> listed) {
    std::vector<std::string> kept;
    for (const auto& address : listed) {
        if (!address.empty()) {
            add_unique(kept, address);
        }
    }
    return remember_locked(settings, fingerprint, reached, port, kept);
}

std::optional<ClientPairing> remember_ready_addresses(SettingsStore& settings,
                                                      std::string_view fingerprint,
                                                      std::string_view reached, int port,
                                                      const Json& listed) {
    std::vector<std::string> kept;
    // `for a in addresses` of whatever arrived: a list's entries, a dict's keys, a string's
    // characters; anything else is not iterable.
    if (listed.is_array()) {
        for (const auto& address : listed) {
            if (address.is_string() && !address.get_ref<const std::string&>().empty()) {
                add_unique(kept, address.get<std::string>());
            }
        }
    } else if (listed.is_object()) {
        for (const auto& item : listed.items()) {
            if (!item.key().empty()) {
                add_unique(kept, item.key());
            }
        }
    } else if (listed.is_string()) {
        for (const char32_t cp : py::to_code_points(listed.get_ref<const std::string&>())) {
            std::string character;
            py::append_utf8(character, cp);
            add_unique(kept, character);
        }
    } else {
        throw py::TypeError(
            std::format("'{}' object is not iterable", listed.is_null()           ? "NoneType"
                                                       : listed.is_boolean()      ? "bool"
                                                       : listed.is_number_float() ? "float"
                                                                                  : "int"));
    }
    return remember_locked(settings, fingerprint, reached, port, kept);
}

std::optional<ClientPairing> rename_client_host(SettingsStore& settings, std::string_view name) {
    std::lock_guard lock(pairing_lock());
    const auto pairing = load_client_pairing(settings);
    if (!pairing) {
        return std::nullopt;
    }
    const std::string own =
        pairing->paired_name.empty() ? pairing->host_name : pairing->paired_name;
    std::string chosen = clean_name(name);
    if (chosen.empty()) {
        chosen = own;
    }
    ClientPairing renamed = *pairing;
    renamed.host_name = chosen;
    renamed.paired_name = chosen != own ? own : std::string();
    if (renamed.host_name != pairing->host_name || renamed.paired_name != pairing->paired_name) {
        write_pairing(settings, renamed);
    }
    return renamed;
}

void forget_client_pairing(SettingsStore& settings, CredentialStore& credentials,
                           const Paths& paths, const AppIdentity& identity) {
    {
        std::lock_guard lock(pairing_lock());
        settings.update(Json::object(), {keys::remote_engine_client.key()});
    }
    try {
        (void)credentials.remove(token_credential(paths, identity));
    } catch (const std::exception& error) {
        kLog.warning("Could not delete the remote engine token: {}", exception_class(error));
    }
}

bool host_enabled(const Json& settings) {
    return keys::remote_host_enabled.read(settings);
}

bool host_model_management(const Json& settings) {
    return keys::remote_host_model_management.read(settings);
}

bool host_keeps_records(const Json& settings) {
    return keys::remote_host_keep_records.read(settings);
}

bool host_manages_mcp(const Json& settings) {
    return keys::remote_host_manage_mcp.read(settings);
}

bool client_shares_history(const Json& settings) {
    return keys::remote_client_history.read(settings);
}

std::string records_location(const Json& settings) {
    return keys::remote_records_location.read(settings);
}

int host_port(const Json& settings) {
    return static_cast<int>(keys::remote_host_port.read(settings));
}

bool host_tailscale_trust(const Json& settings) {
    return keys::remote_host_tailscale_trust.read(settings);
}

Json load_host_devices(const SettingsStore& settings) {
    return keys::remote_host_devices.read(
        settings.load_settings({keys::remote_host_devices.key()}));
}

void save_host_devices(SettingsStore& settings, const Json& devices) {
    settings.save_setting(keys::remote_host_devices.key(), devices);
}

} // namespace fv::remote
