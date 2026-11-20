#pragma once

#include <optional>
#include <string>
#include <string_view>

// urllib.parse.urlsplit of Python 3.12.10, as far as the automation modules read it.
namespace fv::automation::detail {

struct UrlParts {
    std::string scheme; // lower case
    std::string netloc;
    std::string path;
    std::string query;
    std::string fragment;

    // After the last "@", without the port, brackets removed, lower case except an IPv6 zone
    // after "%"; nullopt when empty.
    std::optional<std::string> hostname() const;
};

// Throws py::ValueError where urlsplit raises: an unmatched bracket ("Invalid IPv6 URL"), text
// before or after a bracketed host, or a bracketed host that is not an IPv6 address or a valid
// IPvFuture, or a non-ASCII netloc that NFKC normalization gives a "/", "?", "#", "@" or ":"
// (_checknetloc).
UrlParts urlsplit(std::string_view url);

} // namespace fv::automation::detail
