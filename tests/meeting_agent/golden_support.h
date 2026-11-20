#pragma once

// Replay helpers shared by the golden tests of tests/meeting_agent.

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include "base/json.h"
#include "golden/golden.h"
#include "pyport/errors.h"

namespace fv::meeting_agent_test {

using fv::Json;
namespace golden = fv::golden;

// The Python exception's class name, or "" when nothing pyport-shaped was thrown.
template <class Fn> std::string raised(Fn&& fn) {
    try {
        std::forward<Fn>(fn)();
    } catch (const fv::py::PyError& error) {
        return std::string(error.type_name());
    }
    return "";
}

// Every string in `value` with OpenWhisper's names as OpenWhisper_CPP's.
inline Json rebranded(const Json& value) {
    if (value.is_string()) {
        return golden::rebrand(value.get<std::string>());
    }
    if (value.is_array() || value.is_object()) {
        Json out = value;
        for (auto& item : out) {
            item = rebranded(item);
        }
        return out;
    }
    return value;
}

// Checks a case: the exception Python raised, or the result as JSON, rebranded.
template <class Fn> void replay(const golden::Case& c, Fn&& fn) {
    INFO(c.label());
    if (c.raises()) {
        CHECK(raised(fn) == c.raises_type());
        return;
    }
    const Json actual = fn();
    const Json expected = rebranded(c.result<Json>());
    INFO("expected " << expected.dump());
    INFO("actual   " << actual.dump());
    CHECK(golden::equal(expected, actual));
}

// A positional argument, else the keyword of that name, else `fallback`.
inline Json arg_or_kwarg(const golden::Case& c, std::size_t index, const char* name,
                         Json fallback) {
    if (index < c.arg_count()) {
        return c.args()[index];
    }
    if (c.kwargs().contains(name)) {
        return c.kwargs()[name];
    }
    return fallback;
}

} // namespace fv::meeting_agent_test
