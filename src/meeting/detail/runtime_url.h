#pragma once

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// urllib.parse as Python 3.12.10 runs it, for the runtime's dashboard links (redact_meeting_url,
// _with_history_target).
namespace fv::meeting::detail {

struct UrlParts {
    std::string scheme; // lower case
    std::string netloc;
    std::string path;
    std::string query;
    std::string fragment;
};

// The ValueError urlsplit raises.
class UrlError : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};

UrlParts urlsplit(std::string_view url);
std::string urlunsplit(const UrlParts& parts);

// ipaddress.IPv6Address(text) accepts text (an optional "%scope" included).
bool is_ipv6_address(std::string_view text);

// parse_qsl(query, keep_blank_values=True): "&"-separated name=value pairs, "+" read as a space
// and %XX decoded as UTF-8 (invalid sequences as U+FFFD).
std::vector<std::pair<std::string, std::string>> parse_qsl(std::string_view query);
// urlencode(pairs): quote_plus of each name and value, joined with "&".
std::string urlencode(const std::vector<std::pair<std::string, std::string>>& pairs);

} // namespace fv::meeting::detail
