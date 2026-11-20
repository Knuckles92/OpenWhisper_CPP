#pragma once

#include <optional>
#include <string>
#include <string_view>

// The parts of urllib.parse and ipaddress the update trust rules rest on, as Python 3.12.10
// behaves. Errors are py::ValueError with Python's messages.
namespace fv::update::detail {

// urllib.parse.urlsplit's result.
struct UrlParts {
    std::string scheme; // lower case
    std::string netloc;
    std::string path;
    std::string query;
    std::string fragment;

    // After the last "@", without the port, brackets removed, lower case; nullopt when empty.
    std::optional<std::string> hostname() const;
    std::optional<std::string> username() const;
    std::optional<std::string> password() const;
    // SplitResult.port: nullopt without one; throws py::ValueError when it is not ASCII digits
    // ("Port could not be cast to integer value as '...'") or outside 0..65535.
    std::optional<int> port() const;
};

// Throws py::ValueError("Invalid IPv6 URL") for unbalanced brackets or a bracketed host that is
// neither an IPv6 address nor an IPvFuture literal, as urlsplit does.
UrlParts urlsplit(std::string_view url);
// urlunsplit((scheme, netloc, path, query, fragment)).
std::string urlunsplit(const UrlParts& parts);

// urljoin(base, url) for the absolute and relative references a redirect's Location holds.
std::string urljoin(std::string_view base, std::string_view url);

// What urllib's redirect handler does to a Location before joining it: an empty path of a URL
// with a host becomes "/", and quote(..., safe=string.punctuation) escapes every byte that is
// not ASCII printable.
std::string normalize_redirect_location(std::string_view location);

// ipaddress.ip_address(text).is_loopback; nullopt when text is no IPv4 or IPv6 address (with an
// optional IPv6 "%zone"). An IPv4-mapped IPv6 address is loopback when its IPv4 address is.
std::optional<bool> ip_is_loopback(std::string_view text);

} // namespace fv::update::detail
