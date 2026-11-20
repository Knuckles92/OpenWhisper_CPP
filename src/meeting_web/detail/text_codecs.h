#pragma once

#include <optional>
#include <string>
#include <string_view>

// The byte-level decoding behind the dashboard's cursors, as Python 3.12 does it.
namespace fv::meeting_web::detail {

// base64.urlsafe_b64decode(text): '-' and '_' read as '+' and '/', characters outside the
// alphabet skipped, decoding stopped by a complete pad sequence (binascii.a2b_base64 without
// strict mode). nullopt where Python raises: non-ASCII text, "Incorrect padding", or one data
// character more than a multiple of four.
std::optional<std::string> b64decode_lenient(std::string_view text);

// bytes.decode("utf-8") succeeds: no overlong forms, no surrogates, nothing above U+10FFFF.
bool valid_utf8(std::string_view bytes);

// json.loads(bytes)'s text: json.detect_encoding (UTF-32 and UTF-16 BOMs, the UTF-8 BOM, the
// null-byte patterns of UTF-16 and UTF-32 without a BOM, else UTF-8), decoded with
// surrogatepass, as UTF-8 (a lone surrogate as its three-byte form). nullopt when Python's
// decode raises.
std::optional<std::string> json_bytes_to_text(std::string_view bytes);

} // namespace fv::meeting_web::detail
