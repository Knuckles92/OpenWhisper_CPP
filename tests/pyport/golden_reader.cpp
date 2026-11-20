#include "golden_reader.h"

#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <charconv>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

#include "pyport/errors.h"
#include "pyport/unicode.h"

namespace golden {
namespace {

const Json& file() {
    static const Json data = [] {
        std::ifstream in(FV_PYPORT_GOLDEN, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        return Json::parse(text.str());
    }();
    return data;
}

bool is_int(const Json& v) {
    return v.is_number_integer() || v.is_number_unsigned();
}

} // namespace

const Json& cases(std::string_view function) {
    const Json& functions = file().at("functions");
    const auto it = functions.find(std::string(function));
    REQUIRE(it != functions.end());
    REQUIRE(!it->empty());
    return *it;
}

std::vector<std::string> function_names() {
    std::vector<std::string> names;
    for (const auto& [name, unused] : file().at("functions").items()) {
        names.push_back(name);
    }
    return names;
}

Json decode(const Json& value) {
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) {
            out.push_back(decode(item));
        }
        return out;
    }
    if (!value.is_object()) {
        return value;
    }
    if (value.size() == 1 && value.contains("$float")) {
        const std::string tag = value["$float"].get<std::string>();
        const double inf = std::numeric_limits<double>::infinity();
        return tag == "nan" ? std::numeric_limits<double>::quiet_NaN() : tag == "inf" ? inf : -inf;
    }
    if (value.size() == 1 && value.contains("$int")) {
        const std::string digits = value["$int"].get<std::string>();
        std::uint64_t u = 0;
        const auto r = std::from_chars(digits.data(), digits.data() + digits.size(), u);
        if (r.ec == std::errc() && r.ptr == digits.data() + digits.size()) {
            return u;
        }
        return value;
    }
    if (value.size() == 1 && value.contains("$code_points")) {
        std::string out;
        for (const Json& cp : value["$code_points"]) {
            fv::py::append_utf8(out, cp.get<char32_t>());
        }
        return out;
    }
    Json out = Json::object();
    for (const auto& [key, item] : value.items()) {
        out[key] = decode(item);
    }
    return out;
}

bool same(const Json& expected, const Json& actual) {
    if (is_int(expected) && is_int(actual)) {
        if (expected.is_number_unsigned() != actual.is_number_unsigned()) {
            const Json& u = expected.is_number_unsigned() ? expected : actual;
            const Json& i = expected.is_number_unsigned() ? actual : expected;
            return i.get<std::int64_t>() >= 0 &&
                   static_cast<std::uint64_t>(i.get<std::int64_t>()) == u.get<std::uint64_t>();
        }
        return expected == actual;
    }
    if (expected.is_number_float() && actual.is_number_float()) {
        const double e = expected.get<double>();
        const double a = actual.get<double>();
        return (std::isnan(e) && std::isnan(a)) ||
               std::bit_cast<std::uint64_t>(e) == std::bit_cast<std::uint64_t>(a);
    }
    if (expected.type() != actual.type()) {
        return false;
    }
    if (expected.is_array()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        for (std::size_t i = 0; i < expected.size(); ++i) {
            if (!same(expected[i], actual[i])) {
                return false;
            }
        }
        return true;
    }
    if (expected.is_object()) {
        if (expected.size() != actual.size()) {
            return false;
        }
        auto e = expected.begin();
        auto a = actual.begin();
        for (; e != expected.end(); ++e, ++a) {
            if (e.key() != a.key() || !same(e.value(), a.value())) {
                return false;
            }
        }
        return true;
    }
    return expected == actual;
}

void replay(std::string_view function, const std::function<Json(const Json& args)>& check) {
    for (const Json& c : cases(function)) {
        INFO(function << " " << c.dump(-1, ' ', false, Json::error_handler_t::replace));
        const Json args = decode(c.at("args"));
        if (c.contains("raises")) {
            const std::string type = c["raises"].get<std::string>();
            bool raised = false;
            try {
                check(args);
            } catch (const fv::py::PyError& e) {
                raised = true;
                CHECK(e.type_name() == type);
                if (c.contains("message")) {
                    CHECK(std::string(e.what()) == c["message"].get<std::string>());
                }
            }
            CHECK(raised);
            continue;
        }
        Json actual;
        try {
            actual = check(args);
        } catch (const fv::py::PyError& e) {
            FAIL("raised " << e.type_name() << ": " << e.what());
        }
        const Json expected = decode(c.at("result"));
        INFO("actual " << actual.dump(-1, ' ', false, Json::error_handler_t::replace));
        CHECK(same(expected, actual));
    }
}

} // namespace golden
