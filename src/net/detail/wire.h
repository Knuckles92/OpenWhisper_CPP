#pragma once

#include <chrono>
#include <string>
#include <string_view>

#include "net/types.h"

// What uvicorn and Starlette do to the bytes of a request and a response. No Boost here.
namespace fv::net::detail {

// urllib.parse.unquote(text, errors="replace") of ASCII text: %XX sequences become bytes, the
// result is decoded as UTF-8 with each invalid sequence replaced by U+FFFD; with plus_as_space
// "+" is a space first (parse_qsl).
std::string unquote(std::string_view text, bool plus_as_space = false);

// parse_qsl(query, keep_blank_values=True), as Starlette's request.query_params.
Query parse_query(std::string_view query);

// urllib.parse.quote(text, safe): UTF-8 bytes outside letters, digits, "_.-~" and `safe` as %XX.
std::string quote(std::string_view text, std::string_view safe);

// http.HTTPStatus(status).phrase of Python 3.12, or "" for a status it does not name.
std::string_view status_phrase(int status);

// The Date header as uvicorn writes it: "Sat, 15 Nov 2026 19:00:00 GMT".
std::string http_date(std::chrono::system_clock::time_point when);

std::string ascii_lower(std::string_view text);
bool iequals(std::string_view a, std::string_view b);

} // namespace fv::net::detail
