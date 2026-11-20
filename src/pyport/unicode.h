#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace fv::py {

// The Unicode version of Python 3.12's unicodedata, which every table here reproduces.
inline constexpr std::string_view unidata_version = "15.0.0";

// UTF-8 is decoded leniently: a surrogate encoded in three bytes (as json.loads produces for a
// lone "\ud83d") decodes to that surrogate, so text read from JSON round-trips; any other
// invalid byte decodes to U+FFFD on its own.
char32_t next_code_point(std::string_view utf8, std::size_t& byte_pos);
std::u32string to_code_points(std::string_view utf8);
void append_utf8(std::string& out, char32_t cp);
std::string to_utf8(std::u32string_view code_points);

// Per code point, as one-character str methods and unicodedata of Python 3.12 answer.
std::string_view general_category(char32_t cp); // unicodedata.category, "Lu", "Cn", ...
std::optional<int> decimal_value(char32_t cp);  // unicodedata.decimal
bool is_alpha(char32_t cp);
bool is_decimal(char32_t cp);
bool is_digit(char32_t cp);
bool is_numeric(char32_t cp);
bool is_alnum(char32_t cp);
bool is_space(char32_t cp);
bool is_lower(char32_t cp); // the Lowercase property
bool is_upper(char32_t cp); // the Uppercase property
bool is_title(char32_t cp); // category Lt
bool is_cased(char32_t cp);
bool is_case_ignorable(char32_t cp);
bool is_printable(char32_t cp);
bool is_xid_start(char32_t cp);
bool is_xid_continue(char32_t cp);
bool is_word(char32_t cp); // re's Unicode \w: is_alnum or '_'

} // namespace fv::py
