#include "update/detail/url.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>

#include "pyport/errors.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::update::detail {

namespace {

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' || c == '.';
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

bool is_hex(char c) {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

// str.rpartition('@')[2].
std::string_view after_at(std::string_view netloc) {
    const auto at = netloc.rfind('@');
    return at == std::string_view::npos ? netloc : netloc.substr(at + 1);
}

std::vector<std::string_view> split_view(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const auto found = text.find(separator, start);
        if (found == std::string_view::npos) {
            parts.push_back(text.substr(start));
            return parts;
        }
        parts.push_back(text.substr(start, found - start));
        start = found + 1;
    }
}

// IPv4Address(text): four dotted decimal octets, no leading zeros, each at most 255.
std::optional<std::uint32_t> parse_ipv4(std::string_view text) {
    if (text.empty() || text.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto octets = split_view(text, '.');
    if (octets.size() != 4) {
        return std::nullopt;
    }
    std::uint32_t value = 0;
    for (const auto octet : octets) {
        if (octet.empty() || octet.size() > 3 || !std::ranges::all_of(octet, is_ascii_digit)) {
            return std::nullopt;
        }
        if (octet.size() > 1 && octet.front() == '0') {
            return std::nullopt;
        }
        std::uint32_t number = 0;
        for (const char c : octet) {
            number = number * 10 + static_cast<std::uint32_t>(c - '0');
        }
        if (number > 255) {
            return std::nullopt;
        }
        value = (value << 8) | number;
    }
    return value;
}

using Hextets = std::array<std::uint16_t, 8>;

// IPv6Address(text): IPv6Address._ip_int_from_string after the "%zone" is split off.
std::optional<Hextets> parse_ipv6(std::string_view text) {
    if (text.find('/') != std::string_view::npos) {
        return std::nullopt;
    }
    const auto percent = text.find('%');
    if (percent != std::string_view::npos) {
        const auto zone = text.substr(percent + 1);
        if (zone.empty() || zone.find('%') != std::string_view::npos) {
            return std::nullopt;
        }
        text = text.substr(0, percent);
    }
    if (text.empty()) {
        return std::nullopt;
    }
    std::vector<std::string> parts;
    for (const auto part : split_view(text, ':')) {
        parts.emplace_back(part);
    }
    if (parts.size() < 3) {
        return std::nullopt;
    }
    if (parts.back().find('.') != std::string::npos) {
        const auto ipv4 = parse_ipv4(parts.back());
        if (!ipv4) {
            return std::nullopt;
        }
        parts.pop_back();
        static constexpr char kDigits[] = "0123456789abcdef";
        const auto hex = [](std::uint32_t value) {
            std::string out;
            do {
                out.insert(out.begin(), kDigits[value & 0xF]);
                value >>= 4;
            } while (value != 0);
            return out;
        };
        parts.push_back(hex((*ipv4 >> 16) & 0xFFFF));
        parts.push_back(hex(*ipv4 & 0xFFFF));
    }
    if (parts.size() > 9) {
        return std::nullopt;
    }
    std::optional<std::size_t> skip_index;
    for (std::size_t i = 1; i + 1 < parts.size(); ++i) {
        if (parts[i].empty()) {
            if (skip_index) {
                return std::nullopt;
            }
            skip_index = i;
        }
    }
    std::size_t parts_hi = 0;
    std::size_t parts_lo = 0;
    std::size_t parts_skipped = 0;
    if (skip_index) {
        parts_hi = *skip_index;
        parts_lo = parts.size() - *skip_index - 1;
        if (parts.front().empty()) {
            if (--parts_hi != 0) {
                return std::nullopt;
            }
        }
        if (parts.back().empty()) {
            if (--parts_lo != 0) {
                return std::nullopt;
            }
        }
        if (parts_hi + parts_lo >= 8) {
            return std::nullopt;
        }
        parts_skipped = 8 - (parts_hi + parts_lo);
    } else {
        if (parts.size() != 8 || parts.front().empty() || parts.back().empty()) {
            return std::nullopt;
        }
        parts_hi = parts.size();
    }
    const auto hextet = [](const std::string& part) -> std::optional<std::uint16_t> {
        if (part.empty() || part.size() > 4 || !std::ranges::all_of(part, is_hex)) {
            return std::nullopt;
        }
        return static_cast<std::uint16_t>(std::stoul(part, nullptr, 16));
    };
    Hextets value{};
    std::size_t out = 0;
    for (std::size_t i = 0; i < parts_hi; ++i) {
        const auto parsed = hextet(parts[i]);
        if (!parsed) {
            return std::nullopt;
        }
        value[out++] = *parsed;
    }
    out += parts_skipped;
    for (std::size_t i = parts.size() - parts_lo; i < parts.size(); ++i) {
        const auto parsed = hextet(parts[i]);
        if (!parsed) {
            return std::nullopt;
        }
        value[out++] = *parsed;
    }
    return value;
}

// _check_bracketed_host: an IPvFuture literal or an IPv6 address, never IPv4.
void check_bracketed_host(std::string_view host) {
    static const py::Regex kFuture(R"(\Av[a-fA-F0-9]+\..+\Z)");
    if (host.starts_with('v')) {
        if (!kFuture.match(host)) {
            throw py::ValueError("IPvFuture address is invalid");
        }
        return;
    }
    if (parse_ipv4(host)) {
        throw py::ValueError("An IPv4 address cannot be in brackets");
    }
    if (!parse_ipv6(host)) {
        throw py::ValueError(py::repr(host) + " does not appear to be an IPv4 or IPv6 address");
    }
}

void check_bracketed_netloc(std::string_view netloc) {
    const auto host_and_port = after_at(netloc);
    const auto open = host_and_port.find('[');
    std::string_view host;
    if (open != std::string_view::npos) {
        if (open > 0) {
            throw py::ValueError("Invalid IPv6 URL");
        }
        const auto bracketed = host_and_port.substr(open + 1);
        const auto close = bracketed.find(']');
        host = bracketed.substr(0, close);
        const auto port =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        if (!port.empty() && !port.starts_with(':')) {
            throw py::ValueError("Invalid IPv6 URL");
        }
    } else {
        host = host_and_port.substr(0, host_and_port.find(':'));
    }
    check_bracketed_host(host);
}

// NetlocResultMixinStr._hostinfo: (hostname, port text), port nullopt when empty.
std::pair<std::string_view, std::optional<std::string_view>> hostinfo(std::string_view netloc) {
    const auto info = after_at(netloc);
    std::string_view host;
    std::string_view port;
    if (const auto open = info.find('['); open != std::string_view::npos) {
        const auto bracketed = info.substr(open + 1);
        const auto close = bracketed.find(']');
        host = bracketed.substr(0, close);
        auto rest =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        const auto colon = rest.find(':');
        port = colon == std::string_view::npos ? std::string_view() : rest.substr(colon + 1);
    } else {
        const auto colon = info.find(':');
        host = info.substr(0, colon);
        port = colon == std::string_view::npos ? std::string_view() : info.substr(colon + 1);
    }
    if (port.empty()) {
        return {host, std::nullopt};
    }
    return {host, port};
}

bool in_list(std::string_view scheme, std::initializer_list<std::string_view> list) {
    return std::ranges::find(list, scheme) != list.end();
}

bool uses_netloc(std::string_view scheme) {
    return in_list(scheme,
                   {"",     "ftp",   "http",    "gopher", "nntp",  "telnet",       "imap",
                    "wais", "file",  "mms",     "https",  "shttp", "snews",        "prospero",
                    "rtsp", "rtsps", "rtspu",   "rsync",  "svn",   "svn+ssh",      "sftp",
                    "nfs",  "git",   "git+ssh", "ws",     "wss",   "itms-services"});
}

bool uses_relative(std::string_view scheme) {
    return in_list(scheme, {"",      "ftp",   "http",  "gopher",  "nntp",     "imap", "wais",
                            "file",  "https", "shttp", "mms",     "prospero", "rtsp", "rtsps",
                            "rtspu", "sftp",  "svn",   "svn+ssh", "ws",       "wss"});
}

bool uses_params(std::string_view scheme) {
    return in_list(scheme, {"", "ftp", "hdl", "prospero", "http", "imap", "https", "shttp", "rtsp",
                            "rtsps", "rtspu", "sip", "sips", "mms", "sftp", "tel"});
}

// urlparse's _splitparams: cuts ";params" off the last path segment of `parts` and returns them.
std::string split_params(UrlParts& parts) {
    if (!uses_params(parts.scheme) || parts.path.find(';') == std::string::npos) {
        return {};
    }
    std::size_t at = 0;
    if (const auto slash = parts.path.rfind('/'); slash != std::string::npos) {
        at = parts.path.find(';', slash);
        if (at == std::string::npos) {
            return {};
        }
    } else {
        at = parts.path.find(';');
    }
    auto params = parts.path.substr(at + 1);
    parts.path.resize(at);
    return params;
}

} // namespace

std::optional<std::string> UrlParts::hostname() const {
    const auto host = hostinfo(netloc).first;
    if (host.empty()) {
        return std::nullopt;
    }
    // Only the address is lowered; an IPv6 zone after "%" keeps its case.
    const auto percent = host.find('%');
    return py::lower(host.substr(0, percent)) + std::string(percent == std::string_view::npos
                                                                ? std::string_view()
                                                                : host.substr(percent));
}

std::optional<std::string> UrlParts::username() const {
    const auto at = netloc.rfind('@');
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const std::string_view info(netloc.data(), at);
    return std::string(info.substr(0, info.find(':')));
}

std::optional<std::string> UrlParts::password() const {
    const auto at = netloc.rfind('@');
    if (at == std::string::npos) {
        return std::nullopt;
    }
    const std::string_view info(netloc.data(), at);
    const auto colon = info.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string(info.substr(colon + 1));
}

std::optional<int> UrlParts::port() const {
    const auto text = hostinfo(netloc).second;
    if (!text) {
        return std::nullopt;
    }
    if (!std::ranges::all_of(*text, is_ascii_digit)) {
        throw py::ValueError("Port could not be cast to integer value as " + py::repr(*text));
    }
    // Leading zeros are allowed ("0080" is 80); only the value's range counts.
    const auto digits = text->substr(std::min(text->find_first_not_of('0'), text->size()));
    if (digits.size() > 5) {
        throw py::ValueError("Port out of range 0-65535");
    }
    int value = 0;
    for (const char c : digits) {
        value = value * 10 + (c - '0');
    }
    if (value > 65535) {
        throw py::ValueError("Port out of range 0-65535");
    }
    return value;
}

UrlParts urlsplit(std::string_view input) {
    // _WHATWG_C0_CONTROL_OR_SPACE stripped from the front; tab, CR and LF removed anywhere.
    while (!input.empty() && static_cast<unsigned char>(input.front()) <= 0x20) {
        input.remove_prefix(1);
    }
    std::string url;
    for (const char c : input) {
        if (c != '\t' && c != '\r' && c != '\n') {
            url += c;
        }
    }
    UrlParts parts;
    const auto colon = url.find(':');
    if (colon != std::string::npos && colon > 0 && is_ascii_alpha(url[0]) &&
        std::all_of(url.begin(), url.begin() + static_cast<std::ptrdiff_t>(colon),
                    is_scheme_char)) {
        parts.scheme = url.substr(0, colon);
        std::ranges::transform(parts.scheme, parts.scheme.begin(), ascii_lower);
        url = url.substr(colon + 1);
    }
    if (url.starts_with("//")) {
        auto end = url.find_first_of("/?#", 2);
        if (end == std::string::npos) {
            end = url.size();
        }
        parts.netloc = url.substr(2, end - 2);
        url = url.substr(end);
        const bool open = parts.netloc.find('[') != std::string::npos;
        const bool close = parts.netloc.find(']') != std::string::npos;
        if (open != close) {
            throw py::ValueError("Invalid IPv6 URL");
        }
        if (open) {
            check_bracketed_netloc(parts.netloc);
        }
    }
    if (const auto hash = url.find('#'); hash != std::string::npos) {
        parts.fragment = url.substr(hash + 1);
        url.resize(hash);
    }
    if (const auto question = url.find('?'); question != std::string::npos) {
        parts.query = url.substr(question + 1);
        url.resize(question);
    }
    parts.path = std::move(url);
    return parts;
}

std::string urlunsplit(const UrlParts& parts) {
    // Python 3.12.4 and later (gh-67693): a path starting "//" keeps an empty authority.
    std::string url = parts.path;
    if (!parts.netloc.empty()) {
        if (!url.empty() && !url.starts_with('/')) {
            url = "/" + url;
        }
        url = "//" + parts.netloc + url;
    } else if (url.starts_with("//")) {
        url = "//" + url;
    } else if (!parts.scheme.empty() && uses_netloc(parts.scheme) &&
               (url.empty() || url.starts_with('/'))) {
        url = "//" + url;
    }
    if (!parts.scheme.empty()) {
        url = parts.scheme + ":" + url;
    }
    if (!parts.query.empty()) {
        url += "?" + parts.query;
    }
    if (!parts.fragment.empty()) {
        url += "#" + parts.fragment;
    }
    return url;
}

std::string urljoin(std::string_view base, std::string_view url) {
    if (base.empty()) {
        return std::string(url);
    }
    if (url.empty()) {
        return std::string(base);
    }
    // urlparse: the ";params" of the last segment travel apart from the path, and urlunparse
    // puts back only non-empty ones.
    auto b = urlsplit(base);
    const auto b_params = split_params(b);
    auto u = urlsplit(url);
    if (u.scheme.empty()) {
        u.scheme = b.scheme;
    }
    auto u_params = split_params(u);
    const auto unparse = [](UrlParts parts, const std::string& params) {
        if (!params.empty()) {
            parts.path += ";" + params;
        }
        return urlunsplit(parts);
    };
    if (u.scheme != b.scheme || !uses_relative(u.scheme)) {
        return std::string(url);
    }
    if (uses_netloc(u.scheme)) {
        if (!u.netloc.empty()) {
            return unparse(u, u_params);
        }
        u.netloc = b.netloc;
    }
    if (u.path.empty() && u_params.empty()) {
        u.path = b.path;
        u_params = b_params;
        if (u.query.empty()) {
            u.query = b.query;
        }
        return unparse(u, u_params);
    }
    std::vector<std::string> base_parts;
    for (const auto part : split_view(b.path, '/')) {
        base_parts.emplace_back(part);
    }
    if (!base_parts.back().empty()) {
        base_parts.pop_back();
    }
    std::vector<std::string> segments;
    if (u.path.starts_with('/')) {
        for (const auto part : split_view(u.path, '/')) {
            segments.emplace_back(part);
        }
    } else {
        segments = base_parts;
        for (const auto part : split_view(u.path, '/')) {
            segments.emplace_back(part);
        }
        // segments[1:-1] = filter(None, segments[1:-1])
        if (segments.size() > 2) {
            std::vector<std::string> kept{segments.front()};
            for (std::size_t i = 1; i + 1 < segments.size(); ++i) {
                if (!segments[i].empty()) {
                    kept.push_back(segments[i]);
                }
            }
            kept.push_back(segments.back());
            segments = std::move(kept);
        }
    }
    std::vector<std::string> resolved;
    for (const auto& segment : segments) {
        if (segment == "..") {
            if (!resolved.empty()) {
                resolved.pop_back();
            }
        } else if (segment != ".") {
            resolved.push_back(segment);
        }
    }
    if (segments.back() == "." || segments.back() == "..") {
        resolved.emplace_back();
    }
    std::string path;
    for (std::size_t i = 0; i < resolved.size(); ++i) {
        path += (i == 0 ? "" : "/") + resolved[i];
    }
    u.path = path.empty() ? "/" : path;
    return unparse(u, u_params);
}

std::string normalize_redirect_location(std::string_view location) {
    auto parts = urlsplit(location);
    if (parts.path.empty() && !parts.netloc.empty()) {
        parts.path = "/";
    }
    const auto joined = urlunsplit(parts);
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string quoted;
    for (const char c : joined) {
        const auto byte = static_cast<unsigned char>(c);
        if (byte > 0x20 && byte < 0x7F) {
            quoted += c;
        } else {
            quoted += '%';
            quoted += kHex[byte >> 4];
            quoted += kHex[byte & 0xF];
        }
    }
    return quoted;
}

std::optional<bool> ip_is_loopback(std::string_view text) {
    if (const auto ipv4 = parse_ipv4(text)) {
        return (*ipv4 >> 24) == 127;
    }
    const auto ipv6 = parse_ipv6(text);
    if (!ipv6) {
        return std::nullopt;
    }
    const auto& h = *ipv6;
    const bool mapped =
        h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0 && h[4] == 0 && h[5] == 0xFFFF;
    if (mapped) {
        return (h[6] >> 8) == 127;
    }
    return h[0] == 0 && h[1] == 0 && h[2] == 0 && h[3] == 0 && h[4] == 0 && h[5] == 0 &&
           h[6] == 0 && h[7] == 1;
}

} // namespace fv::update::detail
