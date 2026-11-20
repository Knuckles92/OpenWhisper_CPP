#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include "pyport/email_date.h"

namespace fv::automation::detail {

// py::parse_http_date, where Retry-After is read: a date without a usable zone counts as UTC, as
// _retry_after reads it.
inline std::optional<std::int64_t> parse_http_date(std::string_view text) {
    return py::parse_http_date(text);
}

} // namespace fv::automation::detail
