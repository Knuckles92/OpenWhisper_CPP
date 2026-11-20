#include "base/detail/pytext.h"

#include <cstdint>
#include <utility>

namespace fv::detail {

namespace {

// The code point starting at text[pos] and its length in bytes; malformed bytes count as one
// byte that is not whitespace.
std::pair<char32_t, std::size_t> decode(std::string_view text, std::size_t pos) {
    const auto byte = [&](std::size_t i) {
        return static_cast<std::uint8_t>(text[i]);
    };
    const std::uint8_t lead = byte(pos);
    std::size_t length = 1;
    char32_t value = lead;
    if (lead >= 0xF0) {
        length = 4;
        value = lead & 0x07;
    } else if (lead >= 0xE0) {
        length = 3;
        value = lead & 0x0F;
    } else if (lead >= 0xC0) {
        length = 2;
        value = lead & 0x1F;
    } else if (lead >= 0x80) {
        return {0xFFFD, 1};
    }
    if (pos + length > text.size()) {
        return {0xFFFD, 1};
    }
    for (std::size_t i = 1; i < length; ++i) {
        if ((byte(pos + i) & 0xC0) != 0x80) {
            return {0xFFFD, 1};
        }
        value = (value << 6) | (byte(pos + i) & 0x3F);
    }
    return {value, length};
}

bool is_space(char32_t c) {
    return (c >= 0x09 && c <= 0x0D) || (c >= 0x1C && c <= 0x20) || c == 0x85 || c == 0xA0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200A) || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000;
}

bool is_line_break(char32_t c) {
    return c == '\n' || c == '\r' || c == 0x0B || c == 0x0C || (c >= 0x1C && c <= 0x1E) ||
           c == 0x85 || c == 0x2028 || c == 0x2029;
}

} // namespace

std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= 'A' && c <= 'Z') {
            out.push_back(static_cast<char>(c - 'A' + 'a'));
        } else if (text.substr(i, 3) == "\xE2\x84\xAA") { // KELVIN SIGN
            out.push_back('k');
            i += 2;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string ascii_upper(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c >= 'a' && c <= 'z') {
            out.push_back(static_cast<char>(c - 'a' + 'A'));
        } else if (text.substr(i, 2) == "\xC4\xB1") { // LATIN SMALL LETTER DOTLESS I
            out.push_back('I');
            ++i;
        } else if (text.substr(i, 2) == "\xC5\xBF") { // LATIN SMALL LETTER LONG S
            out.push_back('S');
            ++i;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

std::string_view py_strip(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size()) {
        const auto [c, length] = decode(text, begin);
        if (!is_space(c)) {
            break;
        }
        begin += length;
    }
    std::size_t end = text.size();
    while (end > begin) {
        std::size_t lead = end - 1;
        while (lead > begin && (static_cast<std::uint8_t>(text[lead]) & 0xC0) == 0x80) {
            --lead;
        }
        const auto [c, length] = decode(text, lead);
        if (lead + length != end || !is_space(c)) {
            break;
        }
        end = lead;
    }
    return text.substr(begin, end - begin);
}

std::vector<std::string_view> py_splitlines(std::string_view text) {
    std::vector<std::string_view> lines;
    std::size_t start = 0;
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto [c, length] = decode(text, pos);
        if (!is_line_break(c)) {
            pos += length;
            continue;
        }
        lines.push_back(text.substr(start, pos - start));
        pos += length;
        if (c == '\r' && pos < text.size() && text[pos] == '\n') {
            ++pos;
        }
        start = pos;
    }
    if (start < text.size()) {
        lines.push_back(text.substr(start));
    }
    return lines;
}

} // namespace fv::detail
