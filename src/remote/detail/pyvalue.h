#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "base/json.h"

// Python's view of decoded JSON values and of bytes, as the remote engine's modules read them.
namespace fv::remote::detail {

// bool(value): null, false, 0, 0.0, "", [] and {} are false; NaN is true.
bool truthy(const Json& value);

// str(value): a string as it is, None/True/False, an int in decimal, a float as repr, and a list
// or dict as Python's repr of it.
std::string py_str(const Json& value);

// str(value or ""): "" for anything falsy.
std::string py_str_or_empty(const Json& value);

// isinstance(value, int) and not isinstance(value, bool): a JSON integer.
bool is_int(const Json& value);

// bytes.decode("utf-8") (strict; `surrogatepass` also takes a surrogate's three-byte form, as
// json.loads of bytes does). nullopt where Python raises UnicodeDecodeError.
std::optional<std::string> decode_utf8(std::string_view bytes, bool surrogatepass = false);

// bytes.decode("utf-8", "replace"), each maximal invalid sequence as U+FFFD.
std::string decode_utf8_replace(std::string_view bytes);

// json.loads(bytes)'s decoding: json.detect_encoding (UTF-8 with or without its BOM, UTF-16 and
// UTF-32 by their BOM or by the zero bytes of the first four), then a decode with
// "surrogatepass". The text as UTF-8 (a lone surrogate in its three-byte form), or nullopt where
// Python raises UnicodeDecodeError.
std::optional<std::string> json_bytes_to_text(std::span<const std::byte> data);

// bytes as std::string and back.
std::string_view as_chars(std::span<const std::byte> bytes);
std::span<const std::byte> as_bytes(std::string_view text);

} // namespace fv::remote::detail
