#include "backup/detail/json_bytes.h"

#include <cstdint>
#include <string>

#include "pyport/errors.h"
#include "pyport/json.h"
#include "pyport/unicode.h"

namespace fv::backup::detail {

namespace {

enum class Encoding { Utf8, Utf8Sig, Utf16, Utf16Be, Utf16Le, Utf32, Utf32Be, Utf32Le };

std::uint8_t at(std::string_view bytes, std::size_t i) {
    return static_cast<std::uint8_t>(bytes[i]);
}

// json.detect_encoding.
Encoding detect(std::string_view b) {
    if (b.starts_with(std::string_view("\x00\x00\xFE\xFF", 4)) ||
        b.starts_with(std::string_view("\xFF\xFE\x00\x00", 4))) {
        return Encoding::Utf32;
    }
    if (b.starts_with("\xFE\xFF") || b.starts_with("\xFF\xFE")) {
        return Encoding::Utf16;
    }
    if (b.starts_with("\xEF\xBB\xBF")) {
        return Encoding::Utf8Sig;
    }
    if (b.size() >= 4) {
        if (at(b, 0) == 0) {
            return at(b, 1) != 0 ? Encoding::Utf16Be : Encoding::Utf32Be;
        }
        if (at(b, 1) == 0) {
            return at(b, 2) != 0 || at(b, 3) != 0 ? Encoding::Utf16Le : Encoding::Utf32Le;
        }
    } else if (b.size() == 2) {
        if (at(b, 0) == 0) {
            return Encoding::Utf16Be;
        }
        if (at(b, 1) == 0) {
            return Encoding::Utf16Le;
        }
    }
    return Encoding::Utf8;
}

[[noreturn]] void undecodable() {
    throw py::ValueError("'utf-8' codec can't decode bytes");
}

// UTF-8 with "surrogatepass": three-byte surrogates are kept as they are.
std::string decode_utf8(std::string_view bytes) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const std::uint8_t lead = at(bytes, i);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t length = 0;
        char32_t code = 0;
        char32_t minimum = 0;
        if (lead >= 0xC2 && lead <= 0xDF) {
            length = 2;
            code = lead & 0x1F;
            minimum = 0x80;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            length = 3;
            code = lead & 0x0F;
            minimum = 0x800;
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            length = 4;
            code = lead & 0x07;
            minimum = 0x10000;
        } else {
            undecodable();
        }
        if (i + length > bytes.size()) {
            undecodable();
        }
        for (std::size_t k = 1; k < length; ++k) {
            const std::uint8_t next = at(bytes, i + k);
            if ((next & 0xC0) != 0x80) {
                undecodable();
            }
            code = (code << 6) | (next & 0x3F);
        }
        if (code < minimum || code > 0x10FFFF) {
            undecodable();
        }
        i += length;
    }
    return std::string(bytes);
}

std::string decode_utf16(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 2 != 0) {
        undecodable();
    }
    std::string out;
    auto unit = [&](std::size_t i) -> char32_t {
        return big_endian ? (char32_t{at(bytes, i)} << 8) | at(bytes, i + 1)
                          : (char32_t{at(bytes, i + 1)} << 8) | at(bytes, i);
    };
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        char32_t code = unit(i);
        if (code >= 0xD800 && code <= 0xDBFF && i + 2 < bytes.size()) {
            const char32_t low = unit(i + 2);
            if (low >= 0xDC00 && low <= 0xDFFF) {
                code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                i += 2;
            }
        }
        py::append_utf8(out, code);
    }
    return out;
}

std::string decode_utf32(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 4 != 0) {
        undecodable();
    }
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        const char32_t code =
            big_endian ? (char32_t{at(bytes, i)} << 24) | (char32_t{at(bytes, i + 1)} << 16) |
                             (char32_t{at(bytes, i + 2)} << 8) | at(bytes, i + 3)
                       : (char32_t{at(bytes, i + 3)} << 24) | (char32_t{at(bytes, i + 2)} << 16) |
                             (char32_t{at(bytes, i + 1)} << 8) | at(bytes, i);
        if (code > 0x10FFFF) {
            undecodable();
        }
        py::append_utf8(out, code);
    }
    return out;
}

std::string decode(std::string_view bytes) {
    switch (detect(bytes)) {
    case Encoding::Utf32:
        // The "utf-32" codec reads the byte-order mark and drops it.
        return decode_utf32(bytes.substr(4), at(bytes, 0) == 0);
    case Encoding::Utf16:
        return decode_utf16(bytes.substr(2), at(bytes, 0) == 0xFE);
    case Encoding::Utf8Sig:
        return decode_utf8(bytes.substr(3));
    case Encoding::Utf16Be:
        return decode_utf16(bytes, true);
    case Encoding::Utf16Le:
        return decode_utf16(bytes, false);
    case Encoding::Utf32Be:
        return decode_utf32(bytes, true);
    case Encoding::Utf32Le:
        return decode_utf32(bytes, false);
    case Encoding::Utf8:
        break;
    }
    return decode_utf8(bytes);
}

} // namespace

Json loads_bytes(std::string_view bytes) {
    return py::loads(decode(bytes));
}

} // namespace fv::backup::detail
