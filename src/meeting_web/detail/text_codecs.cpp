#include "meeting_web/detail/text_codecs.h"

#include <cstddef>
#include <cstdint>

namespace fv::meeting_web::detail {

namespace {

// The value of one base64 character (after the URL-safe translation), or 64 for anything else.
unsigned base64_value(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<unsigned>(c - 'A');
    }
    if (c >= 'a' && c <= 'z') {
        return static_cast<unsigned>(c - 'a') + 26U;
    }
    if (c >= '0' && c <= '9') {
        return static_cast<unsigned>(c - '0') + 52U;
    }
    if (c == '+' || c == '-') {
        return 62U;
    }
    if (c == '/' || c == '_') {
        return 63U;
    }
    return 64U;
}

void append_utf8(std::string& out, std::uint32_t code_point) {
    if (code_point < 0x80) {
        out += static_cast<char>(code_point);
    } else if (code_point < 0x800) {
        out += static_cast<char>(0xC0 | (code_point >> 6));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else if (code_point < 0x10000) {
        out += static_cast<char>(0xE0 | (code_point >> 12));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code_point >> 18));
        out += static_cast<char>(0x80 | ((code_point >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code_point >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code_point & 0x3F));
    }
}

// UTF-8 validation; `surrogates` accepts ED A0..BF xx (Python's surrogatepass).
bool check_utf8(std::string_view bytes, bool surrogates) {
    std::size_t i = 0;
    while (i < bytes.size()) {
        const auto lead = static_cast<unsigned char>(bytes[i]);
        if (lead < 0x80) {
            ++i;
            continue;
        }
        std::size_t extra = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            extra = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            extra = 2;
            if (lead == 0xE0) {
                low = 0xA0;
            } else if (lead == 0xED && !surrogates) {
                high = 0x9F;
            }
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            extra = 3;
            if (lead == 0xF0) {
                low = 0x90;
            } else if (lead == 0xF4) {
                high = 0x8F;
            }
        } else {
            return false;
        }
        if (i + extra >= bytes.size()) {
            return false;
        }
        const auto second = static_cast<unsigned char>(bytes[i + 1]);
        if (second < low || second > high) {
            return false;
        }
        for (std::size_t k = 2; k <= extra; ++k) {
            const auto next = static_cast<unsigned char>(bytes[i + k]);
            if (next < 0x80 || next > 0xBF) {
                return false;
            }
        }
        i += extra + 1;
    }
    return true;
}

std::optional<std::string> decode_utf16(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 2 != 0) {
        return std::nullopt;
    }
    auto unit = [&](std::size_t index) {
        const auto a = static_cast<unsigned char>(bytes[index]);
        const auto b = static_cast<unsigned char>(bytes[index + 1]);
        return static_cast<std::uint32_t>(big_endian ? (a << 8) | b : (b << 8) | a);
    };
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 2) {
        const std::uint32_t first = unit(i);
        if (first >= 0xD800 && first <= 0xDBFF && i + 2 < bytes.size()) {
            const std::uint32_t second = unit(i + 2);
            if (second >= 0xDC00 && second <= 0xDFFF) {
                append_utf8(out, 0x10000 + ((first - 0xD800) << 10) + (second - 0xDC00));
                i += 2;
                continue;
            }
        }
        append_utf8(out, first);
    }
    return out;
}

std::optional<std::string> decode_utf32(std::string_view bytes, bool big_endian) {
    if (bytes.size() % 4 != 0) {
        return std::nullopt;
    }
    std::string out;
    for (std::size_t i = 0; i < bytes.size(); i += 4) {
        std::uint32_t value = 0;
        for (std::size_t k = 0; k < 4; ++k) {
            const auto byte = static_cast<unsigned char>(bytes[i + (big_endian ? k : 3 - k)]);
            value = (value << 8) | byte;
        }
        if (value > 0x10FFFF) {
            return std::nullopt;
        }
        append_utf8(out, value);
    }
    return out;
}

bool starts_with(std::string_view bytes, std::string_view prefix) {
    return bytes.substr(0, prefix.size()) == prefix;
}

} // namespace

std::optional<std::string> b64decode_lenient(std::string_view text) {
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x80) {
            return std::nullopt;
        }
    }
    std::string out;
    int quad_pos = 0;
    int pads = 0;
    unsigned leftchar = 0;
    for (const char c : text) {
        if (c == '=') {
            if (quad_pos >= 2 && quad_pos + ++pads >= 4) {
                return out;
            }
            continue;
        }
        const unsigned value = base64_value(c);
        if (value >= 64U) {
            continue;
        }
        pads = 0;
        switch (quad_pos) {
        case 0:
            quad_pos = 1;
            leftchar = value;
            break;
        case 1:
            quad_pos = 2;
            out += static_cast<char>(((leftchar << 2) | (value >> 4)) & 0xFF);
            leftchar = value & 0x0F;
            break;
        case 2:
            quad_pos = 3;
            out += static_cast<char>(((leftchar << 4) | (value >> 2)) & 0xFF);
            leftchar = value & 0x03;
            break;
        default:
            quad_pos = 0;
            out += static_cast<char>(((leftchar << 6) | value) & 0xFF);
            leftchar = 0;
            break;
        }
    }
    if (quad_pos != 0) {
        return std::nullopt;
    }
    return out;
}

bool valid_utf8(std::string_view bytes) {
    return check_utf8(bytes, false);
}

std::optional<std::string> json_bytes_to_text(std::string_view bytes) {
    using namespace std::string_view_literals;
    if (starts_with(bytes, "\x00\x00\xFE\xFF"sv)) {
        return decode_utf32(bytes.substr(4), true);
    }
    if (starts_with(bytes, "\xFF\xFE\x00\x00"sv)) {
        return decode_utf32(bytes.substr(4), false);
    }
    if (starts_with(bytes, "\xFE\xFF"sv)) {
        return decode_utf16(bytes.substr(2), true);
    }
    if (starts_with(bytes, "\xFF\xFE"sv)) {
        return decode_utf16(bytes.substr(2), false);
    }
    if (starts_with(bytes, "\xEF\xBB\xBF"sv)) {
        bytes.remove_prefix(3);
        if (!check_utf8(bytes, true)) {
            return std::nullopt;
        }
        return std::string(bytes);
    }
    if (bytes.size() >= 4) {
        if (bytes[0] == '\0') {
            return bytes[1] != '\0' ? decode_utf16(bytes, true) : decode_utf32(bytes, true);
        }
        if (bytes[1] == '\0') {
            return bytes[2] != '\0' || bytes[3] != '\0' ? decode_utf16(bytes, false)
                                                        : decode_utf32(bytes, false);
        }
    } else if (bytes.size() == 2) {
        if (bytes[0] == '\0') {
            return decode_utf16(bytes, true);
        }
        if (bytes[1] == '\0') {
            return decode_utf16(bytes, false);
        }
    }
    if (!check_utf8(bytes, true)) {
        return std::nullopt;
    }
    return std::string(bytes);
}

} // namespace fv::meeting_web::detail
