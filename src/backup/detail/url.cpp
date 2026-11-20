#include "backup/detail/url.h"

#include <algorithm>
#include <array>
#include <optional>
#include <vector>

#include "pyport/unicode.h"

namespace fv::backup::detail {

namespace {

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex(char c) {
    return is_ascii_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || is_ascii_digit(c) || c == '+' || c == '-' || c == '.';
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (true) {
        const std::size_t at = text.find(separator, start);
        parts.push_back(text.substr(start, at - start));
        if (at == std::string_view::npos) {
            return parts;
        }
        start = at + 1;
    }
}

// IPv4Address(text): four decimal octets, no leading zeros, each at most 255.
bool is_ipv4_address(std::string_view text) {
    if (text.empty() || text.find('/') != std::string_view::npos) {
        return false;
    }
    const auto octets = split(text, '.');
    if (octets.size() != 4) {
        return false;
    }
    for (const auto octet : octets) {
        if (octet.empty() || octet.size() > 3 || !std::ranges::all_of(octet, is_ascii_digit)) {
            return false;
        }
        if (octet.size() > 1 && octet[0] == '0') {
            return false;
        }
        int value = 0;
        for (const char c : octet) {
            value = value * 10 + (c - '0');
        }
        if (value > 255) {
            return false;
        }
    }
    return true;
}

bool is_hextet(std::string_view text) {
    return !text.empty() && text.size() <= 4 && std::ranges::all_of(text, is_hex);
}

// Code points whose NFKC form holds "/", "?", "#", "@" or ":" (Unicode 15.0, computed with
// Python 3.12's unicodedata): what _checknetloc refuses in a non-ASCII netloc.
constexpr std::array<char32_t, 19> kNfkcDelimiters = {
    0x2047, 0x2048, 0x2049, 0x2100, 0x2101, 0x2105, 0x2106, 0x2A74, 0xFE13, 0xFE16,
    0xFE55, 0xFE56, 0xFE5F, 0xFE6B, 0xFF03, 0xFF0F, 0xFF1A, 0xFF1F, 0xFF20,
};

void check_netloc(std::string_view netloc) {
    if (std::ranges::all_of(netloc,
                            [](char c) { return (static_cast<unsigned char>(c) & 0x80) == 0; })) {
        return;
    }
    std::size_t at = 0;
    while (at < netloc.size()) {
        const char32_t cp = py::next_code_point(netloc, at);
        if (std::ranges::find(kNfkcDelimiters, cp) != kNfkcDelimiters.end()) {
            throw UrlError("netloc '" + std::string(netloc) +
                           "' contains invalid characters under NFKC normalization");
        }
    }
}

// re.match(r"\Av[a-fA-F0-9]+\..+\Z", host).
bool is_ipvfuture(std::string_view host) {
    if (host.size() < 4 || host[0] != 'v') {
        return false;
    }
    std::size_t i = 1;
    while (i < host.size() && is_hex(host[i])) {
        ++i;
    }
    return i >= 2 && i < host.size() && host[i] == '.' && i + 1 < host.size() &&
           host.find('\n', i + 1) == std::string_view::npos;
}

void check_bracketed_host(std::string_view host) {
    if (host.starts_with('v')) {
        if (!is_ipvfuture(host)) {
            throw UrlError("IPvFuture address is invalid");
        }
        return;
    }
    if (is_ipv4_address(host)) {
        throw UrlError("An IPv4 address cannot be in brackets");
    }
    if (!is_ipv6_address(host)) {
        throw UrlError("'" + std::string(host) + "' does not appear to be an IPv4 or IPv6 address");
    }
}

void check_bracketed_netloc(std::string_view netloc) {
    const auto at = netloc.rfind('@');
    const std::string_view host_and_port =
        at == std::string_view::npos ? netloc : netloc.substr(at + 1);
    const auto open = host_and_port.find('[');
    std::string_view host;
    if (open != std::string_view::npos) {
        if (open > 0) {
            throw UrlError("Invalid IPv6 URL");
        }
        const std::string_view bracketed = host_and_port.substr(open + 1);
        const auto close = bracketed.find(']');
        host = bracketed.substr(0, close);
        const std::string_view port =
            close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
        if (!port.empty() && !port.starts_with(':')) {
            throw UrlError("Invalid IPv6 URL");
        }
    } else {
        host = host_and_port.substr(0, host_and_port.find(':'));
    }
    check_bracketed_host(host);
}

} // namespace

bool is_ipv6_address(std::string_view text) {
    if (text.find('/') != std::string_view::npos) {
        return false;
    }
    std::string_view address = text;
    if (const auto percent = text.find('%'); percent != std::string_view::npos) {
        const std::string_view scope = text.substr(percent + 1);
        if (scope.empty() || scope.find('%') != std::string_view::npos) {
            return false;
        }
        address = text.substr(0, percent);
    }
    if (address.empty()) {
        return false;
    }
    std::vector<std::string_view> parts = split(address, ':');
    if (parts.size() < 3) {
        return false;
    }
    if (parts.back().find('.') != std::string_view::npos) {
        if (!is_ipv4_address(parts.back())) {
            return false;
        }
        parts.back() = "0";
        parts.push_back("0");
    }
    constexpr std::size_t kHextets = 8;
    if (parts.size() > kHextets + 1) {
        return false;
    }
    std::optional<std::size_t> skip;
    for (std::size_t i = 1; i + 1 < parts.size(); ++i) {
        if (parts[i].empty()) {
            if (skip) {
                return false;
            }
            skip = i;
        }
    }
    std::size_t hi = 0;
    std::size_t lo = 0;
    if (skip) {
        hi = *skip;
        lo = parts.size() - *skip - 1;
        if (parts.front().empty()) {
            if (--hi != 0) {
                return false;
            }
        }
        if (parts.back().empty()) {
            if (--lo != 0) {
                return false;
            }
        }
        if (hi + lo >= kHextets) {
            return false;
        }
    } else {
        if (parts.size() != kHextets || parts.front().empty() || parts.back().empty()) {
            return false;
        }
        hi = parts.size();
    }
    for (std::size_t i = 0; i < hi; ++i) {
        if (!is_hextet(parts[i])) {
            return false;
        }
    }
    for (std::size_t i = parts.size() - lo; i < parts.size(); ++i) {
        if (!is_hextet(parts[i])) {
            return false;
        }
    }
    return true;
}

UrlParts urlsplit(std::string_view input) {
    // _WHATWG_C0_CONTROL_OR_SPACE stripped from the front; tab, CR and LF removed anywhere.
    while (!input.empty() && static_cast<unsigned char>(input.front()) <= 0x20) {
        input.remove_prefix(1);
    }
    std::string url;
    url.reserve(input.size());
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
        for (char& c : parts.scheme) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        url.erase(0, colon + 1);
    }
    if (url.starts_with("//")) {
        auto end = url.find_first_of("/?#", 2);
        if (end == std::string::npos) {
            end = url.size();
        }
        parts.netloc = url.substr(2, end - 2);
        url.erase(0, end);
        const bool open = parts.netloc.find('[') != std::string::npos;
        const bool close = parts.netloc.find(']') != std::string::npos;
        if (open != close) {
            throw UrlError("Invalid IPv6 URL");
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
    check_netloc(parts.netloc);
    parts.path = std::move(url);
    return parts;
}

std::string urlunsplit(const UrlParts& parts) {
    // uses_netloc, of which _scrub only ever rebuilds http and https.
    static constexpr std::array<std::string_view, 27> kUsesNetloc = {
        "",     "ftp",   "http",    "gopher", "nntp",  "telnet",       "imap",
        "wais", "file",  "mms",     "https",  "shttp", "snews",        "prospero",
        "rtsp", "rtsps", "rtspu",   "rsync",  "svn",   "svn+ssh",      "sftp",
        "nfs",  "git",   "git+ssh", "ws",     "wss",   "itms-services"};
    std::string url = parts.path;
    if (!parts.netloc.empty()) {
        if (!url.empty() && !url.starts_with('/')) {
            url = "/" + url;
        }
        url = "//" + parts.netloc + url;
    } else if (url.starts_with("//")) {
        url = "//" + url;
    } else if (!parts.scheme.empty() &&
               std::ranges::find(kUsesNetloc, std::string_view(parts.scheme)) !=
                   kUsesNetloc.end() &&
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

} // namespace fv::backup::detail
