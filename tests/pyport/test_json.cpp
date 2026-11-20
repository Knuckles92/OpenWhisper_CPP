// Direct tests of pyport/json.h.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <string>

#include "pyport/json.h"

using namespace fv::py;

TEST_CASE("a settings file holding Infinity loads", "[pyport]") {
    // test_typed_settings.py::test_an_integer_setting_saved_as_infinity_reads_as_its_default
    // needs this file to load at all; nlohmann refuses it.
    const Json settings = loads("{\"max_chars\": Infinity, \"gain\": -Infinity, \"x\": NaN}");
    CHECK(std::isinf(settings["max_chars"].get<double>()));
    CHECK(settings["gain"].get<double>() < 0);
    CHECK(std::isnan(settings["x"].get<double>()));
}

TEST_CASE("duplicate keys keep the last value in the first position", "[pyport]") {
    const Json doc = loads(R"({"a": 1, "b": 2, "a": 3})");
    CHECK(dumps(doc) == R"({"a": 3, "b": 2})");
}

TEST_CASE("dumps writes what json.dumps writes", "[pyport]") {
    const Json doc =
        loads(R"({"b": [1, 2.5, 1e16, true, null], "a": {}, "s": "\u00e9\ud83d\ude00"})");
    CHECK(dumps(doc) ==
          R"({"b": [1, 2.5, 1e+16, true, null], "a": {}, "s": "\u00e9\ud83d\ude00"})");
    DumpOptions indented;
    indented.indent = 2;
    indented.sort_keys = true;
    CHECK(dumps(doc, indented) ==
          "{\n  \"a\": {},\n  \"b\": [\n    1,\n    2.5,\n    1e+16,\n"
          "    true,\n    null\n  ],\n  \"s\": \"\\u00e9\\ud83d\\ude00\"\n}");
    DumpOptions raw;
    raw.ensure_ascii = false;
    raw.item_sep = ",";
    raw.key_sep = ":";
    CHECK(dumps(doc, raw) ==
          "{\"b\":[1,2.5,1e+16,true,null],\"a\":{},\"s\":\"\xc3\xa9\xf0\x9f\x98\x80\"}");
    CHECK(dumps(Json(1.0)) == "1.0");
}

TEST_CASE("a lone surrogate round-trips", "[pyport]") {
    const Json s = loads(R"("\ud83d")");
    CHECK(s.get<std::string>() == "\xed\xa0\xbd");
    CHECK(dumps(s) == R"("\ud83d")");
}

TEST_CASE("decode errors carry Python's message and position", "[pyport]") {
    try {
        loads("\n\n  [1,\n 2,]");
        FAIL("no error");
    } catch (const JsonError& e) {
        CHECK(e.msg() == "Expecting value");
        CHECK(e.lineno() == 4);
        CHECK(e.colno() == 4);
        CHECK(e.pos() == 11);
        CHECK(std::string(e.what()) == "Expecting value: line 4 column 4 (char 11)");
        CHECK(e.type_name() == "JSONDecodeError");
    }
    CHECK_THROWS_AS(loads("\xef\xbb\xbf[]"), JsonError);
}

TEST_CASE("nesting is limited as Python's decoder limits it", "[pyport]") {
    const auto nested = [](std::size_t n) {
        return std::string(n, '[') + std::string(n, ']');
    };
    CHECK_NOTHROW(loads(nested(kJsonMaxDepth)));
    CHECK_THROWS_AS(loads(nested(kJsonMaxDepth + 1)), RecursionError);
    // Writing a deep value does not recurse natively either.
    const Json deep = loads(nested(kJsonMaxDepth));
    CHECK(dumps(deep).size() == 2 * kJsonMaxDepth);
}

TEST_CASE("py_equal compares as Python's ==", "[pyport]") {
    CHECK(py_equal(Json(1), Json(1.0)));
    CHECK(py_equal(Json(true), Json(1)));
    CHECK(!py_equal(Json("1"), Json(1)));
    CHECK(py_equal(loads(R"({"a": 1, "b": [true]})"), loads(R"({"b": [1.0], "a": 1})")));
    CHECK(!py_equal(Json(9007199254740993LL), Json(9007199254740992.0)));
    CHECK(!py_equal(Json(std::nan("")), Json(std::nan(""))));
    CHECK(py_equal(Json(18446744073709551615ULL), Json(18446744073709551615ULL)));
    CHECK(!py_equal(Json(-1), Json(18446744073709551615ULL)));
}
