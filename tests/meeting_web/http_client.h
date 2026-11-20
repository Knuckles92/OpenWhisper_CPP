#pragma once

// A blocking HTTP/1.1 client for the dashboard tests: one request per connection to 127.0.0.1
// ("Connection: close"), the whole answer read back.

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/json.h"

namespace fv::meeting_web::test_support {

struct HttpResult {
    int status = 0;
    std::vector<std::pair<std::string, std::string>> headers; // names in lower case, in order
    std::string body;

    std::optional<std::string> header(std::string_view name) const;
    Json json() const; // py::loads(body)
};

// Percent-encodes everything but RFC 3986's unreserved characters.
std::string percent_encode(std::string_view text);
// "a=1&b=2" from (name, value) pairs, each percent-encoded.
std::string query_string(const std::vector<std::pair<std::string, std::string>>& query);

// Sends the request (the target is path plus query, already encoded) and reads the answer;
// fails the test when the connection or the answer is broken.
HttpResult http_request(std::uint16_t port, std::string_view method, std::string_view target,
                        const std::vector<std::pair<std::string, std::string>>& headers = {},
                        std::string_view body = {});

} // namespace fv::meeting_web::test_support
