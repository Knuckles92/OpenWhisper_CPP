#include "pyport/regex.h"

#include <algorithm>
#include <format>
#include <utility>
#include <variant>

#include "pyport/detail/unicode_data.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace fv::py {
namespace {

using detail::CodeRange;

constexpr std::uint32_t kBoundaryCallout = 1;
constexpr std::uint32_t kNonBoundaryCallout = 2;

// A class body ("\x{30}-\x{39}\x{5F}...") for ranges, or for their complement. PCRE2 refuses
// surrogate code points in UTF mode, and UTF-8 text cannot hold them, so they are left out.
std::string ranges_text(std::span<const CodeRange> ranges, bool complement) {
    std::vector<CodeRange> chosen;
    if (complement) {
        char32_t next = 0;
        for (const CodeRange& r : ranges) {
            if (r.first > next) {
                chosen.push_back({next, r.first - 1});
            }
            next = r.last + 1;
        }
        if (next <= 0x10FFFF) {
            chosen.push_back({next, 0x10FFFF});
        }
    } else {
        chosen.assign(ranges.begin(), ranges.end());
    }
    std::string out;
    const auto add = [&out](char32_t first, char32_t last) {
        out += std::format("\\x{{{:X}}}", static_cast<std::uint32_t>(first));
        if (last != first) {
            out += std::format("-\\x{{{:X}}}", static_cast<std::uint32_t>(last));
        }
    };
    for (const CodeRange& r : chosen) {
        if (r.last < 0xD800 || r.first > 0xDFFF) {
            add(r.first, r.last);
            continue;
        }
        if (r.first < 0xD800) {
            add(r.first, 0xD7FF);
        }
        if (r.last > 0xDFFF) {
            add(0xE000, r.last);
        }
    }
    return out;
}

// The body of Python's \w, \W, \d, \D, \s or \S as a PCRE2 class body.
const std::string& class_body(char escape) {
    static const std::string word = ranges_text(detail::word_ranges(), false);
    static const std::string not_word = ranges_text(detail::word_ranges(), true);
    static const std::string digit = ranges_text(detail::decimal_ranges(), false);
    static const std::string not_digit = ranges_text(detail::decimal_ranges(), true);
    static const std::string space = ranges_text(detail::space_ranges(), false);
    static const std::string not_space = ranges_text(detail::space_ranges(), true);
    switch (escape) {
    case 'w':
        return word;
    case 'W':
        return not_word;
    case 'd':
        return digit;
    case 'D':
        return not_digit;
    case 's':
        return space;
    default:
        return not_space;
    }
}

std::string code_point_escape(char32_t cp) {
    return std::format("\\x{{{:X}}}", static_cast<std::uint32_t>(cp));
}

bool is_hex(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool is_octal(char c) {
    return c >= '0' && c <= '7';
}

bool is_ascii_letter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

struct Translation {
    std::string pattern;
    std::uint32_t options = 0;
    bool callouts = false;
};

// Reads the hex digits of \xHH, \uHHHH or \UHHHHHHHH after the letter at p[i]; Python wants
// exactly that many.
char32_t read_hex_escape(std::string_view p, std::size_t& i, std::size_t digits, char letter) {
    if (i + 1 + digits > p.size()) {
        throw RegexError(std::format("incomplete escape \\{}{}", letter, p.substr(i + 1)));
    }
    std::uint32_t value = 0;
    for (std::size_t k = 0; k < digits; ++k) {
        const char c = p[i + 1 + k];
        if (!is_hex(c)) {
            throw RegexError(std::format("incomplete escape \\{}{}", letter, p.substr(i + 1, k)));
        }
        value = value * 16 + static_cast<std::uint32_t>(c <= '9'   ? c - '0'
                                                        : c <= 'F' ? c - 'A' + 10
                                                                   : c - 'a' + 10);
    }
    i += digits;
    if (value > 0x10FFFF) {
        throw RegexError(
            std::format("bad escape \\{}{}", letter, p.substr(i - digits + 1, digits)));
    }
    return value;
}

// re's error for \ooo above 0o377; digits are the escape's octal digits.
std::string octal_range_message(std::string_view digits) {
    return std::format("octal escape value \\{} outside of range 0-0o377", digits);
}

// Python's pattern syntax to PCRE2's (see Regex). i indexes the byte after the backslash.
void translate_escape(std::string_view p, std::size_t& i, bool in_class, Translation& t) {
    std::string& out = t.pattern;
    const char c = p[i];
    if (static_cast<unsigned char>(c) >= 0x80) {
        std::size_t pos = i;
        const char32_t cp = next_code_point(p, pos);
        i = pos - 1;
        out += code_point_escape(cp); // an escaped non-ASCII character is itself
        return;
    }
    switch (c) {
    case 'w':
    case 'W':
        // sre tests the category of the character itself even under IGNORECASE, while PCRE2
        // would add case partners (U+0345 folds to iota, a word character), so outside a set
        // the class is matched case-sensitively.
        out += in_class ? class_body(c) : "(?-i:[" + class_body(c) + "])";
        return;
    case 'd':
    case 'D':
    case 's':
    case 'S':
        out += in_class ? class_body(c) : "[" + class_body(c) + "]";
        return;
    case 'x':
        out += code_point_escape(read_hex_escape(p, i, 2, 'x'));
        return;
    case 'u':
        out += code_point_escape(read_hex_escape(p, i, 4, 'u'));
        return;
    case 'U':
        out += code_point_escape(read_hex_escape(p, i, 8, 'U'));
        return;
    case 'v':
        out += "\\x{B}";
        return;
    case 'a':
        out += "\\x{7}";
        return;
    case 'f':
    case 'n':
    case 'r':
    case 't':
        out += '\\';
        out += c;
        return;
    case 'N':
        throw RegexError("named Unicode escapes (\\N{...}) are not supported");
    default:
        break;
    }
    if (c == '0' || (in_class && is_octal(c))) {
        // Octal: \0 plus up to two digits anywhere, \1 to \7 plus up to two in a class.
        const std::size_t first = i;
        std::uint32_t value = static_cast<std::uint32_t>(c - '0');
        std::size_t count = 0;
        while (count < 2 && i + 1 < p.size() && is_octal(p[i + 1])) {
            value = value * 8 + static_cast<std::uint32_t>(p[++i] - '0');
            ++count;
        }
        if (value > 0377) {
            throw RegexError(octal_range_message(p.substr(first, i + 1 - first)));
        }
        out += code_point_escape(value);
        return;
    }
    if (c >= '1' && c <= '9') {
        if (in_class) {
            throw RegexError(std::format("bad escape \\{}", c));
        }
        // Three octal digits are a character, otherwise one or two digits are a group.
        if (i + 1 < p.size() && p[i + 1] >= '0' && p[i + 1] <= '9') {
            if (is_octal(c) && is_octal(p[i + 1]) && i + 2 < p.size() && is_octal(p[i + 2])) {
                const std::uint32_t value = static_cast<std::uint32_t>(
                    (c - '0') * 64 + (p[i + 1] - '0') * 8 + (p[i + 2] - '0'));
                if (value > 0377) {
                    throw RegexError(octal_range_message(p.substr(i, 3)));
                }
                i += 2;
                out += code_point_escape(value);
                return;
            }
            out += std::format("\\g{{{}}}", (c - '0') * 10 + (p[i + 1] - '0'));
            ++i;
            return;
        }
        out += std::format("\\g{{{}}}", c - '0');
        return;
    }
    if (!in_class) {
        switch (c) {
        case 'A':
            out += "\\A";
            return;
        case 'Z':
            out += "\\z";
            return;
        case 'b':
            out += std::format("(?C{})", kBoundaryCallout);
            t.callouts = true;
            return;
        case 'B':
            out += std::format("(?C{})", kNonBoundaryCallout);
            t.callouts = true;
            return;
        default:
            break;
        }
    } else if (c == 'b') {
        out += "\\x{8}";
        return;
    }
    if (is_ascii_letter(c) || (c >= '0' && c <= '9')) {
        throw RegexError(std::format("bad escape \\{}", c));
    }
    out += '\\';
    out += c;
}

// The code point at byte i as text, for error messages that quote it.
std::string code_point_text(std::string_view p, std::size_t i) {
    std::string out;
    append_utf8(out, next_code_point(p, i));
    return out;
}

bool is_flag(char c) {
    return std::string_view("aiLmsux").find(c) != std::string_view::npos;
}

// Python's _parser accepts only these openings after "(?"; PCRE2 knows more ((?<name>,
// (?P>name), (?R), (?C), (?|, (*VERB) ...), which must not silently take on PCRE2's meaning.
// i indexes the '('. Leading global flags were consumed before the scan, so any left here are
// not at the start.
void check_group_opening(std::string_view p, std::size_t i) {
    if (i + 1 >= p.size()) {
        return; // the missing ')' is reported by PCRE2
    }
    if (p[i + 1] == '*') {
        throw RegexError("nothing to repeat");
    }
    if (p[i + 1] != '?') {
        return;
    }
    if (i + 2 >= p.size()) {
        throw RegexError("unexpected end of pattern");
    }
    const char c = p[i + 2];
    switch (c) {
    case ':':
    case '#':
    case '=':
    case '!':
    case '>':
        return;
    case 'P':
    case '<': {
        if (i + 3 >= p.size()) {
            throw RegexError("unexpected end of pattern");
        }
        const char next = p[i + 3];
        const std::string_view allowed = c == 'P' ? "<=" : "=!";
        if (allowed.find(next) == std::string_view::npos) {
            throw RegexError(std::string("unknown extension ?") + c + code_point_text(p, i + 3));
        }
        return;
    }
    case '(': {
        const std::size_t close = p.find(')', i + 3);
        if (close == std::string_view::npos) {
            throw RegexError("missing ), unterminated name");
        }
        const std::string_view name = p.substr(i + 3, close - i - 3);
        if (name.empty()) {
            throw RegexError("missing group name");
        }
        const bool number =
            std::all_of(name.begin(), name.end(), [](char d) { return d >= '0' && d <= '9'; });
        if (!number && !isidentifier(name)) {
            throw RegexError("bad character in group name " + repr(name));
        }
        return;
    }
    default:
        break;
    }
    if (is_flag(c) || c == '-') {
        std::size_t j = i + 2;
        while (j < p.size() && (is_flag(p[j]) || p[j] == '-')) {
            ++j;
        }
        if (j < p.size() && p[j] == ')') {
            const bool turns_off = p.substr(i + 2, j - i - 2).find('-') != std::string_view::npos;
            throw RegexError(turns_off ? "missing :"
                                       : "global flags not at the start of the expression");
        }
        return; // scoped flags (?i:...), (?-i:...): PCRE2's syntax too
    }
    throw RegexError("unknown extension ?" + code_point_text(p, i + 2));
}

Translation translate(std::string_view p, unsigned flags) {
    Translation t;
    // Global inline flags at the start, as Python 3.11+ requires them. Python lets comments,
    // and in verbose mode whitespace, come before them.
    std::size_t i = 0;
    while (i < p.size()) {
        if ((flags & VERBOSE) != 0) {
            if (std::string_view(" \t\n\r\v\f").find(p[i]) != std::string_view::npos) {
                ++i;
                continue;
            }
            if (p[i] == '#') {
                while (i < p.size() && p[i] != '\n') {
                    ++i;
                }
                continue;
            }
        }
        if (p.substr(i, 3) == "(?#") {
            const std::size_t close = p.find(')', i + 3);
            if (close == std::string_view::npos) {
                throw RegexError("missing ), unterminated comment");
            }
            i = close + 1;
            continue;
        }
        if (p.substr(i, 2) != "(?" || i + 2 >= p.size()) {
            break;
        }
        std::size_t j = i + 2;
        unsigned found = 0;
        while (j < p.size() && std::string_view("aiLmsux").find(p[j]) != std::string_view::npos) {
            switch (p[j]) {
            case 'i':
                found |= IGNORECASE;
                break;
            case 'm':
                found |= MULTILINE;
                break;
            case 's':
                found |= DOTALL;
                break;
            case 'x':
                found |= VERBOSE;
                break;
            case 'u':
                break;
            default:
                throw RegexError("the ASCII and LOCALE flags are not supported");
            }
            ++j;
        }
        if (j == i + 2 || j >= p.size() || p[j] != ')') {
            break; // a group such as (?:...) or (?i:...), not a global flag
        }
        flags |= found;
        i = j + 1;
    }
    const bool verbose = (flags & VERBOSE) != 0;
    bool in_class = false;
    std::string& out = t.pattern;
    for (; i < p.size(); ++i) {
        const char c = p[i];
        if (c == '\\') {
            if (++i >= p.size()) {
                throw RegexError("bad escape (end of pattern)");
            }
            translate_escape(p, i, in_class, t);
        } else if (in_class) {
            if (c == ']') {
                in_class = false;
                out += ']';
            } else if (c == '[') {
                out += "\\["; // Python has no POSIX [:alpha:] classes
            } else {
                out += c;
            }
        } else if (c == '[') {
            in_class = true;
            out += '[';
            if (i + 1 < p.size() && p[i + 1] == '^') {
                out += '^';
                ++i;
            }
            if (i + 1 < p.size() && p[i + 1] == ']') {
                out += "\\]"; // a ']' first in the set is a literal
                ++i;
            }
        } else if (verbose && c == '#') {
            while (i < p.size() && p[i] != '\n') {
                out += p[i++];
            }
            if (i < p.size()) {
                out += '\n';
            }
        } else if (p.substr(i, 3) == "(?#") {
            // Dropped whole: a comment's text is not pattern syntax to translate.
            const std::size_t close = p.find(')', i + 3);
            if (close == std::string_view::npos) {
                throw RegexError("missing ), unterminated comment");
            }
            i = close;
        } else if (c == '(') {
            check_group_opening(p, i);
            out += c;
        } else {
            out += c;
        }
    }
    t.options = PCRE2_UTF | PCRE2_UCP | PCRE2_MATCH_INVALID_UTF | PCRE2_ALT_CIRCUMFLEX;
    if ((flags & IGNORECASE) != 0) {
        t.options |= PCRE2_CASELESS;
    }
    if ((flags & MULTILINE) != 0) {
        t.options |= PCRE2_MULTILINE;
    }
    if ((flags & DOTALL) != 0) {
        t.options |= PCRE2_DOTALL;
    }
    if (verbose) {
        t.options |= PCRE2_EXTENDED;
    }
    if (t.callouts) {
        // So that each \b and \B callout runs wherever the matcher would test it.
        t.options |= PCRE2_NO_AUTO_POSSESS | PCRE2_NO_START_OPTIMIZE;
    }
    return t;
}

// Python's \b and \B: at a word boundary by Python's \w, never on empty text.
int boundary_callout(pcre2_callout_block* block, void*) {
    const std::string_view text(reinterpret_cast<const char*>(block->subject),
                                block->subject_length);
    const std::size_t pos = block->current_position;
    bool before = false;
    if (pos > 0) {
        std::size_t start = pos - 1;
        while (start > 0 && (static_cast<unsigned char>(text[start]) & 0xC0) == 0x80) {
            --start;
        }
        before = is_word(next_code_point(text, start));
    }
    bool after = false;
    if (pos < text.size()) {
        std::size_t at = pos;
        after = is_word(next_code_point(text, at));
    }
    bool holds = false;
    if (!text.empty()) {
        holds = block->callout_number == kBoundaryCallout ? before != after : before == after;
    }
    return holds ? 0 : 1;
}

std::string pcre2_message(int code) {
    PCRE2_UCHAR buffer[256];
    if (pcre2_get_error_message(code, buffer, sizeof buffer) < 0) {
        return "PCRE2 error " + std::to_string(code);
    }
    return reinterpret_cast<const char*>(buffer);
}

using NameMap = std::map<std::string, std::size_t, std::less<>>;

// One piece of a parsed re.sub template: text, or a group to insert.
using TemplatePart = std::variant<std::string, std::size_t>;

std::string expand_parts(const std::vector<TemplatePart>& parts, const Match& m) {
    std::string out;
    for (const TemplatePart& part : parts) {
        if (const auto* s = std::get_if<std::string>(&part)) {
            out += *s;
        } else {
            out += m.group(std::get<std::size_t>(part));
        }
    }
    return out;
}

std::size_t byte_offset_of(std::string_view text, std::size_t code_points) {
    std::size_t pos = 0;
    for (std::size_t n = 0; n < code_points && pos < text.size(); ++n) {
        next_code_point(text, pos);
    }
    return pos;
}

} // namespace

struct Regex::Impl {
    pcre2_code* code = nullptr;
    pcre2_match_context* match_context = nullptr;
    std::size_t groups = 0;
    // Shared with every Match, which may outlive the Regex.
    std::shared_ptr<NameMap> names = std::make_shared<NameMap>();

    ~Impl() {
        pcre2_match_context_free(match_context);
        pcre2_code_free(code);
    }

    // Python's re._parser.parse_template, for a pattern of `groups` groups named by `names`.
    static std::vector<TemplatePart> parse_template(std::string_view repl, std::size_t groups,
                                                    const NameMap& names) {
        std::vector<TemplatePart> parts;
        std::string literal;
        const auto flush = [&] {
            if (!literal.empty()) {
                parts.emplace_back(std::move(literal));
                literal.clear();
            }
        };
        const auto add_group = [&](std::size_t index) {
            if (index > groups) {
                throw RegexError(std::format("invalid group reference {}", index));
            }
            flush();
            parts.emplace_back(index);
        };
        const auto digit = [](char c) {
            return c >= '0' && c <= '9';
        };
        for (std::size_t i = 0; i < repl.size(); ++i) {
            if (repl[i] != '\\') {
                literal += repl[i];
                continue;
            }
            if (++i >= repl.size()) {
                throw RegexError("bad escape (end of pattern)");
            }
            const char c = repl[i];
            if (c == 'g') {
                if (i + 1 >= repl.size() || repl[i + 1] != '<') {
                    throw RegexError("missing <");
                }
                const std::size_t close = repl.find('>', i + 2);
                if (close == std::string_view::npos) {
                    throw RegexError("missing >, unterminated name");
                }
                const std::string_view name = repl.substr(i + 2, close - i - 2);
                if (name.empty()) {
                    throw RegexError("missing group name");
                }
                i = close;
                if (std::all_of(name.begin(), name.end(), digit)) {
                    // Python quotes int(name), so leading zeros go and the number stays whole.
                    const std::size_t nonzero = name.find_first_not_of('0');
                    const std::string_view number =
                        nonzero == std::string_view::npos ? "0" : name.substr(nonzero);
                    std::size_t index = 0;
                    for (const char d : number) {
                        index = index * 10 + static_cast<std::size_t>(d - '0');
                        if (index > groups) {
                            throw RegexError(std::format("invalid group reference {}", number));
                        }
                    }
                    add_group(index);
                } else {
                    if (!isidentifier(name)) {
                        throw RegexError("bad character in group name " + repr(name));
                    }
                    const auto found = names.find(name);
                    if (found == names.end()) {
                        throw IndexError("unknown group name " + repr(name));
                    }
                    add_group(found->second);
                }
            } else if (c == '0') {
                unsigned value = 0;
                for (int k = 0; k < 2 && i + 1 < repl.size() && is_octal(repl[i + 1]); ++k) {
                    value = value * 8 + static_cast<unsigned>(repl[++i] - '0');
                }
                append_utf8(literal, value & 0xFF);
            } else if (digit(c)) {
                if (i + 1 < repl.size() && digit(repl[i + 1])) {
                    const char c2 = repl[i + 1];
                    if (is_octal(c) && is_octal(c2) && i + 2 < repl.size() &&
                        is_octal(repl[i + 2])) {
                        const unsigned value = static_cast<unsigned>(
                            (c - '0') * 64 + (c2 - '0') * 8 + (repl[i + 2] - '0'));
                        if (value > 0377) {
                            throw RegexError(octal_range_message(repl.substr(i, 3)));
                        }
                        i += 2;
                        append_utf8(literal, value);
                        continue;
                    }
                    ++i;
                    add_group(static_cast<std::size_t>((c - '0') * 10 + (c2 - '0')));
                } else {
                    add_group(static_cast<std::size_t>(c - '0'));
                }
            } else {
                switch (c) {
                case 'a':
                    literal += '\a';
                    break;
                case 'b':
                    literal += '\b';
                    break;
                case 'f':
                    literal += '\f';
                    break;
                case 'n':
                    literal += '\n';
                    break;
                case 'r':
                    literal += '\r';
                    break;
                case 't':
                    literal += '\t';
                    break;
                case 'v':
                    literal += '\v';
                    break;
                case '\\':
                    literal += '\\';
                    break;
                default:
                    if (is_ascii_letter(c)) {
                        throw RegexError(std::format("bad escape \\{}", c));
                    }
                    // Any other escaped character stays escaped, backslash included.
                    literal += '\\';
                    literal += c;
                }
            }
        }
        flush();
        return parts;
    }
};

Regex::Regex(std::string_view pattern, unsigned flags) : impl_(std::make_unique<Impl>()) {
    const Translation t = translate(pattern, flags);
    pcre2_compile_context* context = pcre2_compile_context_create(nullptr);
    pcre2_set_newline(context, PCRE2_NEWLINE_LF);
    int error = 0;
    PCRE2_SIZE offset = 0;
    impl_->code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(t.pattern.data()), t.pattern.size(),
                                t.options, &error, &offset, context);
    pcre2_compile_context_free(context);
    if (impl_->code == nullptr) {
        throw RegexError(pcre2_message(error));
    }
    impl_->match_context = pcre2_match_context_create(nullptr);
    if (t.callouts) {
        pcre2_set_callout(impl_->match_context, boundary_callout, nullptr);
    }
    std::uint32_t capture_count = 0;
    pcre2_pattern_info(impl_->code, PCRE2_INFO_CAPTURECOUNT, &capture_count);
    impl_->groups = capture_count;
    std::uint32_t name_count = 0;
    std::uint32_t entry_size = 0;
    PCRE2_SPTR table = nullptr;
    pcre2_pattern_info(impl_->code, PCRE2_INFO_NAMECOUNT, &name_count);
    pcre2_pattern_info(impl_->code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
    pcre2_pattern_info(impl_->code, PCRE2_INFO_NAMETABLE, &table);
    for (std::uint32_t n = 0; n < name_count; ++n) {
        const PCRE2_SPTR entry = table + static_cast<std::size_t>(n) * entry_size;
        const std::size_t number = (static_cast<std::size_t>(entry[0]) << 8) | entry[1];
        impl_->names->emplace(reinterpret_cast<const char*>(entry + 2), number);
    }
}

Regex::~Regex() = default;
Regex::Regex(Regex&&) noexcept = default;
Regex& Regex::operator=(Regex&&) noexcept = default;

std::size_t Regex::group_count() const {
    return impl_->groups;
}

const std::map<std::string, std::size_t, std::less<>>& Regex::group_index() const {
    return *impl_->names;
}

std::optional<Match> Regex::run(std::shared_ptr<const std::string> text, std::size_t byte_pos,
                                unsigned options) const {
    pcre2_match_data* data = pcre2_match_data_create_from_pattern(impl_->code, nullptr);
    const int rc = pcre2_match(impl_->code, reinterpret_cast<PCRE2_SPTR>(text->data()),
                               text->size(), byte_pos, options, data, impl_->match_context);
    if (rc < 0) {
        pcre2_match_data_free(data);
        if (rc == PCRE2_ERROR_NOMATCH) {
            return std::nullopt;
        }
        throw RegexError(pcre2_message(rc));
    }
    Match m;
    const PCRE2_SIZE* ovector = pcre2_get_ovector_pointer(data);
    m.offsets_.resize(2 * (impl_->groups + 1), Match::kUnset);
    for (std::size_t g = 0; g <= impl_->groups && g < static_cast<std::size_t>(rc); ++g) {
        if (ovector[2 * g] != PCRE2_UNSET) {
            m.offsets_[2 * g] = ovector[2 * g];
            m.offsets_[2 * g + 1] = ovector[2 * g + 1];
        }
    }
    pcre2_match_data_free(data);
    m.text_ = std::move(text);
    m.names_ = impl_->names;
    return m;
}

std::optional<Match> Regex::search(std::string_view text, std::size_t pos) const {
    auto owned = std::make_shared<const std::string>(text);
    return run(owned, byte_offset_of(*owned, pos), 0);
}

std::optional<Match> Regex::match(std::string_view text, std::size_t pos) const {
    auto owned = std::make_shared<const std::string>(text);
    return run(owned, byte_offset_of(*owned, pos), PCRE2_ANCHORED);
}

std::optional<Match> Regex::fullmatch(std::string_view text, std::size_t pos) const {
    auto owned = std::make_shared<const std::string>(text);
    return run(owned, byte_offset_of(*owned, pos), PCRE2_ANCHORED | PCRE2_ENDANCHORED);
}

// Python's scanner: after an empty match the next one may not be empty at the same place
// (sre's must_advance), which PCRE2_NOTEMPTY_ATSTART expresses exactly.
template <class Visit>
void Regex::each_match(std::string_view text, std::size_t limit, Visit visit) const {
    const auto owned = std::make_shared<const std::string>(text);
    std::size_t pos = 0;
    bool must_advance = false;
    for (std::size_t n = 0; limit == 0 || n < limit; ++n) {
        if (pos > owned->size()) {
            break;
        }
        auto m = run(owned, pos, must_advance ? PCRE2_NOTEMPTY_ATSTART : 0);
        if (!m) {
            break;
        }
        visit(*m);
        must_advance = m->byte_end() == m->byte_start();
        pos = m->byte_end();
    }
}

std::vector<Match> Regex::finditer(std::string_view text) const {
    std::vector<Match> out;
    each_match(text, 0, [&](const Match& m) { out.push_back(m); });
    return out;
}

std::pair<std::string, std::size_t> Regex::subn(std::string_view repl, std::string_view text,
                                                std::size_t count) const {
    const auto parts = Impl::parse_template(repl, impl_->groups, *impl_->names);
    std::string out;
    std::size_t last = 0;
    std::size_t n = 0;
    each_match(text, count, [&](const Match& m) {
        out.append(text.substr(last, m.byte_start() - last));
        out += expand_parts(parts, m);
        last = m.byte_end();
        ++n;
    });
    out.append(text.substr(last));
    return {std::move(out), n};
}

std::string Regex::sub(std::string_view repl, std::string_view text, std::size_t count) const {
    return subn(repl, text, count).first;
}

std::string Regex::sub(const std::function<std::string(const Match&)>& repl, std::string_view text,
                       std::size_t count) const {
    std::string out;
    std::size_t last = 0;
    each_match(text, count, [&](const Match& m) {
        out.append(text.substr(last, m.byte_start() - last));
        out += repl(m);
        last = m.byte_end();
    });
    out.append(text.substr(last));
    return out;
}

std::vector<std::optional<std::string>> Regex::split(std::string_view text,
                                                     std::size_t maxsplit) const {
    std::vector<std::optional<std::string>> out;
    std::size_t last = 0;
    each_match(text, maxsplit, [&](const Match& m) {
        out.emplace_back(std::string(text.substr(last, m.byte_start() - last)));
        for (auto& g : m.groups()) {
            out.push_back(std::move(g));
        }
        last = m.byte_end();
    });
    out.emplace_back(std::string(text.substr(last)));
    return out;
}

std::vector<std::string> Regex::findall(std::string_view text) const {
    if (impl_->groups > 1) {
        throw RegexError("findall of a pattern with several groups gives tuples: use "
                         "findall_groups");
    }
    std::vector<std::string> out;
    each_match(text, 0, [&](const Match& m) { out.push_back(m.group(impl_->groups)); });
    return out;
}

std::vector<std::vector<std::string>> Regex::findall_groups(std::string_view text) const {
    std::vector<std::vector<std::string>> out;
    each_match(text, 0, [&](const Match& m) {
        std::vector<std::string> row;
        if (impl_->groups == 0) {
            row.push_back(m.group(0));
        }
        for (std::size_t g = 1; g <= impl_->groups; ++g) {
            row.push_back(m.group(g));
        }
        out.push_back(std::move(row));
    });
    return out;
}

bool Match::matched(std::size_t index) const {
    return 2 * index + 1 < offsets_.size() && offsets_[2 * index] != kUnset;
}

std::optional<std::string> Match::group_if(std::size_t index) const {
    if (2 * index + 1 >= offsets_.size()) {
        throw IndexError("no such group");
    }
    if (!matched(index)) {
        return std::nullopt;
    }
    return text_->substr(offsets_[2 * index], offsets_[2 * index + 1] - offsets_[2 * index]);
}

std::string Match::group(std::size_t index) const {
    return group_if(index).value_or(std::string());
}

std::string Match::group(std::string_view name) const {
    const auto found = names_->find(name);
    if (found == names_->end()) {
        throw IndexError("no such group");
    }
    return group(found->second);
}

std::vector<std::optional<std::string>> Match::groups() const {
    std::vector<std::optional<std::string>> out;
    for (std::size_t g = 1; 2 * g + 1 < offsets_.size(); ++g) {
        out.push_back(group_if(g));
    }
    return out;
}

std::size_t Match::byte_start(std::size_t index) const {
    return matched(index) ? offsets_[2 * index] : kUnset;
}

std::size_t Match::byte_end(std::size_t index) const {
    return matched(index) ? offsets_[2 * index + 1] : kUnset;
}

std::size_t Match::start(std::size_t index) const {
    return matched(index) ? len(std::string_view(*text_).substr(0, offsets_[2 * index])) : kUnset;
}

std::size_t Match::end(std::size_t index) const {
    return matched(index) ? len(std::string_view(*text_).substr(0, offsets_[2 * index + 1]))
                          : kUnset;
}

std::string Match::expand(std::string_view repl) const {
    return expand_parts(Regex::Impl::parse_template(repl, offsets_.size() / 2 - 1, *names_), *this);
}

std::string regex_escape(std::string_view text) {
    static constexpr std::string_view kSpecial = "()[]{}?*+-|^$\\.&~# \t\n\r\v\f";
    std::string out;
    out.reserve(text.size());
    for (const char c : text) {
        if (kSpecial.find(c) != std::string_view::npos) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

} // namespace fv::py
