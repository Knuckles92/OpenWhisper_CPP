#include "http/proxy.h"

#include <algorithm>
#include <cctype>
#include <cstddef>

namespace fv::http {

namespace {

constexpr std::string_view kSuffix = "_proxy";

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), ascii_lower);
    return out;
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string_view strip(std::string_view text) {
    while (!text.empty() && is_space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && is_space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto at = text.find(separator, start);
        if (at == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, at - start));
        start = at + 1;
    }
}

// urllib's _splitport: "(.*):([0-9]*)" matched against the whole host.
std::string_view host_without_port(std::string_view host) {
    const auto colon = host.rfind(':');
    if (colon == std::string_view::npos) {
        return host;
    }
    const auto port = host.substr(colon + 1);
    if (!std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        return host;
    }
    return host.substr(0, colon);
}

// re.match('(?:[^/:]+)://', address)
bool has_scheme(std::string_view address) {
    std::size_t i = 0;
    while (i < address.size() && address[i] != '/' && address[i] != ':') {
        ++i;
    }
    return i > 0 && address.substr(i).starts_with("://");
}

// fnmatch.fnmatchcase over already normalized text: *, ?, [seq] and [!seq].
bool glob_match(std::string_view name, std::string_view pattern) {
    std::size_t n = 0;
    std::size_t p = 0;
    std::size_t star_p = std::string_view::npos;
    std::size_t star_n = 0;
    while (n < name.size()) {
        bool advanced = false;
        if (p < pattern.size()) {
            const char c = pattern[p];
            if (c == '*') {
                star_p = p++;
                star_n = n;
                continue;
            }
            if (c == '?') {
                ++p;
                ++n;
                advanced = true;
            } else if (c == '[') {
                // A set runs to the next "]" after its first character; without one, "[" is
                // literal (fnmatch.translate).
                std::size_t j = p + 1;
                if (j < pattern.size() && pattern[j] == '!') {
                    ++j;
                }
                if (j < pattern.size() && pattern[j] == ']') {
                    ++j;
                }
                while (j < pattern.size() && pattern[j] != ']') {
                    ++j;
                }
                if (j >= pattern.size()) {
                    if (name[n] == '[') {
                        ++p;
                        ++n;
                        advanced = true;
                    }
                } else {
                    auto set = pattern.substr(p + 1, j - p - 1);
                    const bool negate = set.starts_with('!');
                    if (negate) {
                        set.remove_prefix(1);
                    }
                    bool found = false;
                    const char ch = name[n];
                    for (std::size_t k = 0; k < set.size();) {
                        if (k + 2 < set.size() && set[k + 1] == '-') {
                            found = found || (set[k] <= ch && ch <= set[k + 2]);
                            k += 3;
                        } else {
                            found = found || set[k] == ch;
                            ++k;
                        }
                    }
                    if (found != negate) {
                        p = j + 1;
                        ++n;
                        advanced = true;
                    }
                }
            } else if (c == name[n]) {
                ++p;
                ++n;
                advanced = true;
            }
        }
        if (!advanced) {
            if (star_p == std::string_view::npos) {
                return false;
            }
            p = star_p + 1;
            n = ++star_n;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') {
        ++p;
    }
    return p == pattern.size();
}

// os.path.normcase on Windows: lower case, "/" as "\".
std::string normcase(std::string_view text) {
    std::string out = lower(text);
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

struct UrlTarget {
    std::string scheme;
    std::string host; // Request.host: the netloc, port included
};

// urllib's _splittype and _splithost.
std::optional<UrlTarget> split_url(std::string_view url) {
    const auto colon = url.find(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    const auto scheme = url.substr(0, colon);
    if (scheme.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    auto rest = url.substr(colon + 1);
    if (!rest.starts_with("//")) {
        return UrlTarget{lower(scheme), ""};
    }
    rest.remove_prefix(2);
    const auto end = rest.find_first_of("/#?");
    return UrlTarget{lower(scheme), std::string(rest.substr(0, end))};
}

} // namespace

ProxyMap proxies_from_environment(std::span<const std::pair<std::string, std::string>> entries,
                                  bool cgi) {
    ProxyMap proxies;
    for (const auto& [name, value] : entries) {
        if (name.size() > 5 && name[name.size() - 6] == '_' &&
            lower(std::string_view(name).substr(name.size() - 5)) == "proxy") {
            if (!value.empty()) {
                proxies[lower(std::string_view(name).substr(0, name.size() - 6))] = value;
            }
        }
    }
    if (cgi) {
        proxies.erase("http");
    }
    for (const auto& [name, value] : entries) {
        if (name.size() >= kSuffix.size() && std::string_view(name).ends_with(kSuffix) &&
            name.size() > 5 && name[name.size() - 6] == '_') {
            const auto key = lower(std::string_view(name).substr(0, name.size() - 6));
            if (!value.empty()) {
                proxies[key] = value;
            } else {
                proxies.erase(key);
            }
        }
    }
    return proxies;
}

ProxyMap proxies_from_registry(std::string_view proxy_server) {
    std::string server(proxy_server);
    if (server.find('=') == std::string::npos && server.find(';') == std::string::npos) {
        server = "http=" + server + ";https=" + server + ";ftp=" + server;
    }
    ProxyMap proxies;
    for (const auto entry : split(server, ';')) {
        const auto equals = entry.find('=');
        if (equals == std::string_view::npos) {
            // Python's tuple unpacking raises ValueError here, which ends the parse.
            return proxies;
        }
        const std::string protocol(entry.substr(0, equals));
        std::string address(entry.substr(equals + 1));
        if (!has_scheme(address)) {
            if (protocol == "http" || protocol == "https" || protocol == "ftp") {
                address = "http://" + address;
            } else if (protocol == "socks") {
                address = "socks://" + address;
            }
        }
        proxies[protocol] = address;
    }
    if (const auto socks = proxies.find("socks");
        socks != proxies.end() && !socks->second.empty()) {
        std::string address = socks->second;
        if (address.starts_with("socks://")) {
            address = "socks4://" + address.substr(8);
        }
        for (const char* scheme : {"http", "https"}) {
            auto& slot = proxies[scheme];
            if (slot.empty()) {
                slot = address;
            }
        }
    }
    return proxies;
}

bool proxy_bypass_environment(std::string_view host, const ProxyMap& proxies) {
    const auto no_proxy = proxies.find("no");
    if (no_proxy == proxies.end()) {
        return false;
    }
    if (no_proxy->second == "*") {
        return true;
    }
    const std::string full = lower(host);
    const std::string only(host_without_port(full));
    for (auto name : split(no_proxy->second, ',')) {
        name = strip(name);
        if (name.empty()) {
            continue;
        }
        while (name.starts_with('.')) {
            name.remove_prefix(1);
        }
        const std::string entry = lower(name);
        if (only == entry || full == entry) {
            return true;
        }
        const std::string suffix = "." + entry;
        if (only.ends_with(suffix) || full.ends_with(suffix)) {
            return true;
        }
    }
    return false;
}

bool proxy_bypass_registry_override(std::string_view host, std::string_view override_list) {
    const auto name = normcase(host_without_port(host));
    for (auto test : split(override_list, ';')) {
        test = strip(test);
        if (test == "<local>") {
            if (host_without_port(host).find('.') == std::string_view::npos) {
                return true;
            }
        } else if (glob_match(name, normcase(test))) {
            return true;
        }
    }
    return false;
}

std::optional<std::string> proxy_for_url(std::string_view url, const ProxySettings& settings) {
    const auto target = split_url(url);
    if (!target) {
        return std::nullopt;
    }
    const bool from_environment = !settings.environment.empty();
    const ProxyMap& proxies = from_environment ? settings.environment : settings.registry;
    std::optional<std::string> proxy;
    for (const auto& [scheme, address] : proxies) {
        if (lower(scheme) == target->scheme) {
            proxy = address;
        }
    }
    if (!proxy) {
        return std::nullopt;
    }
    if (!target->host.empty()) {
        const bool bypass =
            from_environment
                ? proxy_bypass_environment(target->host, settings.environment)
                : !settings.registry_override.empty() &&
                      proxy_bypass_registry_override(target->host, settings.registry_override);
        if (bypass) {
            return std::nullopt;
        }
    }
    return proxy;
}

ProxySettings read_system_proxy_settings() {
    ProxySettings settings;
    const auto entries = detail::environment_entries();
    const bool cgi = std::any_of(entries.begin(), entries.end(),
                                 [](const auto& entry) { return entry.first == "REQUEST_METHOD"; });
    settings.environment = proxies_from_environment(entries, cgi);
    if (const auto registry = detail::read_registry_proxy(); registry && registry->enabled) {
        if (registry->server) {
            settings.registry = proxies_from_registry(*registry->server);
        }
        if (registry->override_list) {
            settings.registry_override = *registry->override_list;
        }
    }
    return settings;
}

} // namespace fv::http
