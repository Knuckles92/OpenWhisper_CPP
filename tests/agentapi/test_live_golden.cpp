// tests/golden/services.automation.api.json: services/automation/api.py's helpers on curated
// inputs (tools/parity/cases/services.automation.api.py), against agentapi/live_api.h.
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agentapi/live_api.h"
#include "automation/event_bus.h"
#include "golden/golden.h"
#include "pyport/json.h"

namespace {

using fv::Json;
using namespace fv::agentapi;

constexpr std::string_view kModule = "services.automation.api";

// {"value": ...} or {"invalid", "status", "code"}, as the case file's wrappers record them.
Json outcome(const std::function<Json()>& fn) {
    try {
        return Json{{"value", fn()}};
    } catch (const Invalid& refused) {
        return Json{{"invalid", fv::golden::rebrand(refused.message())},
                    {"status", refused.status()},
                    {"code", refused.code()}};
    }
}

Json rebranded(Json expected) {
    if (expected.contains("invalid")) {
        expected["invalid"] = fv::golden::rebrand(expected.at("invalid").get<std::string>());
    }
    return expected;
}

std::optional<std::string> optional_text(const Json& value) {
    return value.is_null() ? std::nullopt : std::optional(value.get<std::string>());
}

} // namespace

TEST_CASE("parse_types matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "parse_types", [](const fv::golden::Case& c) {
        const Json actual = outcome([&]() -> Json {
            const auto types = parse_types(optional_text(c.args()[0]));
            return types ? Json(*types) : Json(nullptr);
        });
        CHECK(fv::golden::equal(rebranded(c.result<Json>()), actual));
    });
}

TEST_CASE("parse_last_event_id matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "parse_last_event_id", [](const fv::golden::Case& c) {
        const Json actual = outcome([&]() -> Json {
            const auto position = parse_last_event_id(optional_text(c.args()[0]));
            return position ? Json(*position) : Json(nullptr);
        });
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("sse_frame matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "sse_frame", [](const fv::golden::Case& c) {
        const Json& given = c.args()[0];
        fv::automation::Event event;
        event.id = given.at("id").get<std::string>();
        event.seq = given.at("seq").get<std::int64_t>();
        event.type = given.at("type").get<std::string>();
        event.time = given.at("time").get<std::string>();
        event.data = fv::py::loads(given.at("data_json").get<std::string>());
        CHECK(sse_frame(event, c.arg<bool>(1)) == c.result<std::string>());
    });
}

TEST_CASE("gap_event matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "gap_event", [](const fv::golden::Case& c) {
        const fv::automation::Event event =
            gap_event(c.arg<std::int64_t>(0), c.arg<std::string>(1));
        const Json actual{{"seq", event.seq},
                          {"type", event.type},
                          {"data", event.data},
                          {"id_prefix", event.id.substr(0, 4)},
                          {"id_length", event.id.size()}};
        CHECK(fv::golden::equal(c.result<Json>(), actual));
        CHECK(event.time.size() == 24);
        CHECK(event.time.ends_with('Z'));
    });
}

TEST_CASE("_string matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "string", [](const fv::golden::Case& c) {
        const Json body = fv::py::loads(c.arg<std::string>(0));
        const Json actual = outcome([&]() -> Json {
            return string_field(body, c.arg<std::string>(1), c.arg<bool>(2), c.arg<std::size_t>(3),
                                c.arg<std::string>(4));
        });
        CHECK(fv::golden::equal(c.result<Json>(), actual));
    });
}

TEST_CASE("is_unicode_text matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "is_unicode_text", [](const fv::golden::Case& c) {
        const Json text = fv::py::loads(c.arg<std::string>(0));
        CHECK(is_unicode_text(text.get<std::string>()) == c.result<bool>());
    });
}

TEST_CASE("peer_is_local matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "peer_is_local", [](const fv::golden::Case& c) {
        CHECK(peer_is_local(c.arg<std::string>(0)) == c.result<bool>());
    });
}
