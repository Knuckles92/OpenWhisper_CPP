#include "speech/detail/url.h"

#include <algorithm>
#include <array>
#include <stdexcept>

#include "pyport/text.h"

namespace fv::speech::detail {

namespace {

bool is_ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool is_scheme_char(char c) {
    return is_ascii_alpha(c) || (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
}

char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

// str.rpartition('@')[2] and [0].
std::string_view after_at(std::string_view netloc) {
    const auto at = netloc.rfind('@');
    return at == std::string_view::npos ? netloc : netloc.substr(at + 1);
}

void check_bracketed_netloc(std::string_view netloc) {
    const auto host_and_port = after_at(netloc);
    const auto open = host_and_port.find('[');
    if (open == std::string_view::npos) {
        return;
    }
    if (open > 0) {
        throw std::invalid_argument("Invalid IPv6 URL");
    }
    const auto bracketed = host_and_port.substr(open + 1);
    const auto close = bracketed.find(']');
    const auto port =
        close == std::string_view::npos ? std::string_view() : bracketed.substr(close + 1);
    if (!port.empty() && !port.starts_with(':')) {
        throw std::invalid_argument("Invalid IPv6 URL");
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
            throw std::invalid_argument("Invalid IPv6 URL");
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
    std::string url = parts.path;
    static constexpr std::array<std::string_view, 3> kUsesNetloc{"http", "https", ""};
    const bool uses_netloc =
        std::ranges::find(kUsesNetloc, std::string_view(parts.scheme)) != kUsesNetloc.end();
    if (!parts.netloc.empty() || (!parts.scheme.empty() && uses_netloc && !url.starts_with("//"))) {
        if (!url.empty() && !url.starts_with('/')) {
            url = "/" + url;
        }
        url = "//" + parts.netloc + url;
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

} // namespace fv::speech::detail
