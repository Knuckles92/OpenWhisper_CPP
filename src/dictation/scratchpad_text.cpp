#include "dictation/scratchpad_text.h"

#include <algorithm>
#include <string>
#include <vector>

#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::dictation {

namespace {

// _NO_SPACE_AFTER and _NO_SPACE_BEFORE.
constexpr std::u32string_view kNoSpaceAfter = U"([{\"'“‘/-—\n\t ";
constexpr std::u32string_view kNoSpaceBefore = U".,;:!?)]}%…'\"’”";

char32_t first_code_point(std::string_view utf8) {
    std::size_t pos = 0;
    return py::next_code_point(utf8, pos);
}

char32_t last_code_point(std::string_view utf8) {
    // Back up over UTF-8 continuation bytes to the start of the last code point.
    std::size_t start = utf8.size() - 1;
    while (start > 0 && (static_cast<unsigned char>(utf8[start]) & 0xC0) == 0x80) {
        --start;
    }
    return first_code_point(utf8.substr(start));
}

std::vector<char16_t> to_utf16(std::string_view utf8) {
    std::vector<char16_t> units;
    units.reserve(utf8.size());
    std::size_t pos = 0;
    while (pos < utf8.size()) {
        const char32_t cp = py::next_code_point(utf8, pos);
        if (cp >= 0x10000) {
            const char32_t v = cp - 0x10000;
            units.push_back(static_cast<char16_t>(0xD800 + (v >> 10)));
            units.push_back(static_cast<char16_t>(0xDC00 + (v & 0x3FF)));
        } else {
            units.push_back(static_cast<char16_t>(cp));
        }
    }
    return units;
}

std::string group_thousands(std::size_t value) {
    const std::string digits = std::to_string(value);
    std::string out;
    for (std::size_t i = 0; i < digits.size(); ++i) {
        if (i != 0 && (digits.size() - i) % 3 == 0) {
            out += ',';
        }
        out += digits[i];
    }
    return out;
}

} // namespace

std::string utf16_slice(std::string_view text, std::int64_t start, std::int64_t end) {
    const std::vector<char16_t> units = to_utf16(text);
    const auto size = static_cast<std::int64_t>(units.size());
    const std::int64_t from = std::clamp<std::int64_t>(start, 0, size);
    const std::int64_t to = std::clamp<std::int64_t>(end, 0, size);
    std::string out;
    for (std::int64_t i = from; i < to; ++i) {
        const char16_t unit = units[static_cast<std::size_t>(i)];
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            if (i + 1 < to) {
                const char16_t low = units[static_cast<std::size_t>(i + 1)];
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    py::append_utf8(out, 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
                                             (static_cast<char32_t>(low) - 0xDC00));
                    ++i;
                }
            }
            continue;
        }
        if (unit >= 0xDC00 && unit <= 0xDFFF) {
            continue;
        }
        py::append_utf8(out, unit);
    }
    return out;
}

std::string spaced(std::string_view before, std::string_view text, std::string_view after) {
    if (text.empty()) {
        return std::string(text);
    }
    std::string out(text);
    const char32_t first = first_code_point(text);
    if (!before.empty() &&
        kNoSpaceAfter.find(last_code_point(before)) == std::u32string_view::npos &&
        !py::is_space(first) && kNoSpaceBefore.find(first) == std::u32string_view::npos) {
        out.insert(out.begin(), ' ');
    }
    if (!after.empty() && py::is_alnum(first_code_point(after)) &&
        !py::is_space(last_code_point(out))) {
        out += ' ';
    }
    return out;
}

std::string append_block(std::string_view existing, std::string_view text) {
    std::string separator;
    if (!py::strip(existing).empty()) {
        separator = existing.ends_with('\n') ? "\n" : "\n\n";
    }
    return separator + py::strip(text);
}

std::string word_count_text(std::string_view text) {
    const std::size_t count = py::split(text).size();
    if (count == 0) {
        return "";
    }
    if (count == 1) {
        return "1 word";
    }
    return group_thousands(count) + " words";
}

} // namespace fv::dictation
