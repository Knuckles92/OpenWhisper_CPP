// Direct tests of pyport/number.h; the Python-made cases are in test_golden.cpp.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include "pyport/errors.h"
#include "pyport/number.h"

using namespace fv::py;

TEST_CASE("round_half_even rounds ties to the even neighbour", "[pyport]") {
    CHECK(round_half_even(62.5) == 62.0);
    CHECK(round_half_even(63.5) == 64.0);
    CHECK(round_half_even(0.5) == 0.0);
    CHECK(round_half_even(1.5) == 2.0);
    CHECK(round_half_even(-0.5) == 0.0);
    CHECK(std::signbit(round_half_even(-0.5)));
    CHECK(round_half_even(-1.5) == -2.0);
    CHECK(round_half_even(2.4999999999999996) == 2.0);
    CHECK(std::isinf(round_half_even(std::numeric_limits<double>::infinity())));
}

TEST_CASE("format_fixed rounds the exact binary value", "[pyport]") {
    CHECK(format_fixed(2500.0 / 1000.0, 0) == "2");
    CHECK(format_fixed(0.15, 1) == "0.1");
    CHECK(format_fixed(0.25, 1) == "0.2");
    CHECK(format_fixed(-0.04, 1) == "-0.0");
    CHECK(format_fixed(1499.5, 0) == "1500");
    CHECK(format_fixed(std::numeric_limits<double>::quiet_NaN(), 2) == "nan");
    CHECK(format_fixed(-std::numeric_limits<double>::infinity(), 2) == "-inf");
}

TEST_CASE("round_ndigits goes through the correctly rounded decimal", "[pyport]") {
    CHECK(round_ndigits(2.675, 2) == 2.67);
    CHECK(round_ndigits(0.125, 2) == 0.12);
    CHECK(round_ndigits(0.375, 2) == 0.38);
    CHECK(round_ndigits(1250.0, -2) == 1200.0);
    CHECK(round_ndigits(1350.0, -2) == 1400.0);
    CHECK(round_ndigits(9.995, 2) == 9.99);
    CHECK(round_ndigits(99.5, 0) == 100.0);
    CHECK(std::signbit(round_ndigits(-0.4, 0)));
    CHECK(round_ndigits(5e-324, 400) == 5e-324);
    CHECK(round_ndigits(123.0, -400) == 0.0);
    CHECK_THROWS_AS(round_ndigits(1.7976931348623157e308, -308), OverflowError);
}

TEST_CASE("float_repr matches Python's repr", "[pyport]") {
    CHECK(float_repr(1.0) == "1.0");
    CHECK(float_repr(0.1) == "0.1");
    CHECK(float_repr(1e16) == "1e+16");
    CHECK(float_repr(1e15) == "1000000000000000.0");
    CHECK(float_repr(1e-5) == "1e-05");
    CHECK(float_repr(0.0001) == "0.0001");
    CHECK(float_repr(-0.0) == "-0.0");
    CHECK(float_repr(1.5e300) == "1.5e+300");
    CHECK(float_repr(std::numeric_limits<double>::infinity()) == "inf");
    CHECK(float_repr(std::numeric_limits<double>::quiet_NaN()) == "nan");
}

TEST_CASE("py_int follows int() and the strict integer rule", "[pyport]") {
    CHECK(py_int(Json(3.7)) == 3);
    CHECK(py_int(Json(-3.7)) == -3);
    CHECK(py_int(Json(true)) == 1);
    CHECK(py_int("3.7") == std::nullopt);
    CHECK(py_int(" 12 ") == 12);
    CHECK(py_int("1_000") == 1000);
    CHECK(py_int("\xd9\xa1\xd9\xa2") == 12); // Arabic-Indic digits
    CHECK(py_int("-9223372036854775808") == std::numeric_limits<std::int64_t>::min());
    CHECK(py_int_checked("9223372036854775808").error() == IntError::Overflow);
    CHECK(py_int_checked(Json(1e300)).error() == IntError::Overflow);
    CHECK(py_int_checked(Json(std::numeric_limits<double>::infinity())).error() ==
          IntError::Overflow);
    CHECK(py_int_checked(Json(std::numeric_limits<double>::quiet_NaN())).error() ==
          IntError::Value);
    CHECK(py_int_checked(Json(nullptr)).error() == IntError::Type);
    CHECK(py_int_checked(Json::array()).error() == IntError::Type);
    CHECK(py_int(Json(7), true) == 7);
    CHECK(py_int(Json(true), true) == std::nullopt);
    CHECK(py_int(Json(7.0), true) == std::nullopt);
    CHECK(py_int(Json("7"), true) == std::nullopt);
    CHECK(py_int_checked(std::string(4301, '1')).error() == IntError::Value);
}

TEST_CASE("int() and float() strip only what CPython's ASCII parser strips", "[pyport]") {
    // U+001C..U+001F are str.isspace() but below U+007F, where CPython transforms nothing.
    CHECK(py_int("\x1c"
                 "7\xc2\xa0") == std::nullopt);
    CHECK(py_float("\xc2\xa0"
                   "7\x1c") == std::nullopt);
    CHECK(py_int("\xc2\x85"
                 "7\xc2\xa0") == 7); // U+0085 and U+00A0 become ' '
    CHECK(py_float("\xe2\x80\xa8"
                   "7\xe2\x80\xa9") == 7.0);
}

TEST_CASE("py_float follows float()", "[pyport]") {
    CHECK(py_float(" 1_000.5 ") == 1000.5);
    CHECK(py_float("1e400") == std::numeric_limits<double>::infinity());
    CHECK(py_float("-1e-400") == 0.0);
    CHECK(std::isnan(*py_float("NaN")));
    CHECK(py_float("-Infinity") == -std::numeric_limits<double>::infinity());
    CHECK(py_float(".5") == 0.5);
    CHECK(py_float("5.") == 5.0);
    CHECK(py_float("1_") == std::nullopt);
    CHECK(py_float("1__0") == std::nullopt);
    CHECK(py_float("0x10") == std::nullopt);
    CHECK(py_float("") == std::nullopt);
}
