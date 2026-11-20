#pragma once

#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The system proxy as urllib.request reads it (getproxies, proxy_bypass): <scheme>_proxy
// environment variables first (httpx's trust_env reads the same ones), then on Windows the
// Internet Settings registry key. The parsing is pure; read_system_proxy_settings() is the only
// part that touches the platform.
namespace fv::http {

// Scheme ("http", "https", "ftp", "socks", "no", ...) to proxy URL, as Python's dict.
using ProxyMap = std::map<std::string, std::string, std::less<>>;

struct ProxySettings {
    ProxyMap environment; // getproxies_environment()
    ProxyMap registry;    // getproxies_registry(); always empty outside Windows
    // ProxyOverride, kept only when ProxyEnable is set (proxy_bypass_registry ignores it
    // otherwise).
    std::string registry_override;
};

// getproxies_environment over the environment as os.environ lists it ((name, value) pairs; on
// Windows Python upper-cases every name, so the lower-case second pass never matches there).
// `cgi` is "REQUEST_METHOD is set": HTTP_PROXY is then ignored (CVE-2016-1000110).
ProxyMap proxies_from_environment(std::span<const std::pair<std::string, std::string>> entries,
                                  bool cgi);

// getproxies_registry's reading of ProxyServer ("host:port" for every scheme, or
// "http=a:1;https=b:2", with Windows' default http:// and socks:// prefixes and the socks
// fallback). A malformed entry ends the parse and keeps what came before it, as the Python does.
ProxyMap proxies_from_registry(std::string_view proxy_server);

// proxy_bypass_environment: the "no" entry (no_proxy) as comma-separated DNS suffixes, or "*".
bool proxy_bypass_environment(std::string_view host, const ProxyMap& proxies);

// _proxy_bypass_winreg_override: ";"-separated fnmatch patterns, "<local>" for dotless hosts.
bool proxy_bypass_registry_override(std::string_view host, std::string_view override_list);

// The proxy urllib's ProxyHandler would use for `url`: the environment's map when it has any
// entry, else the registry's; none when the scheme has no entry or the host is bypassed.
std::optional<std::string> proxy_for_url(std::string_view url, const ProxySettings&);

// The current process environment and, on Windows, HKCU's Internet Settings. Read on every call,
// as urllib builds a ProxyHandler per opener.
ProxySettings read_system_proxy_settings();

namespace detail {
// The process environment as (name, value) pairs, names upper-cased on Windows as os.environ does.
std::vector<std::pair<std::string, std::string>> environment_entries();
// HKCU's ProxyEnable, ProxyServer and ProxyOverride; nullopt outside Windows or when unreadable.
struct RegistryProxy {
    bool enabled = false;
    std::optional<std::string> server;
    std::optional<std::string> override_list;
};
std::optional<RegistryProxy> read_registry_proxy();
} // namespace detail

} // namespace fv::http
