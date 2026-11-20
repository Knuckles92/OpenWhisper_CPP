#include "pyport/number.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <vector>

#include "pyport/errors.h"
#include "pyport/unicode.h"

namespace fv::py {
namespace {

// Python's limit on decimal digits in int(str) (sys.get_int_max_str_digits()).
constexpr std::size_t kMaxIntDigits = 4300;

// round(x, n) leaves x alone above this many digits and gives a signed zero below the other
// bound; both come from Python's float.__round__ (log10(2) bounded by 0.30103).
constexpr int kNdigitsMax = static_cast<int>((53 - (-1021)) * 0.30103);
constexpr int kNdigitsMin = -static_cast<int>((1024 + 1) * 0.30103);

// Python's _PyUnicode_TransformDecimalAndSpaceToASCII: below U+007F a code point is kept as it
// is, above it Unicode whitespace becomes ' ', a decimal digit its ASCII digit and anything else
// '?', so the ASCII parsers below see what CPython's do. The parsers then strip only ASCII
// whitespace, so U+001C to U+001F (str.isspace, but ASCII) never count as space: "\x1c7" and
// "\x1c7" followed by U+00A0 are both invalid, while U+0085 and U+00A0 around "7" are not.
std::string to_ascii_number(std::string_view text) {
    if (std::all_of(text.begin(), text.end(),
                    [](char c) { return static_cast<unsigned char>(c) < 0x80; })) {
        return std::string(text);
    }
    std::string out;
    out.reserve(text.size());
    for (std::size_t pos = 0; pos < text.size();) {
        const char32_t cp = next_code_point(text, pos);
        if (cp < 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (is_space(cp)) {
            out.push_back(' ');
        } else if (const auto digit = decimal_value(cp)) {
            out.push_back(static_cast<char>('0' + *digit));
        } else {
            out.push_back('?');
        }
    }
    return out;
}

bool ascii_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r';
}

bool ascii_digit(char c) {
    return c >= '0' && c <= '9';
}

std::string_view trim_ascii(std::string_view s) {
    while (!s.empty() && ascii_space(s.front())) {
        s.remove_prefix(1);
    }
    while (!s.empty() && ascii_space(s.back())) {
        s.remove_suffix(1);
    }
    return s;
}

bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.size(); ++i) {
        const char x = (a[i] >= 'A' && a[i] <= 'Z') ? static_cast<char>(a[i] + 32) : a[i];
        if (x != b[i]) {
            return false;
        }
    }
    return true;
}

// Decimal digits as text, a decimal point after `point` of them, no sign: rounds half to even
// so that `ndigits` digits after the point remain, and returns the kept digits and the power
// of ten they are scaled by.
std::string round_decimal(std::string digits, std::size_t point, int ndigits, int& exponent) {
    const long long keep = static_cast<long long>(point) + ndigits;
    exponent = -ndigits;
    if (keep >= static_cast<long long>(digits.size())) {
        digits.append(static_cast<std::size_t>(keep) - digits.size(), '0');
        return digits;
    }
    if (keep < 0) {
        return "0"; // even the first digit is below half a unit of the kept place
    }
    const auto k = static_cast<std::size_t>(keep);
    std::string kept = digits.substr(0, k);
    const char next = digits[k];
    bool up = next > '5';
    if (next == '5') {
        const bool beyond = digits.find_first_not_of('0', k + 1) != std::string::npos;
        const bool odd = !kept.empty() && ((kept.back() - '0') % 2 == 1);
        up = beyond || odd;
    }
    if (up) {
        std::size_t i = kept.size();
        while (i > 0 && kept[i - 1] == '9') {
            kept[i - 1] = '0';
            --i;
        }
        if (i == 0) {
            kept.insert(kept.begin(), '1');
        } else {
            ++kept[i - 1];
        }
    }
    return kept.empty() ? std::string("0") : kept;
}

} // namespace

double round_half_even(double x) {
    if (!std::isfinite(x)) {
        return x;
    }
    // x - floor(x) is exact for every double, and every |x| >= 2**52 is already integral.
    const double floor = std::floor(x);
    const double diff = x - floor;
    double rounded = floor;
    if (diff > 0.5 || (diff == 0.5 && std::fmod(floor, 2.0) != 0.0)) {
        rounded = floor + 1.0;
    }
    return std::copysign(rounded, x);
}

double round_ndigits(double x, int ndigits) {
    if (!std::isfinite(x) || ndigits > kNdigitsMax) {
        return x;
    }
    if (ndigits < kNdigitsMin) {
        return 0.0 * x;
    }
    // The exact decimal expansion of a double has at most 1074 digits after the point.
    std::vector<char> buffer(400 + 1074);
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), std::fabs(x),
                                         std::chars_format::fixed, 1074);
    if (ec != std::errc()) {
        throw OverflowError("rounded value too large to represent");
    }
    const std::string_view exact(buffer.data(), static_cast<std::size_t>(end - buffer.data()));
    const auto dot = exact.find('.');
    std::string digits(exact.substr(0, dot));
    digits.append(exact.substr(dot + 1));
    int exponent = 0;
    std::string text = round_decimal(std::move(digits), dot, ndigits, exponent);
    text = (std::signbit(x) ? "-" : "") + text + "e" + std::to_string(exponent);
    double rounded = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), rounded);
    if (result.ec == std::errc::result_out_of_range) {
        throw OverflowError("rounded value too large to represent");
    }
    return rounded;
}

std::string format_fixed(double x, int precision) {
    if (std::isnan(x)) {
        return "nan";
    }
    if (std::isinf(x)) {
        return x > 0 ? "inf" : "-inf";
    }
    if (precision < 0) {
        precision = 0;
    }
    std::vector<char> buffer(400 + static_cast<std::size_t>(precision));
    const auto [end, ec] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), x,
                                         std::chars_format::fixed, precision);
    return ec == std::errc() ? std::string(buffer.data(), end) : std::string();
}

std::string float_repr(double x) {
    if (std::isnan(x)) {
        return "nan";
    }
    if (std::isinf(x)) {
        return x > 0 ? "inf" : "-inf";
    }
    char buffer[64];
    const auto [end, ec] =
        std::to_chars(buffer, buffer + sizeof buffer, std::fabs(x), std::chars_format::scientific);
    const std::string_view sci(buffer, static_cast<std::size_t>(end - buffer));
    const auto e = sci.find('e');
    std::string digits(sci.substr(0, e));
    if (digits.size() > 1) {
        digits.erase(1, 1); // the '.'
    }
    int exp10 = 0;
    const auto exp_text = sci.substr(e + 1);
    std::from_chars(exp_text.data() + (exp_text.front() == '+' ? 1 : 0),
                    exp_text.data() + exp_text.size(), exp10);
    // Python's repr: fixed notation when the decimal point falls within -4 < decpt <= 16.
    const int decpt = exp10 + 1;
    std::string out = std::signbit(x) ? "-" : "";
    const int ndigits = static_cast<int>(digits.size());
    if (decpt > -4 && decpt <= 16) {
        if (decpt <= 0) {
            out += "0." + std::string(static_cast<std::size_t>(-decpt), '0') + digits;
        } else if (decpt >= ndigits) {
            out += digits + std::string(static_cast<std::size_t>(decpt - ndigits), '0') + ".0";
        } else {
            out += digits.substr(0, static_cast<std::size_t>(decpt)) + "." +
                   digits.substr(static_cast<std::size_t>(decpt));
        }
        return out;
    }
    out += digits.substr(0, 1);
    if (ndigits > 1) {
        out += "." + digits.substr(1);
    }
    const int shown = decpt - 1;
    out += shown < 0 ? "e-" : "e+";
    const std::string magnitude = std::to_string(shown < 0 ? -shown : shown);
    if (magnitude.size() < 2) {
        out += '0';
    }
    out += magnitude;
    return out;
}

std::expected<std::int64_t, IntError> py_int_checked(std::string_view text) {
    const std::string ascii = to_ascii_number(text);
    std::string_view s = trim_ascii(ascii);
    bool negative = false;
    if (!s.empty() && (s.front() == '+' || s.front() == '-')) {
        negative = s.front() == '-';
        s.remove_prefix(1);
    }
    if (s.empty() || !ascii_digit(s.front()) || !ascii_digit(s.back())) {
        return std::unexpected(IntError::Value);
    }
    std::string digits;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '_') {
            if (!ascii_digit(s[i - 1]) || !ascii_digit(s[i + 1])) {
                return std::unexpected(IntError::Value);
            }
        } else if (ascii_digit(s[i])) {
            digits.push_back(s[i]);
        } else {
            return std::unexpected(IntError::Value);
        }
    }
    if (digits.size() > kMaxIntDigits) {
        return std::unexpected(IntError::Value);
    }
    // Accumulate negatively so that INT64_MIN, whose magnitude int64 cannot hold, still fits.
    std::int64_t value = 0;
    constexpr std::int64_t kMin = std::numeric_limits<std::int64_t>::min();
    for (const char c : digits) {
        const int d = c - '0';
        if (value < (kMin + d) / 10) {
            return std::unexpected(IntError::Overflow);
        }
        value = value * 10 - d;
    }
    if (!negative) {
        if (value == kMin) {
            return std::unexpected(IntError::Overflow);
        }
        value = -value;
    }
    return value;
}

std::optional<std::int64_t> py_int(std::string_view text) {
    const auto result = py_int_checked(text);
    return result ? std::optional<std::int64_t>(*result) : std::nullopt;
}

std::expected<std::int64_t, IntError> py_int_checked(const Json& value, bool strict) {
    switch (value.type()) {
    case Json::value_t::number_integer:
        return value.get<std::int64_t>();
    case Json::value_t::number_unsigned: {
        const auto u = value.get<std::uint64_t>();
        if (u > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            return std::unexpected(IntError::Overflow);
        }
        return static_cast<std::int64_t>(u);
    }
    default:
        break;
    }
    if (strict) {
        return std::unexpected(IntError::Type);
    }
    switch (value.type()) {
    case Json::value_t::boolean:
        return value.get<bool>() ? 1 : 0;
    case Json::value_t::number_float: {
        const double d = value.get<double>();
        if (std::isnan(d)) {
            return std::unexpected(IntError::Value);
        }
        const double truncated = std::trunc(d);
        // 2**63 is exactly representable; every double below it in magnitude fits int64.
        if (std::isinf(d) || truncated >= 9223372036854775808.0 ||
            truncated < -9223372036854775808.0) {
            return std::unexpected(IntError::Overflow);
        }
        return static_cast<std::int64_t>(truncated);
    }
    case Json::value_t::string:
        return py_int_checked(std::string_view(value.get_ref<const std::string&>()));
    default:
        return std::unexpected(IntError::Type);
    }
}

std::optional<std::int64_t> py_int(const Json& value, bool strict) {
    const auto result = py_int_checked(value, strict);
    return result ? std::optional<std::int64_t>(*result) : std::nullopt;
}

std::optional<double> py_float(std::string_view text) {
    const std::string ascii = to_ascii_number(text);
    std::string_view s = trim_ascii(ascii);
    // Underscores only between digits (Python's _Py_string_to_number_with_underscores).
    std::string plain;
    if (s.find('_') != std::string_view::npos) {
        char prev = '\0';
        for (const char c : s) {
            if (c == '_') {
                if (!ascii_digit(prev)) {
                    return std::nullopt;
                }
            } else {
                if (prev == '_' && !ascii_digit(c)) {
                    return std::nullopt;
                }
                plain.push_back(c);
            }
            prev = c;
        }
        if (prev == '_') {
            return std::nullopt;
        }
        s = plain;
    }
    if (s.empty()) {
        return std::nullopt;
    }
    std::string_view body = s;
    const bool negative = body.front() == '-';
    if (body.front() == '+' || body.front() == '-') {
        body.remove_prefix(1);
    }
    if (iequals(body, "inf") || iequals(body, "infinity")) {
        return negative ? -std::numeric_limits<double>::infinity()
                        : std::numeric_limits<double>::infinity();
    }
    if (iequals(body, "nan")) {
        return negative ? -std::numeric_limits<double>::quiet_NaN()
                        : std::numeric_limits<double>::quiet_NaN();
    }
    // [digits][.digits][e[sign]digits] with at least one mantissa digit.
    std::size_t i = 0;
    std::size_t mantissa_digits = 0;
    long long first_nonzero = -1; // digit index of the leading nonzero digit
    long long point = -1;         // digits before the decimal point
    while (i < body.size() && (ascii_digit(body[i]) || (body[i] == '.' && point < 0))) {
        if (body[i] == '.') {
            point = static_cast<long long>(mantissa_digits);
        } else {
            if (body[i] != '0' && first_nonzero < 0) {
                first_nonzero = static_cast<long long>(mantissa_digits);
            }
            ++mantissa_digits;
        }
        ++i;
    }
    if (mantissa_digits == 0) {
        return std::nullopt;
    }
    long long exponent = 0;
    if (i < body.size() && (body[i] == 'e' || body[i] == 'E')) {
        std::size_t j = i + 1;
        bool exp_negative = false;
        if (j < body.size() && (body[j] == '+' || body[j] == '-')) {
            exp_negative = body[j] == '-';
            ++j;
        }
        const std::size_t exp_start = j;
        while (j < body.size() && ascii_digit(body[j])) {
            if (exponent < 100000000) {
                exponent = exponent * 10 + (body[j] - '0');
            }
            ++j;
        }
        if (j == exp_start) {
            return std::nullopt;
        }
        if (exp_negative) {
            exponent = -exponent;
        }
        i = j;
    }
    if (i != body.size()) {
        return std::nullopt;
    }
    double value = 0.0;
    const auto result =
        std::from_chars(body.data(), body.data() + body.size(), value, std::chars_format::general);
    if (result.ec == std::errc::result_out_of_range) {
        // The decimal exponent of the leading digit tells overflow (inf) from underflow (0).
        const long long before_point = point < 0 ? static_cast<long long>(mantissa_digits) : point;
        const long long magnitude = before_point - first_nonzero + exponent;
        value = magnitude > 0 ? std::numeric_limits<double>::infinity() : 0.0;
    } else if (result.ec != std::errc() || result.ptr != body.data() + body.size()) {
        return std::nullopt;
    }
    return negative ? -value : value;
}

std::optional<double> py_float(const Json& value) {
    switch (value.type()) {
    case Json::value_t::boolean:
        return value.get<bool>() ? 1.0 : 0.0;
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
    case Json::value_t::number_float:
        return value.get<double>();
    case Json::value_t::string:
        return py_float(std::string_view(value.get_ref<const std::string&>()));
    default:
        return std::nullopt;
    }
}

} // namespace fv::py
