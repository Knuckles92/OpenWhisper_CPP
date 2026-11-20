#pragma once

#include <string>
#include <string_view>

// Light Markdown for formatted snippets (services/snippets.py): bold, italic, links (http,
// https, mailto, tel; "www." becomes https://), bare web addresses, line breaks and bullet or
// numbered lists. Every function is pure and thread-safe.
namespace fv::text {

// Readable plain text: inline markers dropped, list markers kept, links as "label (url)" (the
// bare url when the label is the url); an unsafe link stays literal.
std::string markdown_to_plain(std::string_view text);

// An HTML fragment: text escaped (body text without quotes, attribute values with them), lines
// joined by <br>, lists as <ul> or <ol start="N"> when the first number is not 1. CR and CRLF
// count as line ends.
std::string markdown_to_html(std::string_view text);

// Python's _escape_lines: each line HTML-escaped without quotes, joined by <br>.
std::string escape_lines(std::string_view text);

} // namespace fv::text
