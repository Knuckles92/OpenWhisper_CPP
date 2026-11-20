#include "pyport/text.h"

#include <algorithm>
#include <format>

#include "pyport/detail/unicode_data.h"
#include "pyport/errors.h"
#include "pyport/unicode.h"

namespace fv::py {
namespace {

using detail::CaseKind;

constexpr char32_t kCapitalSigma = 0x3A3;
constexpr char32_t kSmallSigma = 0x3C3;
constexpr char32_t kFinalSigma = 0x3C2;

void append_mapped(std::string& out, char32_t cp, CaseKind kind) {
    char32_t mapped[3];
    const std::size_t n = detail::map_case(cp, kind, mapped);
    for (std::size_t i = 0; i < n; ++i) {
        append_utf8(out, mapped[i]);
    }
}

// CPython's handle_capital_sigma: sigma at i is final when a cased letter precedes it and none
// follows, case-ignorable code points skipped on both sides.
char32_t lower_sigma(const std::u32string& s, std::size_t i) {
    std::size_t j = i;
    char32_t c = 0;
    bool found = false;
    while (j > 0) {
        c = s[--j];
        if (!is_case_ignorable(c)) {
            found = true;
            break;
        }
    }
    bool final_sigma = found && is_cased(c);
    if (final_sigma && i + 1 < s.size()) {
        std::size_t k = i + 1;
        for (; k < s.size(); ++k) {
            c = s[k];
            if (!is_case_ignorable(c)) {
                break;
            }
        }
        final_sigma = k == s.size() || !is_cased(c);
    }
    return final_sigma ? kFinalSigma : kSmallSigma;
}

// CPython's lower_ucs4: the full lowercase mapping, with the final-sigma context.
void append_lower(std::string& out, const std::u32string& s, std::size_t i) {
    if (s[i] == kCapitalSigma) {
        append_utf8(out, lower_sigma(s, i));
    } else {
        append_mapped(out, s[i], CaseKind::Lower);
    }
}

std::string map_all(std::string_view utf8, CaseKind kind) {
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t pos = 0; pos < utf8.size();) {
        append_mapped(out, next_code_point(utf8, pos), kind);
    }
    return out;
}

template <class Pred> bool all_of_nonempty(std::string_view utf8, Pred pred) {
    if (utf8.empty()) {
        return false;
    }
    for (std::size_t pos = 0; pos < utf8.size();) {
        if (!pred(next_code_point(utf8, pos))) {
            return false;
        }
    }
    return true;
}

// Byte offset of each code point, plus the end, so that code point indices map to bytes.
std::vector<std::size_t> code_point_offsets(std::string_view utf8) {
    std::vector<std::size_t> offsets;
    offsets.reserve(utf8.size() + 1);
    for (std::size_t pos = 0; pos < utf8.size();) {
        offsets.push_back(pos);
        next_code_point(utf8, pos);
    }
    offsets.push_back(utf8.size());
    return offsets;
}

template <class Pred>
std::string strip_where(std::string_view utf8, bool left, bool right, Pred strip_it) {
    const auto offsets = code_point_offsets(utf8);
    std::size_t first = 0;
    std::size_t last = offsets.size() - 1; // code point count
    if (left) {
        while (first < last) {
            std::size_t pos = offsets[first];
            if (!strip_it(next_code_point(utf8, pos))) {
                break;
            }
            ++first;
        }
    }
    if (right) {
        while (last > first) {
            std::size_t pos = offsets[last - 1];
            if (!strip_it(next_code_point(utf8, pos))) {
                break;
            }
            --last;
        }
    }
    return std::string(utf8.substr(offsets[first], offsets[last] - offsets[first]));
}

auto contains_code_point(std::string_view chars) {
    return [set = to_code_points(chars)](char32_t cp) {
        return set.find(cp) != std::u32string::npos;
    };
}

} // namespace

std::size_t len(std::string_view utf8) {
    std::size_t n = 0;
    for (std::size_t pos = 0; pos < utf8.size(); ++n) {
        next_code_point(utf8, pos);
    }
    return n;
}

std::string slice(std::string_view utf8, std::optional<std::int64_t> start,
                  std::optional<std::int64_t> stop) {
    const auto offsets = code_point_offsets(utf8);
    const auto n = static_cast<std::int64_t>(offsets.size() - 1);
    const auto clamp = [n](std::optional<std::int64_t> index, std::int64_t fallback) {
        if (!index) {
            return fallback;
        }
        std::int64_t i = *index < 0 ? *index + n : *index;
        return std::clamp<std::int64_t>(i, 0, n);
    };
    const std::int64_t from = clamp(start, 0);
    const std::int64_t to = clamp(stop, n);
    if (to <= from) {
        return {};
    }
    const std::size_t begin = offsets[static_cast<std::size_t>(from)];
    return std::string(utf8.substr(begin, offsets[static_cast<std::size_t>(to)] - begin));
}

std::string casefold(std::string_view utf8) {
    return map_all(utf8, CaseKind::Fold);
}

std::string upper(std::string_view utf8) {
    return map_all(utf8, CaseKind::Upper);
}

std::string lower(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        append_lower(out, s, i);
    }
    return out;
}

std::string title(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    std::string out;
    out.reserve(utf8.size());
    bool previous_cased = false;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (previous_cased) {
            append_lower(out, s, i);
        } else {
            append_mapped(out, s[i], CaseKind::Title);
        }
        previous_cased = is_cased(s[i]);
    }
    return out;
}

std::string capitalize(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    std::string out;
    out.reserve(utf8.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (i == 0) {
            append_mapped(out, s[i], CaseKind::Title);
        } else {
            append_lower(out, s, i);
        }
    }
    return out;
}

std::string strip(std::string_view utf8) {
    return strip_where(utf8, true, true, is_space);
}
std::string lstrip(std::string_view utf8) {
    return strip_where(utf8, true, false, is_space);
}
std::string rstrip(std::string_view utf8) {
    return strip_where(utf8, false, true, is_space);
}
std::string strip(std::string_view utf8, std::string_view chars) {
    return strip_where(utf8, true, true, contains_code_point(chars));
}
std::string lstrip(std::string_view utf8, std::string_view chars) {
    return strip_where(utf8, true, false, contains_code_point(chars));
}
std::string rstrip(std::string_view utf8, std::string_view chars) {
    return strip_where(utf8, false, true, contains_code_point(chars));
}

std::vector<std::string> split(std::string_view utf8, std::int64_t maxsplit) {
    std::vector<std::string> out;
    std::size_t pos = 0;
    const auto skip_space = [&] {
        while (pos < utf8.size()) {
            std::size_t next = pos;
            if (!is_space(next_code_point(utf8, next))) {
                break;
            }
            pos = next;
        }
    };
    skip_space();
    while (pos < utf8.size()) {
        if (maxsplit >= 0 && static_cast<std::int64_t>(out.size()) == maxsplit) {
            // The remainder keeps its trailing whitespace, as CPython's split_whitespace does.
            out.emplace_back(utf8.substr(pos));
            return out;
        }
        const std::size_t start = pos;
        while (pos < utf8.size()) {
            std::size_t next = pos;
            if (is_space(next_code_point(utf8, next))) {
                break;
            }
            pos = next;
        }
        out.emplace_back(utf8.substr(start, pos - start));
        skip_space();
    }
    return out;
}

std::vector<std::string> split_on(std::string_view utf8, std::string_view sep,
                                  std::int64_t maxsplit) {
    if (sep.empty()) {
        throw ValueError("empty separator");
    }
    std::vector<std::string> out;
    std::size_t start = 0;
    while (maxsplit < 0 || static_cast<std::int64_t>(out.size()) < maxsplit) {
        const std::size_t found = utf8.find(sep, start);
        if (found == std::string_view::npos) {
            break;
        }
        out.emplace_back(utf8.substr(start, found - start));
        start = found + sep.size();
    }
    out.emplace_back(utf8.substr(start));
    return out;
}

bool isspace(std::string_view utf8) {
    return all_of_nonempty(utf8, is_space);
}
bool isalnum(std::string_view utf8) {
    return all_of_nonempty(utf8, is_alnum);
}
bool isalpha(std::string_view utf8) {
    return all_of_nonempty(utf8, is_alpha);
}
bool isdigit(std::string_view utf8) {
    return all_of_nonempty(utf8, is_digit);
}
bool isdecimal(std::string_view utf8) {
    return all_of_nonempty(utf8, is_decimal);
}
bool isnumeric(std::string_view utf8) {
    return all_of_nonempty(utf8, is_numeric);
}

bool isupper(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    if (s.size() == 1) {
        return is_upper(s[0]);
    }
    bool cased = false;
    for (const char32_t c : s) {
        if (is_lower(c) || is_title(c)) {
            return false;
        }
        cased = cased || is_upper(c);
    }
    return cased;
}

bool islower(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    if (s.size() == 1) {
        return is_lower(s[0]);
    }
    bool cased = false;
    for (const char32_t c : s) {
        if (is_upper(c) || is_title(c)) {
            return false;
        }
        cased = cased || is_lower(c);
    }
    return cased;
}

bool istitle(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    if (s.size() == 1) {
        return is_title(s[0]) || is_upper(s[0]);
    }
    bool cased = false;
    bool previous_is_cased = false;
    for (const char32_t c : s) {
        if (is_upper(c) || is_title(c)) {
            if (previous_is_cased) {
                return false;
            }
            previous_is_cased = cased = true;
        } else if (is_lower(c)) {
            if (!previous_is_cased) {
                return false;
            }
            previous_is_cased = cased = true;
        } else {
            previous_is_cased = false;
        }
    }
    return cased;
}

bool isprintable(std::string_view utf8) {
    for (std::size_t pos = 0; pos < utf8.size();) {
        if (!is_printable(next_code_point(utf8, pos))) {
            return false;
        }
    }
    return true;
}

bool isidentifier(std::string_view utf8) {
    if (utf8.empty()) {
        return false;
    }
    std::size_t pos = 0;
    const char32_t first = next_code_point(utf8, pos);
    if (first != U'_' && !is_xid_start(first)) {
        return false;
    }
    while (pos < utf8.size()) {
        if (!is_xid_continue(next_code_point(utf8, pos))) {
            return false;
        }
    }
    return true;
}

std::string html_escape(std::string_view utf8, bool quote) {
    std::string out;
    out.reserve(utf8.size());
    for (const char c : utf8) {
        switch (c) {
        case '&':
            out += "&amp;";
            break;
        case '<':
            out += "&lt;";
            break;
        case '>':
            out += "&gt;";
            break;
        case '"':
            out += quote ? "&quot;" : "\"";
            break;
        case '\'':
            out += quote ? "&#x27;" : "'";
            break;
        default:
            out.push_back(c);
        }
    }
    return out;
}

std::string repr(std::string_view utf8) {
    const std::u32string s = to_code_points(utf8);
    const bool has_single = s.find(U'\'') != std::u32string::npos;
    const bool has_double = s.find(U'"') != std::u32string::npos;
    const char32_t quote = (has_single && !has_double) ? U'"' : U'\'';
    std::string out(1, static_cast<char>(quote));
    for (const char32_t c : s) {
        if (c == quote || c == U'\\') {
            out.push_back('\\');
            out.push_back(static_cast<char>(c));
        } else if (c == U'\t') {
            out += "\\t";
        } else if (c == U'\n') {
            out += "\\n";
        } else if (c == U'\r') {
            out += "\\r";
        } else if (c < U' ' || c == 0x7F) {
            out += std::format("\\x{:02x}", static_cast<unsigned>(c));
        } else if (c < 0x7F || is_printable(c)) {
            append_utf8(out, c);
        } else if (c <= 0xFF) {
            out += std::format("\\x{:02x}", static_cast<unsigned>(c));
        } else if (c <= 0xFFFF) {
            out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        } else {
            out += std::format("\\U{:08x}", static_cast<unsigned>(c));
        }
    }
    out.push_back(static_cast<char>(quote));
    return out;
}

} // namespace fv::py
