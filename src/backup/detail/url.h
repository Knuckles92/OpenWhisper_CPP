#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

// urllib.parse.urlsplit and urlunsplit exactly as Python 3.12.10 runs them, for _scrub.
namespace fv::backup::detail {

struct UrlParts {
    std::string scheme; // lower case
    std::string netloc;
    std::string path;
    std::string query;
    std::string fragment;
};

// The ValueError urlsplit raises: a netloc with one bracket and not the other, a bracketed host
// that is not an IPv6 address or a valid IPvFuture, or a non-ASCII netloc that NFKC
// normalization turns into one holding "/", "?", "#", "@" or ":".
class UrlError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

UrlParts urlsplit(std::string_view url);
std::string urlunsplit(const UrlParts& parts);

// ipaddress.IPv6Address(text) accepts text (an optional "%scope" included).
bool is_ipv6_address(std::string_view text);

} // namespace fv::backup::detail
