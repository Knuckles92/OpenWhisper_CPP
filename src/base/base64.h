#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Base64 as Python's base64 module writes and reads it (RFC 4648).
namespace fv {

// base64.b64encode, or urlsafe_b64encode with '-' and '_' for '+' and '/'; pad = false drops the
// trailing '=' (base64.urlsafe_b64encode(...).rstrip(b"=")).
std::string b64encode(std::span<const std::byte> bytes, bool urlsafe = false, bool pad = true);
std::string b64encode(std::string_view bytes, bool urlsafe = false, bool pad = true);

// base64.b64decode(text, validate=True): nullopt wherever Python raises binascii.Error or
// ValueError. Only the alphabet and '=' are allowed (no whitespace, no non-ASCII), padding only
// at the end and only after two or three characters of a quad, nothing after it, and the length
// a whole number of quads once padded. Trailing bits of a short quad are not checked, as in
// Python. urlsafe also accepts '-' and '_' for '+' and '/' (Python translates them first).
std::optional<std::vector<std::byte>> b64decode(std::string_view text, bool urlsafe = false);

} // namespace fv
