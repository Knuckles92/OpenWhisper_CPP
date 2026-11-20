#include "agentapi/detail/pydantic.h"

#include <cmath>
#include <cstdlib>
#include <format>
#include <limits>

#include "pyport/number.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

namespace fv::agentapi::detail {

namespace {

constexpr std::size_t kReprLimit = 50; // code points before pydantic shortens an input's repr
constexpr std::size_t kReprHead = 25;
constexpr std::size_t kReprTail = 24;
constexpr int kMaxDepth = 200; // jiter's recursion limit

std::string truncated_repr(const Json& value) {
    const std::string text = py_repr(value);
    const std::u32string points = py::to_code_points(text);
    if (points.size() <= kReprLimit) {
        return text;
    }
    return py::to_utf8(points.substr(0, kReprHead)) + "..." +
           py::to_utf8(points.substr(points.size() - kReprTail));
}

bool is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A jiter-style reader over the raw body. Errors carry the byte index jiter reports them at.
class Reader {
public:
    explicit Reader(std::string_view data) : data_(data) {}

    struct Failure {
        std::string message;
        std::ptrdiff_t index;
    };

    std::expected<Json, Failure> document() {
        skip_space();
        auto value = parse_value(0);
        if (!value) {
            return value;
        }
        skip_space();
        if (pos_ < data_.size()) {
            return std::unexpected(Failure{"trailing characters", at()});
        }
        return value;
    }

private:
    std::ptrdiff_t at() const { return static_cast<std::ptrdiff_t>(pos_); }
    std::ptrdiff_t eof() const { return static_cast<std::ptrdiff_t>(data_.size()) - 1; }
    bool done() const { return pos_ >= data_.size(); }
    char peek() const { return data_[pos_]; }

    void skip_space() {
        while (!done() && is_space(peek())) {
            ++pos_;
        }
    }

    std::expected<Json, Failure> fail(std::string message, std::ptrdiff_t index) {
        return std::unexpected(Failure{std::move(message), index});
    }

    std::expected<Json, Failure> literal(std::string_view word, Json value) {
        for (const char expected : word) {
            if (done()) {
                return fail("EOF while parsing a value", eof());
            }
            if (peek() != expected) {
                return fail("expected ident", at());
            }
            ++pos_;
        }
        return value;
    }

    std::expected<Json, Failure> parse_value(int depth) {
        if (done()) {
            return fail("EOF while parsing a value", eof());
        }
        switch (peek()) {
        case '{':
            return parse_object(depth + 1);
        case '[':
            return parse_array(depth + 1);
        case '"': {
            auto text = parse_string();
            if (!text) {
                return std::unexpected(text.error());
            }
            return Json(std::move(*text));
        }
        case 't':
            return literal("true", true);
        case 'f':
            return literal("false", false);
        case 'n':
            return literal("null", nullptr);
        case 'N':
            return literal("NaN", std::numeric_limits<double>::quiet_NaN());
        case 'I':
            return literal("Infinity", std::numeric_limits<double>::infinity());
        default:
            if (peek() == '-' || is_digit(peek())) {
                return parse_number();
            }
            return fail("expected value", at());
        }
    }

    std::expected<Json, Failure> parse_object(int depth) {
        if (depth > kMaxDepth) {
            return fail("recursion limit exceeded", at());
        }
        ++pos_;
        Json object = Json::object();
        skip_space();
        if (done()) {
            return fail("EOF while parsing an object", eof());
        }
        if (peek() == '}') {
            ++pos_;
            return object;
        }
        while (true) {
            if (done()) {
                return fail("EOF while parsing an object", eof());
            }
            if (peek() != '"') {
                return fail("key must be a string", at());
            }
            auto key = parse_string();
            if (!key) {
                return std::unexpected(key.error());
            }
            skip_space();
            if (done()) {
                return fail("EOF while parsing an object", eof());
            }
            if (peek() != ':') {
                return fail("expected `:`", at());
            }
            ++pos_;
            skip_space();
            auto value = parse_value(depth);
            if (!value) {
                return value;
            }
            // A repeated key keeps its first position and its last value, as a dict does.
            object[*key] = std::move(*value);
            skip_space();
            if (done()) {
                return fail("EOF while parsing an object", eof());
            }
            if (peek() == '}') {
                ++pos_;
                return object;
            }
            if (peek() != ',') {
                return fail("expected `,` or `}`", at());
            }
            ++pos_;
            skip_space();
            if (!done() && peek() == '}') {
                return fail("trailing comma", at());
            }
        }
    }

    std::expected<Json, Failure> parse_array(int depth) {
        if (depth > kMaxDepth) {
            return fail("recursion limit exceeded", at());
        }
        ++pos_;
        Json array = Json::array();
        skip_space();
        if (done()) {
            return fail("EOF while parsing a list", eof());
        }
        if (peek() == ']') {
            ++pos_;
            return array;
        }
        while (true) {
            auto value = parse_value(depth);
            if (!value) {
                return value;
            }
            array.push_back(std::move(*value));
            skip_space();
            if (done()) {
                return fail("EOF while parsing a list", eof());
            }
            if (peek() == ']') {
                ++pos_;
                return array;
            }
            if (peek() != ',') {
                return fail("expected `,` or `]`", at());
            }
            ++pos_;
            skip_space();
            if (!done() && peek() == ']') {
                return fail("trailing comma", at());
            }
        }
    }

    // Four hex digits at pos_; -1 when they are not there.
    int read_hex4() {
        if (pos_ + 4 > data_.size()) {
            return -1;
        }
        int value = 0;
        for (int i = 0; i < 4; ++i) {
            const int digit = hex_value(data_[pos_ + static_cast<std::size_t>(i)]);
            if (digit < 0) {
                return -1;
            }
            value = value * 16 + digit;
        }
        pos_ += 4;
        return value;
    }

    std::expected<std::string, Failure> parse_string() {
        ++pos_; // the opening quote
        std::string out;
        while (true) {
            if (done()) {
                return std::unexpected(Failure{"EOF while parsing a string", eof()});
            }
            const char c = peek();
            if (c == '"') {
                ++pos_;
                return out;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::unexpected(Failure{
                    "control character (\\u0000-\\u001F) found while parsing a string", at()});
            }
            if (c != '\\') {
                const std::size_t start = pos_;
                const char32_t point = py::next_code_point(data_, pos_);
                if (point == 0xFFFD && data_.substr(start, 3) != "\xEF\xBF\xBD") {
                    return std::unexpected(
                        Failure{"invalid unicode code point", static_cast<std::ptrdiff_t>(start)});
                }
                out.append(data_.substr(start, pos_ - start));
                continue;
            }
            ++pos_;
            if (done()) {
                return std::unexpected(Failure{"EOF while parsing a string", eof()});
            }
            const char escape = peek();
            ++pos_;
            switch (escape) {
            case '"':
                out += '"';
                break;
            case '\\':
                out += '\\';
                break;
            case '/':
                out += '/';
                break;
            case 'b':
                out += '\b';
                break;
            case 'f':
                out += '\f';
                break;
            case 'n':
                out += '\n';
                break;
            case 'r':
                out += '\r';
                break;
            case 't':
                out += '\t';
                break;
            case 'u': {
                const int unit = read_hex4();
                if (unit < 0) {
                    if (pos_ + 4 > data_.size()) {
                        return std::unexpected(Failure{"EOF while parsing a string", eof()});
                    }
                    return std::unexpected(Failure{"invalid escape", at()});
                }
                if (unit >= 0xDC00 && unit <= 0xDFFF) {
                    return std::unexpected(
                        Failure{"lone leading surrogate in hex escape", at() - 1});
                }
                if (unit >= 0xD800 && unit <= 0xDBFF) {
                    if (pos_ + 2 > data_.size() || data_[pos_] != '\\' || data_[pos_ + 1] != 'u') {
                        return std::unexpected(Failure{"unexpected end of hex escape", at()});
                    }
                    pos_ += 2;
                    const int low = read_hex4();
                    if (low < 0xDC00 || low > 0xDFFF) {
                        return std::unexpected(Failure{"invalid unicode code point", at() - 1});
                    }
                    py::append_utf8(out, 0x10000 + ((static_cast<char32_t>(unit) - 0xD800) << 10) +
                                             (static_cast<char32_t>(low) - 0xDC00));
                } else {
                    py::append_utf8(out, static_cast<char32_t>(unit));
                }
                break;
            }
            default:
                return std::unexpected(Failure{"invalid escape", at() - 1});
            }
        }
    }

    std::expected<Json, Failure> parse_number() {
        const std::size_t start = pos_;
        bool integer = true;
        if (peek() == '-') {
            ++pos_;
            if (!done() && peek() == 'I') {
                auto value = literal("Infinity", -std::numeric_limits<double>::infinity());
                return value;
            }
            if (done()) {
                return fail("EOF while parsing a value", eof());
            }
        }
        if (!is_digit(peek())) {
            return fail("invalid number", at());
        }
        if (peek() == '0') {
            ++pos_;
            if (!done() && is_digit(peek())) {
                return fail("invalid number", at());
            }
        } else {
            while (!done() && is_digit(peek())) {
                ++pos_;
            }
        }
        if (!done() && peek() == '.') {
            integer = false;
            ++pos_;
            if (done() || !is_digit(peek())) {
                return done() ? fail("EOF while parsing a value", eof())
                              : fail("invalid number", at());
            }
            while (!done() && is_digit(peek())) {
                ++pos_;
            }
        }
        if (!done() && (peek() == 'e' || peek() == 'E')) {
            integer = false;
            ++pos_;
            if (!done() && (peek() == '+' || peek() == '-')) {
                ++pos_;
            }
            if (done() || !is_digit(peek())) {
                return done() ? fail("EOF while parsing a value", eof())
                              : fail("invalid number", at());
            }
            while (!done() && is_digit(peek())) {
                ++pos_;
            }
        }
        const std::string text(data_.substr(start, pos_ - start));
        if (integer) {
            errno = 0;
            char* end = nullptr;
            const long long value = std::strtoll(text.c_str(), &end, 10);
            if (errno == 0) {
                return Json(static_cast<std::int64_t>(value));
            }
            errno = 0;
            const unsigned long long big = std::strtoull(text.c_str(), &end, 10);
            if (errno == 0 && text[0] != '-') {
                return Json(static_cast<std::uint64_t>(big));
            }
        }
        return Json(std::strtod(text.c_str(), nullptr));
    }

    std::string_view data_;
    std::size_t pos_ = 0;
};

std::string position(std::string_view data, std::ptrdiff_t index) {
    std::size_t line = 1;
    std::ptrdiff_t last_newline = -1;
    for (std::ptrdiff_t i = 0; i <= index && i < static_cast<std::ptrdiff_t>(data.size()); ++i) {
        if (data[static_cast<std::size_t>(i)] == '\n') {
            ++line;
            last_newline = i;
        }
    }
    const std::ptrdiff_t column = index < 0 ? 0 : index - last_newline;
    return std::format("line {} column {}", line, column);
}

std::string ascii_strip(std::string_view text) {
    std::size_t begin = 0;
    std::size_t end = text.size();
    const auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (begin < end && space(text[begin])) {
        ++begin;
    }
    while (end > begin && space(text[end - 1])) {
        --end;
    }
    return std::string(text.substr(begin, end - begin));
}

Refusal int_parsing() {
    return {"int_parsing", "Input should be a valid integer, unable to parse string as an integer"};
}

Refusal int_size() {
    return {"int_parsing_size",
            "Unable to parse input string as an integer, exceeded maximum size"};
}

constexpr double kTwoTo63 = 9223372036854775808.0;

std::expected<std::int64_t, Refusal> int_from_text(std::string_view raw) {
    const std::string text = ascii_strip(raw);
    std::size_t pos = 0;
    bool negative = false;
    if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
        negative = text[pos] == '-';
        ++pos;
    }
    std::string digits;
    bool last_digit = false;
    for (; pos < text.size() && text[pos] != '.'; ++pos) {
        const char c = text[pos];
        if (is_digit(c)) {
            digits += c;
            last_digit = true;
        } else if (c == '_' && last_digit && pos + 1 < text.size() && is_digit(text[pos + 1])) {
            last_digit = false;
        } else {
            return std::unexpected(int_parsing());
        }
    }
    if (digits.empty()) {
        return std::unexpected(int_parsing());
    }
    if (pos < text.size()) {
        // "5.0": a fraction of zeros only.
        ++pos;
        if (pos == text.size()) {
            return std::unexpected(int_parsing());
        }
        for (; pos < text.size(); ++pos) {
            if (text[pos] != '0') {
                return std::unexpected(int_parsing());
            }
        }
    }
    errno = 0;
    char* end = nullptr;
    const long long value = std::strtoll(((negative ? "-" : "") + digits).c_str(), &end, 10);
    if (errno != 0) {
        return std::unexpected(int_size());
    }
    return static_cast<std::int64_t>(value);
}

// The proleptic Gregorian date `z` days after 1970-01-01.
void civil_from_days(std::int64_t z, int& y, int& m, int& d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const auto doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t year = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = static_cast<int>(doy - (153 * mp + 2) / 5 + 1);
    m = static_cast<int>(mp < 10 ? mp + 3 : mp - 9);
    y = static_cast<int>(year + (m <= 2 ? 1 : 0));
}

bool leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

int days_in_month(int year, int month) {
    static constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return month == 2 && leap(year) ? 29 : days[month - 1];
}

std::optional<int> two_digits(std::string_view text, std::size_t at) {
    if (at + 2 > text.size() || !is_digit(text[at]) || !is_digit(text[at + 1])) {
        return std::nullopt;
    }
    return (text[at] - '0') * 10 + (text[at + 1] - '0');
}

// speedate's Date::parse_str over the first ten bytes; `whole` also refuses anything after them.
std::expected<py::Date, std::string> parse_date(std::string_view text, bool whole) {
    if (text.size() < 10) {
        return std::unexpected("input is too short");
    }
    int year = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        if (!is_digit(text[i])) {
            return std::unexpected("invalid character in year");
        }
        year = year * 10 + (text[i] - '0');
    }
    if (text[4] != '-') {
        return std::unexpected("invalid date separator, expected `-`");
    }
    const auto month = two_digits(text, 5);
    if (!month) {
        return std::unexpected("invalid character in month");
    }
    if (text[7] != '-') {
        return std::unexpected("invalid date separator, expected `-`");
    }
    const auto day = two_digits(text, 8);
    if (!day) {
        return std::unexpected("invalid character in day");
    }
    if (*month < 1 || *month > 12) {
        return std::unexpected("month value is outside expected range of 1-12");
    }
    if (*day < 1 || *day > days_in_month(year, *month)) {
        return std::unexpected("day value is outside expected range");
    }
    if (whole && text.size() > 10) {
        return std::unexpected("unexpected extra characters at the end of the input");
    }
    return py::Date{year, *month, *day};
}

// The time and offset after a date and its separator; nullopt for anything speedate refuses.
std::optional<py::DateTime> parse_time(const py::Date& date, std::string_view text) {
    py::DateTime out;
    out.year = date.year;
    out.month = date.month;
    out.day = date.day;
    std::size_t pos = 0;
    const auto hour = two_digits(text, pos);
    if (!hour || *hour > 23 || pos + 2 >= text.size() || text[pos + 2] != ':') {
        return std::nullopt;
    }
    pos += 3;
    const auto minute = two_digits(text, pos);
    if (!minute || *minute > 59) {
        return std::nullopt;
    }
    pos += 2;
    out.hour = *hour;
    out.minute = *minute;
    if (pos < text.size() && text[pos] == ':') {
        ++pos;
        const auto second = two_digits(text, pos);
        if (!second || *second > 59) {
            return std::nullopt;
        }
        out.second = *second;
        pos += 2;
        if (pos < text.size() && (text[pos] == '.' || text[pos] == ',')) {
            ++pos;
            int digits = 0;
            int micro = 0;
            while (pos < text.size() && is_digit(text[pos])) {
                if (digits < 6) {
                    micro = micro * 10 + (text[pos] - '0');
                }
                ++digits;
                ++pos;
            }
            if (digits == 0 || digits > 9) {
                return std::nullopt;
            }
            for (int i = digits; i < 6; ++i) {
                micro *= 10;
            }
            out.microsecond = micro;
        }
    }
    if (pos == text.size()) {
        return out;
    }
    if (text[pos] == 'Z' || text[pos] == 'z') {
        if (pos + 1 != text.size()) {
            return std::nullopt;
        }
        out.utc_offset_us = 0;
        return out;
    }
    if (text[pos] != '+' && text[pos] != '-') {
        return std::nullopt;
    }
    const int sign = text[pos] == '-' ? -1 : 1;
    ++pos;
    const auto offset_hour = two_digits(text, pos);
    if (!offset_hour || *offset_hour > 23) {
        return std::nullopt;
    }
    pos += 2;
    if (pos < text.size() && text[pos] == ':') {
        ++pos;
    }
    const auto offset_minute = two_digits(text, pos);
    if (!offset_minute || *offset_minute > 59 || pos + 2 != text.size()) {
        return std::nullopt;
    }
    out.utc_offset_us =
        sign * (static_cast<std::int64_t>(*offset_hour) * 3600 + *offset_minute * 60) * 1'000'000;
    return out;
}

std::optional<py::DateTime> from_timestamp(double seconds) {
    if (!std::isfinite(seconds)) {
        return std::nullopt;
    }
    // speedate reads a value above 2e10 as milliseconds.
    if (std::fabs(seconds) > 2e10) {
        seconds /= 1000.0;
    }
    const double whole = std::floor(seconds);
    auto micro = static_cast<std::int64_t>(std::llround((seconds - whole) * 1e6));
    auto total = static_cast<std::int64_t>(whole);
    if (micro >= 1'000'000) {
        micro -= 1'000'000;
        ++total;
    }
    std::int64_t days = total / 86400;
    std::int64_t rest = total % 86400;
    if (rest < 0) {
        rest += 86400;
        --days;
    }
    py::DateTime out;
    civil_from_days(days, out.year, out.month, out.day);
    out.hour = static_cast<int>(rest / 3600);
    out.minute = static_cast<int>(rest % 3600 / 60);
    out.second = static_cast<int>(rest % 60);
    out.microsecond = static_cast<int>(micro);
    out.utc_offset_us = 0;
    return out;
}

bool numeric_text(std::string_view text) {
    std::size_t pos = 0;
    if (pos < text.size() && text[pos] == '-') {
        ++pos;
    }
    const std::size_t digits_start = pos;
    while (pos < text.size() && is_digit(text[pos])) {
        ++pos;
    }
    if (pos == digits_start) {
        return false;
    }
    if (pos < text.size() && text[pos] == '.') {
        ++pos;
        const std::size_t fraction = pos;
        while (pos < text.size() && is_digit(text[pos])) {
            ++pos;
        }
        if (pos == fraction) {
            return false;
        }
    }
    return pos == text.size();
}

Refusal datetime_type() {
    return {"datetime_type", "Input should be a valid datetime"};
}

} // namespace

std::string py_repr(const Json& value) {
    switch (value.type()) {
    case Json::value_t::null:
        return "None";
    case Json::value_t::boolean:
        return value.get<bool>() ? "True" : "False";
    case Json::value_t::number_integer:
        return std::to_string(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
        return std::to_string(value.get<std::uint64_t>());
    case Json::value_t::number_float:
        return py::float_repr(value.get<double>());
    case Json::value_t::string:
        return py::repr(value.get_ref<const std::string&>());
    case Json::value_t::array: {
        std::string out = "[";
        bool first = true;
        for (const Json& item : value) {
            out += first ? "" : ", ";
            out += py_repr(item);
            first = false;
        }
        return out + "]";
    }
    case Json::value_t::object: {
        std::string out = "{";
        bool first = true;
        for (const auto& [key, item] : value.items()) {
            out += first ? "" : ", ";
            out += py::repr(key) + ": " + py_repr(item);
            first = false;
        }
        return out + "}";
    }
    default:
        return "None";
    }
}

std::string_view py_type_name(const Json& value) {
    switch (value.type()) {
    case Json::value_t::boolean:
        return "bool";
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned:
        return "int";
    case Json::value_t::number_float:
        return "float";
    case Json::value_t::string:
        return "str";
    case Json::value_t::array:
        return "list";
    case Json::value_t::object:
        return "dict";
    default:
        return "NoneType";
    }
}

bool has_surrogate(std::string_view utf8) {
    for (std::size_t i = 0; i + 1 < utf8.size(); ++i) {
        if (static_cast<unsigned char>(utf8[i]) == 0xED &&
            (static_cast<unsigned char>(utf8[i + 1]) & 0xE0) == 0xA0) {
            return true;
        }
    }
    return false;
}

std::string format_validation_error(std::string_view title, const std::vector<LineError>& errors) {
    std::string out = std::format("{} validation error{} for {}", errors.size(),
                                  errors.size() == 1 ? "" : "s", title);
    for (const LineError& error : errors) {
        out += "\n" + error.loc;
        out += std::format("\n  {} [type={}, input_value={}, input_type={}]", error.message,
                           error.type, truncated_repr(error.input), py_type_name(error.input));
        out +=
            "\n    For further information visit https://errors.pydantic.dev/2.13/v/" + error.type;
    }
    return out;
}

std::expected<Json, std::string> from_json(std::string_view body) {
    Reader reader(body);
    auto value = reader.document();
    if (!value) {
        return std::unexpected(value.error().message + " at " +
                               position(body, value.error().index));
    }
    return std::move(*value);
}

std::expected<std::int64_t, Refusal> lax_int(const Json& value) {
    switch (value.type()) {
    case Json::value_t::boolean:
        return value.get<bool>() ? 1 : 0;
    case Json::value_t::number_integer:
        return value.get<std::int64_t>();
    case Json::value_t::number_unsigned:
        return std::unexpected(int_size());
    case Json::value_t::number_float: {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            return std::unexpected(Refusal{"finite_number", "Input should be a finite number"});
        }
        if (number != std::trunc(number)) {
            return std::unexpected(
                Refusal{"int_from_float",
                        "Input should be a valid integer, got a number with a fractional part"});
        }
        if (number >= kTwoTo63 || number < -kTwoTo63) {
            return std::unexpected(int_size());
        }
        return static_cast<std::int64_t>(number);
    }
    case Json::value_t::string:
        return int_from_text(value.get_ref<const std::string&>());
    default:
        return std::unexpected(Refusal{"int_type", "Input should be a valid integer"});
    }
}

std::expected<double, Refusal> lax_float(const Json& value) {
    const Refusal finite{"finite_number", "Input should be a finite number"};
    switch (value.type()) {
    case Json::value_t::boolean:
        return value.get<bool>() ? 1.0 : 0.0;
    case Json::value_t::number_integer:
        return static_cast<double>(value.get<std::int64_t>());
    case Json::value_t::number_unsigned:
        return static_cast<double>(value.get<std::uint64_t>());
    case Json::value_t::number_float: {
        const double number = value.get<double>();
        if (!std::isfinite(number)) {
            return std::unexpected(finite);
        }
        return number;
    }
    case Json::value_t::string: {
        const std::string text = ascii_strip(value.get_ref<const std::string&>());
        const auto parsed =
            text.find('_') == std::string::npos ? py::py_float(text) : std::optional<double>();
        if (!parsed) {
            return std::unexpected(
                Refusal{"float_parsing",
                        "Input should be a valid number, unable to parse string as a number"});
        }
        if (!std::isfinite(*parsed)) {
            return std::unexpected(finite);
        }
        return *parsed;
    }
    default:
        return std::unexpected(Refusal{"float_type", "Input should be a valid number"});
    }
}

std::expected<bool, Refusal> lax_bool(const Json& value) {
    const Refusal parsing{"bool_parsing",
                          "Input should be a valid boolean, unable to interpret input"};
    const Refusal type{"bool_type", "Input should be a valid boolean"};
    switch (value.type()) {
    case Json::value_t::boolean:
        return value.get<bool>();
    case Json::value_t::number_integer:
    case Json::value_t::number_unsigned: {
        const Json& number = value;
        if (number == 0) {
            return false;
        }
        if (number == 1) {
            return true;
        }
        return std::unexpected(parsing);
    }
    case Json::value_t::number_float: {
        const double number = value.get<double>();
        if (number == 0.0) {
            return false;
        }
        if (number == 1.0) {
            return true;
        }
        return std::unexpected(type);
    }
    case Json::value_t::string: {
        std::string text = value.get<std::string>();
        for (char& c : text) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        for (const std::string_view yes : {"1", "on", "t", "true", "y", "yes"}) {
            if (text == yes) {
                return true;
            }
        }
        for (const std::string_view no : {"0", "off", "f", "false", "n", "no"}) {
            if (text == no) {
                return false;
            }
        }
        return std::unexpected(parsing);
    }
    default:
        return std::unexpected(type);
    }
}

std::expected<std::string, Refusal> lax_str(const Json& value) {
    if (!value.is_string()) {
        return std::unexpected(Refusal{"string_type", "Input should be a valid string"});
    }
    const std::string& text = value.get_ref<const std::string&>();
    if (has_surrogate(text)) {
        return std::unexpected(Refusal{
            "string_unicode",
            "Input should be a valid string, unable to parse raw data as a unicode string"});
    }
    return text;
}

std::expected<py::DateTime, Refusal> lax_datetime(const Json& value) {
    if (value.is_number_integer()) {
        if (auto stamp = from_timestamp(static_cast<double>(value.get<std::int64_t>()))) {
            return *stamp;
        }
        return std::unexpected(datetime_type());
    }
    if (value.is_number_float()) {
        if (auto stamp = from_timestamp(value.get<double>())) {
            return *stamp;
        }
        return std::unexpected(datetime_type());
    }
    if (!value.is_string()) {
        return std::unexpected(datetime_type());
    }
    const std::string& text = value.get_ref<const std::string&>();
    if (numeric_text(text)) {
        if (auto stamp = from_timestamp(std::strtod(text.c_str(), nullptr))) {
            return *stamp;
        }
    }
    const auto date = parse_date(text, false);
    if (date && text.size() > 11) {
        const char separator = text[10];
        if (separator == 'T' || separator == 't' || separator == '_' || separator == ' ') {
            if (auto parsed = parse_time(*date, std::string_view(text).substr(11))) {
                return *parsed;
            }
        }
    }
    // A failed datetime is read again as a date, and the date's error is the one reported.
    const auto whole = parse_date(text, true);
    if (whole) {
        py::DateTime midnight;
        midnight.year = whole->year;
        midnight.month = whole->month;
        midnight.day = whole->day;
        return midnight;
    }
    return std::unexpected(Refusal{"datetime_from_date_parsing",
                                   "Input should be a valid datetime or date, " + whole.error()});
}

} // namespace fv::agentapi::detail
