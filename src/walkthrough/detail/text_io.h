#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// Text the way Python's pathlib reads and writes it for walkthrough files.
namespace fv::walkthrough::detail {

// Python's strict "utf-8" codec: no overlong forms, no surrogates, nothing above U+10FFFF.
// fv::py::ValueError with UnicodeDecodeError's message otherwise.
void check_utf8(std::string_view text);

// Text mode's universal newlines: "\r\n" and "\r" read as "\n".
std::string universal_newlines(std::string_view text);

// The file's bytes. std::filesystem::filesystem_error when it cannot be opened or read.
std::string read_bytes(const std::filesystem::path& path);

// Path.read_text(encoding="utf-8"): strict UTF-8, universal newlines.
std::string read_text(const std::filesystem::path& path);

// Path.write_text(text, encoding="utf-8", newline="\n"): the bytes as given. Throws
// std::filesystem::filesystem_error.
void write_text(const std::filesystem::path& path, std::string_view text);

// str.splitlines(): every line boundary Python knows ("\n", "\r", "\r\n", "\v", "\f", "\x1c",
// "\x1d", "\x1e", U+0085, U+2028, U+2029), without them; no empty last line.
std::vector<std::string> splitlines(std::string_view text);

// str.strip(chars) for ASCII chars.
std::string strip_chars(std::string_view text, std::string_view chars);

} // namespace fv::walkthrough::detail
