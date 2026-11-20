#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace fv::py {

// email.utils.parsedate_to_datetime of Python 3.12 (_parsedate_tz, then datetime and timezone),
// as seconds since 1970: a date without a usable zone ("-0000") counts as UTC. nullopt wherever
// Python raises (no date, a field datetime refuses, a zone of a day or more). Read for
// Retry-After by automation's webhooks and the OpenAI SDK's retry wait.
std::optional<std::int64_t> parse_http_date(std::string_view text);

} // namespace fv::py
