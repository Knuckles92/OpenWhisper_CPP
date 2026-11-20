#include "pyport/json.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <format>
#include <limits>
#include <utility>
#include <vector>

#include "pyport/number.h"
#include "pyport/unicode.h"

namespace fv::py {
namespace {

// ---------------------------------------------------------------------------------------------
// dumps

void write_string(std::string& out, const std::string& s, bool ensure_ascii) {
    out.push_back('"');
    for (std::size_t pos = 0; pos < s.size();) {
        const std::size_t start = pos;
        const char32_t c = next_code_point(s, pos);
        switch (c) {
        case U'"':
            out += "\\\"";
            continue;
        case U'\\':
            out += "\\\\";
            continue;
        case U'\b':
            out += "\\b";
            continue;
        case U'\f':
            out += "\\f";
            continue;
        case U'\n':
            out += "\\n";
            continue;
        case U'\r':
            out += "\\r";
            continue;
        case U'\t':
            out += "\\t";
            continue;
        default:
            break;
        }
        if (c < 0x20 || (ensure_ascii && c > 0x7E)) {
            if (c >= 0x10000) {
                const char32_t v = c - 0x10000;
                out += std::format("\\u{:04x}\\u{:04x}", static_cast<unsigned>(0xD800 | (v >> 10)),
                                   static_cast<unsigned>(0xDC00 | (v & 0x3FF)));
            } else {
                out += std::format("\\u{:04x}", static_cast<unsigned>(c));
            }
        } else {
            out.append(s, start, pos - start);
        }
    }
    out.push_back('"');
}

void write_scalar(std::string& out, const Json& value, bool ensure_ascii) {
    switch (value.type()) {
    case Json::value_t::null:
        out += "null";
        break;
    case Json::value_t::boolean:
        out += value.get<bool>() ? "true" : "false";
        break;
    case Json::value_t::number_integer:
        out += std::to_string(value.get<std::int64_t>());
        break;
    case Json::value_t::number_unsigned:
        out += std::to_string(value.get<std::uint64_t>());
        break;
    case Json::value_t::number_float: {
        const double d = value.get<double>();
        if (std::isnan(d)) {
            out += "NaN";
        } else if (std::isinf(d)) {
            out += d > 0 ? "Infinity" : "-Infinity";
        } else {
            out += float_repr(d);
        }
        break;
    }
    case Json::value_t::string:
        write_string(out, value.get_ref<const std::string&>(), ensure_ascii);
        break;
    default:
        throw TypeError(
            std::format("Object of type {} is not JSON serializable", value.type_name()));
    }
}

// One open container while writing: its members in output order.
struct WriteFrame {
    bool object = false;
    std::vector<std::pair<const std::string*, const Json*>> items;
    std::size_t next = 0;
};

WriteFrame open_frame(const Json& value, bool sort_keys) {
    WriteFrame frame;
    frame.object = value.is_object();
    if (frame.object) {
        for (const auto& [key, member] : value.items()) {
            frame.items.emplace_back(&key, &member);
        }
        if (sort_keys) {
            std::stable_sort(frame.items.begin(), frame.items.end(),
                             [](const auto& a, const auto& b) { return *a.first < *b.first; });
        }
    } else {
        for (const auto& member : value) {
            frame.items.emplace_back(nullptr, &member);
        }
    }
    return frame;
}

// ---------------------------------------------------------------------------------------------
// loads

class Parser {
public:
    explicit Parser(std::string_view text) : s_(to_code_points(text)) {}

    Json parse() {
        if (!s_.empty() && s_[0] == 0xFEFF) {
            fail("Unexpected UTF-8 BOM (decode using utf-8-sig)", 0);
        }
        std::size_t idx = skip_ws(0);
        Json value = parse_value(idx);
        idx = skip_ws(idx);
        if (idx != s_.size()) {
            fail("Extra data", idx);
        }
        return value;
    }

private:
    struct Frame {
        Json container;
        std::string key; // the member being read, for an object
    };

    [[noreturn]] void fail(const char* msg, std::size_t pos) const {
        std::size_t lineno = 1;
        std::size_t last_newline = 0;
        bool newline_seen = false;
        for (std::size_t i = 0; i < pos && i < s_.size(); ++i) {
            if (s_[i] == U'\n') {
                ++lineno;
                last_newline = i;
                newline_seen = true;
            }
        }
        const std::size_t colno = newline_seen ? pos - last_newline : pos + 1;
        throw JsonError(msg, pos, lineno, colno);
    }

    char32_t at(std::size_t idx) const { return idx < s_.size() ? s_[idx] : 0; }

    std::size_t skip_ws(std::size_t idx) const {
        while (idx < s_.size() &&
               (s_[idx] == U' ' || s_[idx] == U'\t' || s_[idx] == U'\n' || s_[idx] == U'\r')) {
            ++idx;
        }
        return idx;
    }

    bool matches(std::size_t idx, std::u32string_view word) const {
        return idx + word.size() <= s_.size() && s_.compare(idx, word.size(), word) == 0;
    }

    void enter(std::vector<Frame>& stack, bool object) const {
        if (stack.size() >= kJsonMaxDepth) {
            throw RecursionError(std::format(
                "maximum recursion depth exceeded while decoding a JSON {} from a unicode string",
                object ? "object" : "array"));
        }
        stack.push_back({object ? Json::object() : Json::array(), {}});
    }

    // The member key at idx (which must be '"'), then the ':' and the whitespace after it.
    std::size_t read_key(std::size_t idx, Frame& frame) const {
        if (at(idx) != U'"' || idx >= s_.size()) {
            fail("Expecting property name enclosed in double quotes", idx);
        }
        idx = scan_string(idx + 1, frame.key);
        idx = skip_ws(idx);
        if (idx >= s_.size() || s_[idx] != U':') {
            fail("Expecting ':' delimiter", idx);
        }
        return skip_ws(idx + 1);
    }

    // CPython's scan_once over an explicit stack, so that kJsonMaxDepth containers do not
    // need kJsonMaxDepth native frames. idx is advanced past the value.
    Json parse_value(std::size_t& idx) const {
        std::vector<Frame> stack;
        Json value;
        while (true) {
            // Read one value at idx, or open a container and continue with its first member.
            if (idx >= s_.size()) {
                fail("Expecting value", idx);
            }
            const char32_t c = s_[idx];
            if (c == U'{' || c == U'[') {
                const bool object = c == U'{';
                enter(stack, object);
                idx = skip_ws(idx + 1);
                if (at(idx) == (object ? U'}' : U']') && idx < s_.size()) {
                    ++idx;
                    value = std::move(stack.back().container);
                    stack.pop_back();
                } else {
                    if (object) {
                        idx = read_key(idx, stack.back());
                    }
                    continue;
                }
            } else {
                value = scalar(idx);
            }
            // Store the finished value in its container and close every container that ends.
            while (true) {
                if (stack.empty()) {
                    return value;
                }
                Frame& frame = stack.back();
                const bool object = frame.container.is_object();
                if (object) {
                    frame.container[frame.key] = std::move(value);
                } else {
                    frame.container.push_back(std::move(value));
                }
                idx = skip_ws(idx);
                if (idx < s_.size() && s_[idx] == (object ? U'}' : U']')) {
                    ++idx;
                    value = std::move(frame.container);
                    stack.pop_back();
                    continue;
                }
                if (idx >= s_.size() || s_[idx] != U',') {
                    fail("Expecting ',' delimiter", idx);
                }
                idx = skip_ws(idx + 1);
                if (object) {
                    idx = read_key(idx, frame);
                }
                break;
            }
        }
    }

    Json scalar(std::size_t& idx) const {
        const char32_t c = s_[idx];
        switch (c) {
        case U'"': {
            std::string out;
            idx = scan_string(idx + 1, out);
            return out;
        }
        case U'n':
            if (matches(idx, U"null")) {
                idx += 4;
                return nullptr;
            }
            break;
        case U't':
            if (matches(idx, U"true")) {
                idx += 4;
                return true;
            }
            break;
        case U'f':
            if (matches(idx, U"false")) {
                idx += 5;
                return false;
            }
            break;
        case U'N':
            if (matches(idx, U"NaN")) {
                idx += 3;
                return std::numeric_limits<double>::quiet_NaN();
            }
            break;
        case U'I':
            if (matches(idx, U"Infinity")) {
                idx += 8;
                return std::numeric_limits<double>::infinity();
            }
            break;
        case U'-':
            if (matches(idx, U"-Infinity")) {
                idx += 9;
                return -std::numeric_limits<double>::infinity();
            }
            break;
        default:
            break;
        }
        return number(idx);
    }

    static bool digit(char32_t c) { return c >= U'0' && c <= U'9'; }

    // CPython's _match_number_unicode.
    Json number(std::size_t& idx) const {
        const std::size_t start = idx;
        std::size_t i = idx;
        if (at(i) == U'-') {
            ++i;
            if (i >= s_.size()) {
                fail("Expecting value", start);
            }
        }
        if (at(i) >= U'1' && at(i) <= U'9') {
            ++i;
            while (i < s_.size() && digit(s_[i])) {
                ++i;
            }
        } else if (at(i) == U'0') {
            ++i;
        } else {
            fail("Expecting value", start);
        }
        bool is_float = false;
        if (i + 1 < s_.size() && s_[i] == U'.' && digit(s_[i + 1])) {
            is_float = true;
            i += 2;
            while (i < s_.size() && digit(s_[i])) {
                ++i;
            }
        }
        if (i + 1 < s_.size() && (s_[i] == U'e' || s_[i] == U'E')) {
            const std::size_t e_start = i;
            ++i;
            if (i + 1 < s_.size() && (s_[i] == U'-' || s_[i] == U'+')) {
                ++i;
            }
            while (i < s_.size() && digit(s_[i])) {
                ++i;
            }
            if (digit(s_[i - 1])) {
                is_float = true;
            } else {
                i = e_start;
            }
        }
        std::string ascii;
        for (std::size_t k = start; k < i; ++k) {
            ascii.push_back(static_cast<char>(s_[k]));
        }
        idx = i;
        const char* first = ascii.data();
        const char* last = ascii.data() + ascii.size();
        if (!is_float) {
            std::int64_t v = 0;
            if (std::from_chars(first, last, v).ec == std::errc()) {
                return v;
            }
            std::uint64_t u = 0;
            if (ascii[0] != '-' && std::from_chars(first, last, u).ec == std::errc()) {
                return u;
            }
        }
        // An integer beyond uint64 also lands here, as the nearest float. The text always
        // parses: the grammar above is a subset of float()'s.
        return py_float(ascii).value_or(0.0);
    }

    static int hex_value(char32_t c) {
        if (c >= U'0' && c <= U'9') {
            return static_cast<int>(c - U'0');
        }
        if (c >= U'a' && c <= U'f') {
            return static_cast<int>(c - U'a' + 10);
        }
        if (c >= U'A' && c <= U'F') {
            return static_cast<int>(c - U'A' + 10);
        }
        return -1;
    }

    // CPython's scanstring_unicode (strict): end is just past the opening quote; returns the
    // index past the closing quote.
    std::size_t scan_string(std::size_t end, std::string& out) const {
        out.clear();
        const std::size_t begin = end - 1;
        const std::size_t len = s_.size();
        while (true) {
            std::size_t next = end;
            char32_t c = 0;
            for (; next < len; ++next) {
                c = s_[next];
                if (c == U'"' || c == U'\\') {
                    break;
                }
                if (c <= 0x1F) {
                    fail("Invalid control character at", next);
                }
            }
            if (!(c == U'"' || c == U'\\')) {
                fail("Unterminated string starting at", begin);
            }
            for (std::size_t k = end; k < next; ++k) {
                append_utf8(out, s_[k]);
            }
            ++next;
            if (c == U'"') {
                return next;
            }
            if (next == len) {
                fail("Unterminated string starting at", begin);
            }
            c = s_[next];
            if (c != U'u') {
                end = next + 1;
                switch (c) {
                case U'"':
                case U'\\':
                case U'/':
                    break;
                case U'b':
                    c = U'\b';
                    break;
                case U'f':
                    c = U'\f';
                    break;
                case U'n':
                    c = U'\n';
                    break;
                case U'r':
                    c = U'\r';
                    break;
                case U't':
                    c = U'\t';
                    break;
                default:
                    fail("Invalid \\escape", end - 2);
                }
            } else {
                c = 0;
                ++next;
                end = next + 4;
                if (end >= len) {
                    fail("Invalid \\uXXXX escape", next - 1);
                }
                for (; next < end; ++next) {
                    const int h = hex_value(s_[next]);
                    if (h < 0) {
                        fail("Invalid \\uXXXX escape", end - 5);
                    }
                    c = (c << 4) | static_cast<char32_t>(h);
                }
                // A high surrogate followed by an escaped low surrogate is one code point.
                if (c >= 0xD800 && c <= 0xDBFF && end + 6 < len && s_[next] == U'\\' &&
                    s_[next + 1] == U'u') {
                    next += 2;
                    char32_t c2 = 0;
                    end += 6;
                    for (; next < end; ++next) {
                        const int h = hex_value(s_[next]);
                        if (h < 0) {
                            fail("Invalid \\uXXXX escape", end - 5);
                        }
                        c2 = (c2 << 4) | static_cast<char32_t>(h);
                    }
                    if (c2 >= 0xDC00 && c2 <= 0xDFFF) {
                        c = 0x10000 + (((c - 0xD800) << 10) | (c2 - 0xDC00));
                    } else {
                        end -= 6;
                    }
                }
            }
            append_utf8(out, c);
        }
    }

    std::u32string s_;
};

// Python compares int and float exactly; these do so without a wider type.
bool equal_int_float(std::int64_t i, double d) {
    if (!std::isfinite(d) || d != std::trunc(d) || d < -9223372036854775808.0 ||
        d >= 9223372036854775808.0) {
        return false;
    }
    return static_cast<std::int64_t>(d) == i;
}

bool equal_uint_float(std::uint64_t u, double d) {
    if (!std::isfinite(d) || d != std::trunc(d) || d < 0.0 || d >= 18446744073709551616.0) {
        return false;
    }
    return static_cast<std::uint64_t>(d) == u;
}

bool numeric(const Json& v) {
    return v.is_number() || v.is_boolean();
}

bool equal_numbers(const Json& a, const Json& b) {
    const auto as_int = [](const Json& v) {
        return v.is_boolean() ? std::int64_t{v.get<bool>() ? 1 : 0} : v.get<std::int64_t>();
    };
    const bool a_float = a.is_number_float();
    const bool b_float = b.is_number_float();
    const bool a_unsigned = a.is_number_unsigned();
    const bool b_unsigned = b.is_number_unsigned();
    if (a_float && b_float) {
        return a.get<double>() == b.get<double>();
    }
    if (a_float || b_float) {
        const Json& f = a_float ? a : b;
        const Json& n = a_float ? b : a;
        return n.is_number_unsigned() ? equal_uint_float(n.get<std::uint64_t>(), f.get<double>())
                                      : equal_int_float(as_int(n), f.get<double>());
    }
    if (a_unsigned && b_unsigned) {
        return a.get<std::uint64_t>() == b.get<std::uint64_t>();
    }
    if (a_unsigned || b_unsigned) {
        const Json& u = a_unsigned ? a : b;
        const std::int64_t i = as_int(a_unsigned ? b : a);
        return i >= 0 && static_cast<std::uint64_t>(i) == u.get<std::uint64_t>();
    }
    return as_int(a) == as_int(b);
}

} // namespace

JsonError::JsonError(std::string msg, std::size_t pos, std::size_t lineno, std::size_t colno)
    : ValueError("JSONDecodeError",
                 std::format("{}: line {} column {} (char {})", msg, lineno, colno, pos)),
      msg_(std::move(msg)), pos_(pos), lineno_(lineno), colno_(colno) {}

std::string dumps(const Json& value, const DumpOptions& options) {
    const bool indented = options.indent.has_value();
    const std::string indent_unit(static_cast<std::size_t>(std::max(0, options.indent.value_or(0))),
                                  ' ');
    const std::string item_sep =
        !options.item_sep.empty() ? options.item_sep : (indented ? "," : ", ");
    const std::string key_sep = !options.key_sep.empty() ? options.key_sep : ": ";

    std::string out;
    std::vector<WriteFrame> stack;
    const auto newline_indent = [&](std::size_t level) {
        std::string s = "\n";
        for (std::size_t i = 0; i < level; ++i) {
            s += indent_unit;
        }
        return s;
    };
    // Writes v, or opens it as a container and leaves it on the stack.
    const auto write = [&](const Json& v) {
        if (!v.is_object() && !v.is_array()) {
            write_scalar(out, v, options.ensure_ascii);
            return;
        }
        if (v.empty()) {
            out += v.is_object() ? "{}" : "[]";
            return;
        }
        out.push_back(v.is_object() ? '{' : '[');
        stack.push_back(open_frame(v, options.sort_keys));
        if (indented) {
            out += newline_indent(stack.size());
        }
    };
    write(value);
    while (!stack.empty()) {
        WriteFrame& frame = stack.back();
        if (frame.next == frame.items.size()) {
            const bool object = frame.object;
            stack.pop_back();
            if (indented) {
                out += newline_indent(stack.size());
            }
            out.push_back(object ? '}' : ']');
            continue;
        }
        const auto [key, member] = frame.items[frame.next];
        if (frame.next++ > 0) {
            out += item_sep;
            if (indented) {
                out += newline_indent(stack.size());
            }
        }
        if (key != nullptr) {
            write_string(out, *key, options.ensure_ascii);
            out += key_sep;
        }
        write(*member); // may push, which invalidates frame
    }
    return out;
}

Json loads(std::string_view text) {
    return Parser(text).parse();
}

bool py_equal(const Json& a, const Json& b) {
    if (numeric(a) && numeric(b)) {
        return equal_numbers(a, b);
    }
    if (a.type() != b.type()) {
        return false;
    }
    switch (a.type()) {
    case Json::value_t::null:
        return true;
    case Json::value_t::string:
        return a.get_ref<const std::string&>() == b.get_ref<const std::string&>();
    case Json::value_t::array:
        if (a.size() != b.size()) {
            return false;
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            if (!py_equal(a[i], b[i])) {
                return false;
            }
        }
        return true;
    case Json::value_t::object:
        if (a.size() != b.size()) {
            return false;
        }
        for (const auto& [key, member] : a.items()) {
            const auto it = b.find(key);
            if (it == b.end() || !py_equal(member, *it)) {
                return false;
            }
        }
        return true;
    default:
        return a == b;
    }
}

} // namespace fv::py
