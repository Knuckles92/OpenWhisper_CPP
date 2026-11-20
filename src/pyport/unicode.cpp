#include "pyport/unicode.h"

#include <algorithm>
#include <iterator>
#include <vector>

#include "pyport/detail/unicode_data.h"

namespace fv::py::detail {
namespace {

#include "pyport/data/unicode_tables.inc"

static_assert(std::string_view(FV_PY_UNIDATA_VERSION) == unidata_version);

constexpr const char* kCategoryNames[] = {
    "Cn", "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No", "Pc", "Pd", "Ps",
    "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So", "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co",
};

std::span<const CaseMapping> table(CaseKind kind) {
    switch (kind) {
    case CaseKind::Lower:
        return kLower;
    case CaseKind::Upper:
        return kUpper;
    case CaseKind::Title:
        return kTitle;
    case CaseKind::Fold:
        return kFold;
    }
    return {};
}

template <class Pred> std::vector<CodeRange> ranges_where(Pred pred) {
    std::vector<CodeRange> out;
    for (std::size_t i = 0; i < std::size(kRuns); ++i) {
        const char32_t first = kRuns[i].first;
        const char32_t last = i + 1 < std::size(kRuns) ? kRuns[i + 1].first - 1 : 0x10FFFF;
        if (!pred(first, kRecords[kRuns[i].record].flags)) {
            continue;
        }
        if (!out.empty() && out.back().last + 1 == first) {
            out.back().last = last;
        } else {
            out.push_back({first, last});
        }
    }
    return out;
}

} // namespace

const PropertyRecord& properties(char32_t cp) {
    if (cp > 0x10FFFF) {
        cp = 0x10FFFF; // a noncharacter: Cn with no flags, as Python treats nothing above it
    }
    const auto it =
        std::upper_bound(std::begin(kRuns), std::end(kRuns), cp,
                         [](char32_t c, const PropertyRun& run) { return c < run.first; });
    return kRecords[std::prev(it)->record];
}

const char* category_name(std::uint8_t category) {
    return category < std::size(kCategoryNames) ? kCategoryNames[category] : "Cn";
}

std::size_t map_case(char32_t cp, CaseKind kind, char32_t out[3]) {
    const auto entries = table(kind);
    const auto it = std::lower_bound(entries.begin(), entries.end(), cp,
                                     [](const CaseMapping& m, char32_t c) { return m.cp < c; });
    if (it == entries.end() || it->cp != cp) {
        out[0] = cp;
        return 1;
    }
    std::size_t n = 0;
    while (n < 3 && it->mapped[n] != 0) {
        out[n] = it->mapped[n];
        ++n;
    }
    return n;
}

std::span<const CodeRange> word_ranges() {
    static const std::vector<CodeRange> ranges =
        ranges_where([](char32_t first, std::uint16_t flags) {
            return (flags & (kAlpha | kDecimal | kDigit | kNumeric)) != 0 || first == U'_';
        });
    return ranges;
}

std::span<const CodeRange> decimal_ranges() {
    static const std::vector<CodeRange> ranges =
        ranges_where([](char32_t, std::uint16_t flags) { return (flags & kDecimal) != 0; });
    return ranges;
}

std::span<const CodeRange> space_ranges() {
    static const std::vector<CodeRange> ranges =
        ranges_where([](char32_t, std::uint16_t flags) { return (flags & kSpace) != 0; });
    return ranges;
}

} // namespace fv::py::detail

namespace fv::py {

using detail::properties;

char32_t next_code_point(std::string_view utf8, std::size_t& byte_pos) {
    const auto b0 = static_cast<unsigned char>(utf8[byte_pos]);
    if (b0 < 0x80) {
        ++byte_pos;
        return b0;
    }
    std::size_t length = 0;
    char32_t cp = 0;
    char32_t minimum = 0;
    if ((b0 & 0xE0) == 0xC0) {
        length = 2;
        cp = b0 & 0x1F;
        minimum = 0x80;
    } else if ((b0 & 0xF0) == 0xE0) {
        length = 3;
        cp = b0 & 0x0F;
        minimum = 0x800;
    } else if ((b0 & 0xF8) == 0xF0) {
        length = 4;
        cp = b0 & 0x07;
        minimum = 0x10000;
    } else {
        ++byte_pos;
        return 0xFFFD;
    }
    if (byte_pos + length > utf8.size()) {
        ++byte_pos;
        return 0xFFFD;
    }
    for (std::size_t i = 1; i < length; ++i) {
        const auto b = static_cast<unsigned char>(utf8[byte_pos + i]);
        if ((b & 0xC0) != 0x80) {
            ++byte_pos;
            return 0xFFFD;
        }
        cp = (cp << 6) | (b & 0x3F);
    }
    if (cp < minimum || cp > 0x10FFFF) {
        ++byte_pos;
        return 0xFFFD;
    }
    byte_pos += length;
    return cp;
}

std::u32string to_code_points(std::string_view utf8) {
    std::u32string out;
    out.reserve(utf8.size());
    for (std::size_t pos = 0; pos < utf8.size();) {
        out.push_back(next_code_point(utf8, pos));
    }
    return out;
}

void append_utf8(std::string& out, char32_t cp) {
    if (cp < 0x80) {
        out.push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

std::string to_utf8(std::u32string_view code_points) {
    std::string out;
    out.reserve(code_points.size());
    for (const char32_t cp : code_points) {
        append_utf8(out, cp);
    }
    return out;
}

std::string_view general_category(char32_t cp) {
    return detail::category_name(properties(cp).category);
}

std::optional<int> decimal_value(char32_t cp) {
    if ((properties(cp).flags & detail::kDecimal) == 0) {
        return std::nullopt;
    }
    const auto zeros = std::span(detail::kDecimalZeros);
    const auto it = std::upper_bound(zeros.begin(), zeros.end(), cp);
    return static_cast<int>(cp - *std::prev(it));
}

namespace {
bool has(char32_t cp, std::uint16_t flag) {
    return (properties(cp).flags & flag) != 0;
}
} // namespace

bool is_alpha(char32_t cp) {
    return has(cp, detail::kAlpha);
}
bool is_decimal(char32_t cp) {
    return has(cp, detail::kDecimal);
}
bool is_digit(char32_t cp) {
    return has(cp, detail::kDigit);
}
bool is_numeric(char32_t cp) {
    return has(cp, detail::kNumeric);
}
bool is_alnum(char32_t cp) {
    return has(cp, detail::kAlpha | detail::kDecimal | detail::kDigit | detail::kNumeric);
}
bool is_space(char32_t cp) {
    return has(cp, detail::kSpace);
}
bool is_lower(char32_t cp) {
    return has(cp, detail::kLower);
}
bool is_upper(char32_t cp) {
    return has(cp, detail::kUpper);
}
bool is_title(char32_t cp) {
    return has(cp, detail::kTitle);
}
bool is_cased(char32_t cp) {
    return has(cp, detail::kCased);
}
bool is_case_ignorable(char32_t cp) {
    return has(cp, detail::kCaseIgnorable);
}
bool is_printable(char32_t cp) {
    return has(cp, detail::kPrintable);
}
bool is_xid_start(char32_t cp) {
    return has(cp, detail::kXidStart);
}
bool is_xid_continue(char32_t cp) {
    return has(cp, detail::kXidContinue);
}
bool is_word(char32_t cp) {
    return cp == U'_' || is_alnum(cp);
}

} // namespace fv::py
