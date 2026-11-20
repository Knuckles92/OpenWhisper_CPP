#include "backup/detail/values.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

#include "pyport/errors.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::backup::detail::values {

namespace {

std::string bytes_repr(const sql::Blob& blob) {
    bool single = false;
    bool double_quote = false;
    for (const std::byte b : blob) {
        single = single || b == std::byte{'\''};
        double_quote = double_quote || b == std::byte{'"'};
    }
    const char quote = single && !double_quote ? '"' : '\'';
    std::string out = "b";
    out += quote;
    for (const std::byte b : blob) {
        const auto c = static_cast<unsigned char>(b);
        if (c == '\\' || c == static_cast<unsigned char>(quote)) {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c < 32 || c >= 127) {
            char hex[5];
            std::snprintf(hex, sizeof(hex), "\\x%02x", c);
            out += hex;
        } else {
            out += static_cast<char>(c);
        }
    }
    out += quote;
    return out;
}

} // namespace

std::string str(const sql::Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return "None";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        return std::to_string(*integer);
    }
    if (const auto* real = std::get_if<double>(&value)) {
        return py::float_repr(*real);
    }
    if (const auto* text = std::get_if<std::string>(&value)) {
        return *text;
    }
    return bytes_repr(std::get<sql::Blob>(value));
}

std::string str_or_empty(const sql::Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        return "";
    }
    if (const auto* integer = std::get_if<std::int64_t>(&value); integer && *integer == 0) {
        return "";
    }
    if (const auto* real = std::get_if<double>(&value); real && *real == 0.0) {
        return "";
    }
    if (const auto* blob = std::get_if<sql::Blob>(&value); blob && blob->empty()) {
        return "";
    }
    return str(value);
}

int to_int(const sql::Value& value) {
    if (std::holds_alternative<std::nullptr_t>(value)) {
        throw py::TypeError("int() argument must be a string, a bytes-like object or a real "
                            "number, not 'NoneType'");
    }
    std::int64_t result = 0;
    if (const auto* integer = std::get_if<std::int64_t>(&value)) {
        result = *integer;
    } else if (const auto* real = std::get_if<double>(&value)) {
        if (std::isnan(*real)) {
            throw py::ValueError("cannot convert float NaN to integer");
        }
        if (std::isinf(*real)) {
            throw py::OverflowError("cannot convert float infinity to integer");
        }
        result = static_cast<std::int64_t>(std::trunc(*real));
    } else {
        std::string text;
        if (const auto* string = std::get_if<std::string>(&value)) {
            text = *string;
        } else {
            const auto& blob = std::get<sql::Blob>(value);
            text.assign(reinterpret_cast<const char*>(blob.data()), blob.size());
        }
        const auto parsed = py::py_int_checked(text);
        if (!parsed) {
            throw py::ValueError("invalid literal for int() with base 10: " + py::repr(text));
        }
        result = *parsed;
    }
    // The schema version is compared with small numbers; anything beyond int is beyond them.
    if (result > std::numeric_limits<int>::max()) {
        return std::numeric_limits<int>::max();
    }
    if (result < std::numeric_limits<int>::min()) {
        return std::numeric_limits<int>::min();
    }
    return static_cast<int>(result);
}

sql::Value from_json(const Json& value, int index) {
    if (value.is_null()) {
        return nullptr;
    }
    if (value.is_boolean()) {
        return std::int64_t{value.get<bool>() ? 1 : 0};
    }
    if (value.is_number_unsigned()) {
        const auto number = value.get<std::uint64_t>();
        if (number > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
            throw py::OverflowError("Python int too large to convert to SQLite INTEGER");
        }
        return static_cast<std::int64_t>(number);
    }
    if (value.is_number_integer()) {
        return value.get<std::int64_t>();
    }
    if (value.is_number_float()) {
        return value.get<double>();
    }
    if (value.is_string()) {
        return value.get<std::string>();
    }
    throw std::runtime_error("Error binding parameter " + std::to_string(index) + ": type '" +
                             (value.is_array() ? "list" : "dict") + "' is not supported");
}

std::string fold_case(std::string_view text) {
#ifdef _WIN32
    return py::lower(text);
#else
    return std::string(text);
#endif
}

} // namespace fv::backup::detail::values
