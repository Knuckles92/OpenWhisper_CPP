#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace fv::llm::detail {

// urllib.parse.urlsplit's parts, as Python 3.12 splits them.
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
};

// Throws std::invalid_argument("Invalid IPv6 URL") for a netloc with one bracket and not the
// other, as urlsplit raises ValueError.
UrlParts urlsplit(std::string_view url);
// urlunsplit((scheme, netloc, path, query, fragment)).
std::string urlunsplit(const UrlParts& parts);

} // namespace fv::llm::detail
