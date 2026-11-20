// Replays tests/golden/builtins.json (tools/gen/pyport_vectors.py, Python 3.12) against pyport.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <set>

#include "golden_reader.h"
#include "pyport/datetime.h"
#include "pyport/difflib.h"
#include "pyport/json.h"
#include "pyport/number.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "pyport/unicode.h"

using golden::Json;
using golden::replay;
namespace py = fv::py;

namespace {

std::string str(const Json& v) {
    return v.get<std::string>();
}

Json strings(const std::vector<std::string>& items) {
    Json out = Json::array();
    for (const auto& s : items) {
        out.push_back(s);
    }
    return out;
}

Json optional_strings(const std::vector<std::optional<std::string>>& items) {
    Json out = Json::array();
    for (const auto& s : items) {
        out.push_back(s ? Json(*s) : Json(nullptr));
    }
    return out;
}

std::vector<char32_t> code_points(const std::string& s) {
    const std::u32string cps = py::to_code_points(s);
    return {cps.begin(), cps.end()};
}

std::vector<std::string> words(const Json& v) {
    return v.get<std::vector<std::string>>();
}

// Runs fn on a SequenceMatcher over the two arguments: strings by code point, lists by word.
template <class Fn> Json with_matcher(const Json& args, Fn fn) {
    const bool autojunk = args[2].get<bool>();
    if (args[0].is_string()) {
        py::BasicSequenceMatcher<char32_t> m(code_points(str(args[0])), code_points(str(args[1])),
                                             autojunk);
        return fn(m);
    }
    py::WordSequenceMatcher m(words(args[0]), words(args[1]), autojunk);
    return fn(m);
}

const std::set<std::string>& replayed() {
    static const std::set<std::string> names = {"round",
                                                "round_ndigits",
                                                "format",
                                                "repr_float",
                                                "int",
                                                "float",
                                                "casefold",
                                                "lower",
                                                "upper",
                                                "title",
                                                "capitalize",
                                                "strip",
                                                "split",
                                                "isspace",
                                                "isalnum",
                                                "isalpha",
                                                "isdigit",
                                                "isdecimal",
                                                "isnumeric",
                                                "isupper",
                                                "islower",
                                                "istitle",
                                                "isprintable",
                                                "isidentifier",
                                                "len",
                                                "repr",
                                                "split_maxsplit",
                                                "split_sep",
                                                "strip_chars",
                                                "slice",
                                                "html.escape",
                                                "unicodedata.category",
                                                "re.sub",
                                                "re.sub_count",
                                                "re.findall",
                                                "re.split",
                                                "re.search_span",
                                                "json.dumps",
                                                "json.loads",
                                                "json.loads_depth",
                                                "py_equal",
                                                "datetime.fromisoformat",
                                                "datetime.isoformat",
                                                "date.fromisoformat",
                                                "SequenceMatcher.ratio",
                                                "SequenceMatcher.get_opcodes",
                                                "SequenceMatcher.get_matching_blocks",
                                                "SequenceMatcher.quick_ratio",
                                                "get_close_matches"};
    return names;
}

} // namespace

TEST_CASE("every function of builtins.json is replayed", "[golden]") {
    for (const std::string& name : golden::function_names()) {
        INFO(name);
        CHECK(replayed().contains(name));
    }
}

TEST_CASE("golden round and format", "[golden]") {
    replay("round", [](const Json& a) {
        return Json(static_cast<std::int64_t>(py::round_half_even(a[0].get<double>())));
    });
    replay("round_ndigits", [](const Json& a) {
        return Json(py::round_ndigits(a[0].get<double>(), a[1].get<int>()));
    });
    replay("format", [](const Json& a) {
        const std::string spec = str(a[1]); // ".Nf"
        return Json(py::format_fixed(a[0].get<double>(), std::stoi(spec.substr(1))));
    });
    replay("repr_float", [](const Json& a) { return Json(py::float_repr(a[0].get<double>())); });
}

TEST_CASE("golden int and float", "[golden]") {
    // A Python int beyond int64 is recorded tagged {"$int": digits}; pyport reports overflow.
    for (const Json& c : golden::cases("int")) {
        INFO(c.dump());
        const auto result = py::py_int_checked(golden::decode(c["args"])[0]);
        if (c.contains("raises")) {
            REQUIRE(!result.has_value());
            const auto type = c["raises"].get<std::string>();
            CHECK(type == (result.error() == py::IntError::Type    ? "TypeError"
                           : result.error() == py::IntError::Value ? "ValueError"
                                                                   : "OverflowError"));
        } else if (c["result"].is_object()) {
            REQUIRE(!result.has_value());
            CHECK(result.error() == py::IntError::Overflow);
        } else {
            REQUIRE(result.has_value());
            CHECK(*result == c["result"].get<std::int64_t>());
        }
    }
    replay("float", [](const Json& a) -> Json {
        const auto result = py::py_float(a[0]);
        if (!result) {
            throw py::PyError(a[0].is_string() ? "ValueError" : "TypeError", "float()");
        }
        return *result;
    });
}

TEST_CASE("golden str methods", "[golden]") {
    using Fn = std::string (*)(std::string_view);
    const std::pair<const char*, Fn> mappings[] = {
        {"casefold", py::casefold}, {"lower", py::lower},           {"upper", py::upper},
        {"title", py::title},       {"capitalize", py::capitalize}, {"repr", py::repr},
    };
    for (const auto& [name, fn] : mappings) {
        replay(name, [fn](const Json& a) { return Json(fn(str(a[0]))); });
    }
    replay("strip", [](const Json& a) { return Json(py::strip(str(a[0]))); });
    using Pred = bool (*)(std::string_view);
    const std::pair<const char*, Pred> predicates[] = {
        {"isspace", py::isspace},           {"isalnum", py::isalnum},
        {"isalpha", py::isalpha},           {"isdigit", py::isdigit},
        {"isdecimal", py::isdecimal},       {"isnumeric", py::isnumeric},
        {"isupper", py::isupper},           {"islower", py::islower},
        {"istitle", py::istitle},           {"isprintable", py::isprintable},
        {"isidentifier", py::isidentifier},
    };
    for (const auto& [name, fn] : predicates) {
        replay(name, [fn](const Json& a) { return Json(fn(str(a[0]))); });
    }
    replay("len", [](const Json& a) { return Json(py::len(str(a[0]))); });
    replay("split", [](const Json& a) { return strings(py::split(str(a[0]))); });
    replay("split_maxsplit",
           [](const Json& a) { return strings(py::split(str(a[0]), a[1].get<std::int64_t>())); });
    replay("split_sep", [](const Json& a) {
        return strings(py::split_on(str(a[0]), str(a[1]), a[2].get<std::int64_t>()));
    });
    replay("strip_chars", [](const Json& a) { return Json(py::strip(str(a[0]), str(a[1]))); });
    replay("slice", [](const Json& a) {
        const auto index = [](const Json& v) {
            return v.is_null() ? std::nullopt : std::optional<std::int64_t>(v.get<std::int64_t>());
        };
        return Json(py::slice(str(a[0]), index(a[1]), index(a[2])));
    });
    replay("html.escape", [](const Json& a) {
        return Json(py::html_escape(str(a[0]), a.size() < 2 || a[1].get<bool>()));
    });
    replay("unicodedata.category", [](const Json& a) {
        return Json(std::string(py::general_category(py::to_code_points(str(a[0]))[0])));
    });
}

TEST_CASE("golden re", "[golden]") {
    const auto sub = [](const Json& a) {
        const py::Regex re(str(a[0]), a[1].get<unsigned>());
        return Json(re.sub(str(a[2]), str(a[3]), a.size() > 4 ? a[4].get<std::size_t>() : 0));
    };
    replay("re.sub", sub);
    replay("re.sub_count", sub);
    replay("re.findall", [](const Json& a) -> Json {
        const py::Regex re(str(a[0]), a[1].get<unsigned>());
        if (re.group_count() > 1) {
            Json out = Json::array();
            for (const auto& row : re.findall_groups(str(a[2]))) {
                out.push_back(strings(row));
            }
            return out;
        }
        return strings(re.findall(str(a[2])));
    });
    replay("re.split", [](const Json& a) {
        const py::Regex re(str(a[0]), a[1].get<unsigned>());
        return optional_strings(re.split(str(a[2]), a.size() > 3 ? a[3].get<std::size_t>() : 0));
    });
    replay("re.search_span", [](const Json& a) -> Json {
        const py::Regex re(str(a[0]), a[1].get<unsigned>());
        const auto m = re.search(str(a[2]));
        if (!m) {
            return nullptr;
        }
        return Json::array({m->start(), m->end(), m->group()});
    });
}

TEST_CASE("golden json", "[golden]") {
    replay("json.dumps", [](const Json& a) {
        py::DumpOptions options;
        const Json& o = a[1];
        if (o.contains("indent")) {
            options.indent = o["indent"].get<int>();
        }
        if (o.contains("separators")) {
            options.item_sep = str(o["separators"][0]);
            options.key_sep = str(o["separators"][1]);
        }
        options.ensure_ascii = o.value("ensure_ascii", true);
        options.sort_keys = o.value("sort_keys", false);
        return Json(py::dumps(a[0], options));
    });
    replay("json.loads", [](const Json& a) { return py::loads(str(a[0])); });
    replay("json.loads_depth", [](const Json& a) {
        const auto n = a[0].get<std::size_t>();
        const bool array = str(a[1]) == "array";
        std::string text;
        for (std::size_t i = 0; i < n; ++i) {
            text += array ? "[" : "{\"a\":";
        }
        text += array ? "" : "1";
        text += std::string(n, array ? ']' : '}');
        Json value = py::loads(text);
        std::size_t depth = 0;
        const Json* v = &value;
        while ((v->is_array() || v->is_object()) && !v->empty()) {
            v = v->is_array() ? &(*v)[0] : &(*v)["a"];
            ++depth;
        }
        return Json(depth + (array ? 1 : 0));
    });
    replay("py_equal", [](const Json& a) { return Json(py::py_equal(a[0], a[1])); });
}

TEST_CASE("golden datetime", "[golden]") {
    replay("datetime.fromisoformat",
           [](const Json& a) { return Json(py::fromisoformat(str(a[0])).isoformat()); });
    replay("datetime.isoformat", [](const Json& a) {
        return Json(py::fromisoformat(str(a[0])).isoformat(str(a[1])[0], str(a[2])));
    });
    replay("date.fromisoformat",
           [](const Json& a) { return Json(py::date_fromisoformat(str(a[0])).isoformat()); });
}

TEST_CASE("golden difflib", "[golden]") {
    replay("SequenceMatcher.ratio",
           [](const Json& a) { return with_matcher(a, [](auto& m) { return Json(m.ratio()); }); });
    replay("SequenceMatcher.quick_ratio", [](const Json& a) {
        return with_matcher(
            a, [](auto& m) { return Json::array({m.quick_ratio(), m.real_quick_ratio()}); });
    });
    replay("SequenceMatcher.get_opcodes", [](const Json& a) {
        return with_matcher(a, [](auto& m) {
            Json out = Json::array();
            for (const py::Opcode& op : m.get_opcodes()) {
                out.push_back(Json::array(
                    {std::string(py::op_tag_name(op.tag)), op.i1, op.i2, op.j1, op.j2}));
            }
            return out;
        });
    });
    replay("SequenceMatcher.get_matching_blocks", [](const Json& a) {
        return with_matcher(a, [](auto& m) {
            Json out = Json::array();
            for (const py::MatchBlock& b : m.get_matching_blocks()) {
                out.push_back(Json::array({b.a, b.b, b.size}));
            }
            return out;
        });
    });
    replay("get_close_matches", [](const Json& a) {
        return strings(
            py::get_close_matches(str(a[0]), words(a[1]), a[2].get<int>(), a[3].get<double>()));
    });
}
