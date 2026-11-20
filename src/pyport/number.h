#pragma once

#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>

#include "pyport/json.h"

namespace fv::py {

// round(x): half to even. Python returns an int and raises for inf and nan; here inf and nan
// come back unchanged and the caller decides.
double round_half_even(double x);

// round(x, ndigits): rounds the exact binary value of x half to even at 10**-ndigits, as
// Python does through a correctly rounded decimal string (round(2.675, 2) == 2.67). Throws
// OverflowError("rounded value too large to represent") as Python does.
double round_ndigits(double x, int ndigits);

// f"{x:.{precision}f}" and format(x, ".Nf"): the exact binary value rounded half to even
// (format(0.15, ".1f") == "0.1"); "inf", "-inf", "nan".
std::string format_fixed(double x, int precision);

// repr(x) and str(x) of a float: shortest round-trip digits, "1.0", "1e+16", "1e-05", "inf",
// "nan". Exponent form below 1e-4 and from 1e16.
std::string float_repr(double x);

// Why int() failed: TypeError (None, list, dict), ValueError (bad text, nan) or
// OverflowError (inf). A value Python could hold but int64 cannot is Overflow as well, so a
// caller treats it as Python's OverflowError path does.
enum class IntError { Type, Value, Overflow };

// int(value) over a JSON value with the rules of setting_types.IntSetting: bool gives 0 or 1,
// a float truncates toward zero, a string goes through int(str). strict accepts only a JSON
// integer (never a bool, a float or a string).
std::expected<std::int64_t, IntError> py_int_checked(const Json& value, bool strict = false);
std::optional<std::int64_t> py_int(const Json& value, bool strict = false);

// int(text): surrounding whitespace, one sign, '_' between digits, any Unicode Nd digit.
// More than 4300 digits is ValueError, Python's integer string conversion limit.
std::expected<std::int64_t, IntError> py_int_checked(std::string_view text);
std::optional<std::int64_t> py_int(std::string_view text);
// Text arguments would otherwise be ambiguous between the Json and string_view overloads.
inline std::expected<std::int64_t, IntError> py_int_checked(const char* text) {
    return py_int_checked(std::string_view(text));
}
inline std::expected<std::int64_t, IntError> py_int_checked(const std::string& text) {
    return py_int_checked(std::string_view(text));
}
inline std::optional<std::int64_t> py_int(const char* text) {
    return py_int(std::string_view(text));
}
inline std::optional<std::int64_t> py_int(const std::string& text) {
    return py_int(std::string_view(text));
}

// float(text): as int(text) plus a fraction, an exponent, "inf", "infinity" and "nan" in any
// case; a value beyond the double range is inf, as in Python.
std::optional<double> py_float(std::string_view text);
// float(value) over a JSON value: a bool gives 0.0 or 1.0, a number itself, a string goes
// through float(str); null, arrays and objects fail (TypeError).
std::optional<double> py_float(const Json& value);
inline std::optional<double> py_float(const char* text) {
    return py_float(std::string_view(text));
}
inline std::optional<double> py_float(const std::string& text) {
    return py_float(std::string_view(text));
}

} // namespace fv::py
