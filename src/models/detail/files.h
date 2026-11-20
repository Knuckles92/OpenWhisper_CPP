#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/json.h"

namespace fv::models::detail {

// shutil.rmtree(path, ignore_errors=True) when `path` is a directory: everything that can be
// removed is, links are removed rather than followed, and nothing throws.
void remove_tree(const std::filesystem::path& path);

// open(path, "w", encoding="utf-8").write(text): text mode, so "\r\n" line ends on Windows.
// Throws std::filesystem::filesystem_error.
void write_text(const std::filesystem::path& path, std::string_view text);

// The file's bytes, or nullopt when it cannot be read.
std::optional<std::string> read_bytes(const std::filesystem::path& path);

// urllib.parse.urlsplit(url).hostname: the host, lowercased, without user, port or brackets;
// nullopt when there is none.
std::optional<std::string> url_hostname(std::string_view url);

// The lowercase hex SHA-256 of a file, read in 1 MiB blocks with `cancel` checked before each
// (ComponentCanceled): a model of gigabytes takes seconds, and Quit or a closing window must not
// leave the io executor hashing on. std::system_error when it cannot be read.
std::string sha256_cancellable(const std::filesystem::path& file, const CancelToken& cancel);

// std::filesystem::copy_file(from, to, overwrite_existing) in 4 MiB blocks with `cancel` checked
// before each: ComponentCanceled, with what was written of `to` removed. std::system_error when
// either file cannot be opened, read or written.
void copy_file_cancellable(const std::filesystem::path& from, const std::filesystem::path& to,
                           const CancelToken& cancel);

// Python's truthiness of a JSON value (None, False, 0, "", [] and {} are false).
bool truthy(const Json& value);

// int(value) for the sizes a catalog holds: integers as they are, floats truncated, bools as
// 0 or 1, digit strings parsed, null 0. Anything else is std::invalid_argument (Python's
// ValueError or TypeError).
std::int64_t to_int(const Json& value);

// The member of an object, or nullptr (also when `object` is not an object).
const Json* member(const Json& object, std::string_view key);

// str(value) as an f-string writes a JSON value.
std::string py_str(const Json& value);

} // namespace fv::models::detail
