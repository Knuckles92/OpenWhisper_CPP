#include "automation/detail/url.h"

#include <algorithm>
#include <array>

#include "net/address.h"
#include "pyport/errors.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::automation::detail {

namespace {

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string_view after_at(std::string_view netloc) {
    const auto at = netloc.rfind('@');
    return at == std::string_view::npos ? netloc : netloc.substr(at + 1);
}

// _check_bracketed_host.
void check_bracketed_host(std::string_view host) {
    if (host.starts_with('v')) {
        // \Av[a-fA-F0-9]+\..+\Z
        std::size_t i = 1;
        while (i < host.size() && is_hex(host[i])) {
            ++i;
        }
        const bool valid = i > 1 && i < host.size() && host[i] == '.' && i + 1 < host.size() &&
                           host.find('\n', i + 1) == std::string_view::npos;
        if (!valid) {
            throw py::ValueError("IPvFuture address is invalid");
        }
        return;
    }
    const auto address = net::parse_address(host);
    if (!address) {
        throw py::ValueError("does not appear to be an IPv4 or IPv6 address");
    }
    if (address->version != 6) {
        throw py::ValueError("An IPv4 address cannot be in brackets");
    }
}

// _check_bracketed_netloc.
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

// Code points whose NFKC form holds "/", "?", "#", "@" or ":" (Unicode 15.0, computed with
// Python 3.12.10's unicodedata). Composition never yields ASCII punctuation, so a netloc gains a
// separator under NFKC exactly when it holds one of these.
constexpr std::array<char32_t, 19> kNfkcSeparators = {
    0x2047, 0x2048, 0x2049, 0x2100, 0x2101, 0x2105, 0x2106, 0x2A74, 0xFE13, 0xFE16,
    0xFE55, 0xFE56, 0xFE5F, 0xFE6B, 0xFF03, 0xFF0F, 0xFF1A, 0xFF1F, 0xFF20,
};

// _checknetloc: a non-ASCII netloc whose NFKC form gains a separator is refused.
void check_netloc(std::string_view netloc) {
    if (std::ranges::all_of(netloc,
                            [](char c) { return (static_cast<unsigned char>(c) & 0x80) == 0; })) {
        return;
    }
    std::size_t at = 0;
    while (at < netloc.size()) {
        if (std::ranges::find(kNfkcSeparators, py::next_code_point(netloc, at)) !=
            kNfkcSeparators.end()) {
            throw py::ValueError("netloc '" + std::string(netloc) +
                                 "' contains invalid characters under NFKC normalization");
        }
    }
}

} // namespace

std::optional<std::string> UrlParts::hostname() const {
    const auto info = after_at(netloc);
    std::string_view host;
    if (const auto open = info.find('['); open != std::string_view::npos) {
        const auto bracketed = info.substr(open + 1);
        host = bracketed.substr(0, bracketed.find(']'));
    } else {
        host = info.substr(0, info.find(':'));
    }
    if (host.empty()) {
        return std::nullopt;
    }
    const auto percent = host.find('%');
    return py::lower(host.substr(0, percent)) + std::string(percent == std::string_view::npos
                                                                ? std::string_view()
                                                                : host.substr(percent));
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
    check_netloc(parts.netloc);
    parts.path = std::move(url);
    return parts;
}

} // namespace fv::automation::detail
