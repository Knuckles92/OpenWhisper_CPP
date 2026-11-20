#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace fv::credentials_detail {

// Where Python's strict "utf-8" codec would stop: overlong forms, surrogates, code points above
// U+10FFFF and truncated sequences are errors, as in CPython.
struct Utf8Error {
    std::size_t position;    // byte offset of the offending sequence
    std::size_t end;         // one past its last byte
    unsigned char byte;      // the byte at position
    std::string_view reason; // CPython's: "invalid start byte", ...
};

std::optional<Utf8Error> check_utf8(std::string_view bytes);

// CPython's UnicodeDecodeError text for it, which names bytes and offsets, never the content
// around them: "'utf-8' codec can't decode byte 0xff in position 3: invalid start byte".
std::string describe(const Utf8Error&);

} // namespace fv::credentials_detail
