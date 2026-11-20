#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace fv::py {

// Python 3.12 str semantics on UTF-8 text, counted and indexed in code points (never bytes).
// Case mappings are the full ones of Python's Unicode 15.0 tables, with the final-sigma rule
// of lower(), title() and capitalize().

std::size_t len(std::string_view utf8);
// s[start:stop], negative indices counting from the end, out-of-range indices clamped.
std::string slice(std::string_view utf8, std::optional<std::int64_t> start,
                  std::optional<std::int64_t> stop = std::nullopt);

std::string casefold(std::string_view utf8); // full folding: "straße" -> "strasse"
std::string lower(std::string_view utf8);
std::string upper(std::string_view utf8);
std::string title(std::string_view utf8);
std::string capitalize(std::string_view utf8); // first code point titlecased, the rest lower

// strip() removes Unicode whitespace (str.isspace); the chars overloads remove any code point
// of chars, as str.strip(chars).
std::string strip(std::string_view utf8);
std::string lstrip(std::string_view utf8);
std::string rstrip(std::string_view utf8);
std::string strip(std::string_view utf8, std::string_view chars);
std::string lstrip(std::string_view utf8, std::string_view chars);
std::string rstrip(std::string_view utf8, std::string_view chars);

// str.split(): runs of whitespace separate, no empty items; maxsplit < 0 means no limit.
std::vector<std::string> split(std::string_view utf8, std::int64_t maxsplit = -1);
// str.split(sep): every occurrence separates, empty items kept. Throws ValueError("empty
// separator") for an empty sep, as Python does.
std::vector<std::string> split_on(std::string_view utf8, std::string_view sep,
                                  std::int64_t maxsplit = -1);

// The str.isX predicates: False for "" except isprintable, which is True.
bool isspace(std::string_view utf8);
bool isalnum(std::string_view utf8);
bool isalpha(std::string_view utf8);
bool isdigit(std::string_view utf8);
bool isdecimal(std::string_view utf8);
bool isnumeric(std::string_view utf8);
bool isupper(std::string_view utf8);
bool islower(std::string_view utf8);
bool istitle(std::string_view utf8);
bool isprintable(std::string_view utf8);
bool isidentifier(std::string_view utf8);

// html.escape(s, quote): '&', '<', '>' and, with quote, '"' as "&quot;" and '\'' as "&#x27;".
std::string html_escape(std::string_view utf8, bool quote = true);

// repr(s) of a str: quotes chosen as Python does, non-printable code points escaped.
std::string repr(std::string_view utf8);

} // namespace fv::py
