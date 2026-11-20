#pragma once

#include "base/json.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

// fv_golden: replays the golden vectors tools/parity/golden_vectors.py writes to
// tests/golden/<python.module>.json (format: tools/parity/golden_format.py).
//
//   golden::for_each("services.format_utils", "format_audio_duration",
//       [](const golden::Case& c) {
//           CHECK(format_audio_duration(c.arg<double>(0)) == c.result<std::string>());
//       });
namespace fv::golden {

// A case's arguments or result cannot be read as the type a test asked for.
class DecodeError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

namespace detail {
template <class T> struct is_optional : std::false_type {};
template <class T> struct is_optional<std::optional<T>> : std::true_type {};
template <class T> struct is_vector : std::false_type {};
template <class T> struct is_vector<std::vector<T>> : std::true_type {};

double decode_double(const Json& value);
std::int64_t decode_integer(const Json& value);
[[noreturn]] void fail(std::string_view expected, const Json& value);
} // namespace detail

// A JSON value as the C++ type T: bool, an integer type (Python int; never a bool or a float),
// a floating type (int or float, and {"$float": "nan"|"inf"|"-inf"}), std::string,
// std::optional<U> (null is nullopt), std::vector<U>, or Json for the raw value.
template <class T> T decode(const Json& value) {
    if constexpr (std::is_same_v<T, Json>) {
        return value;
    } else if constexpr (detail::is_optional<T>::value) {
        if (value.is_null()) {
            return std::nullopt;
        }
        return decode<typename T::value_type>(value);
    } else if constexpr (std::is_same_v<T, bool>) {
        if (!value.is_boolean()) {
            detail::fail("a bool", value);
        }
        return value.get<bool>();
    } else if constexpr (std::is_integral_v<T>) {
        const std::int64_t integer = detail::decode_integer(value);
        if (integer < static_cast<std::int64_t>(std::numeric_limits<T>::min()) ||
            (integer > 0 && static_cast<std::uint64_t>(integer) >
                                static_cast<std::uint64_t>(std::numeric_limits<T>::max()))) {
            detail::fail("an integer in range", value);
        }
        return static_cast<T>(integer);
    } else if constexpr (std::is_floating_point_v<T>) {
        return static_cast<T>(detail::decode_double(value));
    } else if constexpr (std::is_same_v<T, std::string>) {
        if (!value.is_string()) {
            detail::fail("a string", value);
        }
        return value.get<std::string>();
    } else if constexpr (detail::is_vector<T>::value) {
        if (!value.is_array()) {
            detail::fail("a list", value);
        }
        T items;
        for (const auto& item : value) {
            items.push_back(decode<typename T::value_type>(item));
        }
        return items;
    } else {
        static_assert(sizeof(T) == 0, "golden::decode: unsupported type");
    }
}

class Case {
public:
    Case(std::string function, std::size_t index, const Json& raw, double float_tolerance);

    template <class T> T arg(std::size_t index) const {
        if (index >= arg_count()) {
            throw DecodeError(label() + ": no argument " + std::to_string(index));
        }
        return decode<T>(args()[index]);
    }
    std::size_t arg_count() const;
    const Json& args() const;
    // Keyword arguments; an empty object when the call had none.
    const Json& kwargs() const;

    template <class T> T result() const {
        if (raises()) {
            throw DecodeError(label() + " raised " + std::string(raises_type()) +
                              "; it has no result");
        }
        return decode<T>(raw_.at("result"));
    }
    bool raises() const;
    std::string_view raises_type() const; // the Python exception's class name, or ""
    const Json& raw() const;
    // Inputs harvested from OpenWhisper's own tests rather than curated.
    bool traced() const;
    // The file's "float_tolerance" (absolute), 0 when it has none.
    double float_tolerance() const;
    // "<function> #<index> <args as compact JSON>", for failure messages.
    std::string label() const;

private:
    std::string function_;
    std::size_t index_;
    const Json& raw_;
    double tolerance_;
};

// Path of a module's vector file, and the parsed file (loaded once per process).
std::filesystem::path vector_file(std::string_view module);
const Json& load(std::string_view module);

// Runs `body` once per case of module.function, each in its own Catch2 DYNAMIC_SECTION. A
// module or function without vectors fails the test.
void for_each(std::string_view module, std::string_view function,
              const std::function<void(const Case&)>& body);
// The same over an already parsed document (tests of the harness itself).
void for_each_in(const Json& document, std::string_view function,
                 const std::function<void(const Case&)>& body);

// OpenWhisper's product, file and variable names as FioriVoce's, for comparing strings a
// vector recorded with strings the port builds from identity(). A literal map: M0.11 checks it
// against identity() once fv_base implements it.
std::string rebrand(std::string_view text);

// Comparators. equal: JSON values as Python's == sees them for JSON data (1 == 1.0, but a bool
// equals only a bool), with tagged floats decoded and NaN equal to NaN. near: absolute
// tolerance, NaN equal to NaN and infinities equal to themselves. matches: equal, except that
// floats compare with near(tolerance). equal_rebranded: rebrand(expected) == actual.
bool equal(const Json& expected, const Json& actual);
bool near(double expected, double actual, double tolerance);
bool matches(const Json& expected, const Json& actual, double tolerance);
bool equal_rebranded(std::string_view expected, std::string_view actual);

} // namespace fv::golden
