// MCP Events' pure functions against tests/golden/services.agent_mcp.events_ext.json, written by
// tools/parity/golden_vectors.py from OpenWhisper's services/agent_mcp/events_ext.py (case file
// tools/parity/cases/services.agent_mcp.events_ext.py).
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <string>

#include "agentapi/mcp_events.h"
#include "agentapi/mcp_events_delivery.h"
#include "agentapi/mcp_events_registry.h"
#include "automation/event_bus.h"
#include "golden/golden.h"
#include "pyport/json.h"

namespace {

using fv::Json;
using namespace fv::agentapi;
using namespace fv::agentapi::events_ext;

constexpr std::string_view kModule = "services.agent_mcp.events_ext";

// golden_format's tagged floats ({"$float": "nan"}) as numbers, at any depth.
Json untag(const Json& value) {
    if (value.is_object()) {
        if (value.size() == 1 && value.contains("$float")) {
            const std::string tag = value.at("$float").get<std::string>();
            return tag == "nan"   ? std::numeric_limits<double>::quiet_NaN()
                   : tag == "inf" ? std::numeric_limits<double>::infinity()
                                  : -std::numeric_limits<double>::infinity();
        }
        Json out = Json::object();
        for (const auto& [key, item] : value.items()) {
            out[key] = untag(item);
        }
        return out;
    }
    if (value.is_array()) {
        Json out = Json::array();
        for (const Json& item : value) {
            out.push_back(untag(item));
        }
        return out;
    }
    return value;
}

// The wrapper's {"ok": result} or {"error": {code, message, data}}, as the port gives it.
template <class Fn> Json outcome(Fn&& fn) {
    Json out = Json::object();
    try {
        out["ok"] = fn();
    } catch (const EventsError& error) {
        Json failure = Json::object();
        failure["code"] = error.code();
        failure["message"] = error.what();
        failure["data"] = error.data() ? *error.data() : Json(nullptr);
        out["error"] = std::move(failure);
    }
    return out;
}

// The recorded outcome with OpenWhisper's names in messages as FioriVoce's.
Json rebranded(const Json& expected) {
    Json out = expected;
    if (out.contains("error")) {
        out["error"]["message"] =
            fv::golden::rebrand(out.at("error").at("message").get<std::string>());
    }
    return out;
}

void check_outcome(const fv::golden::Case& c, const Json& actual) {
    INFO(c.label());
    CHECK(fv::golden::equal(rebranded(c.result<Json>()), actual));
}

} // namespace

TEST_CASE("normalize_arguments matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "normalize_arguments", [](const fv::golden::Case& c) {
        check_outcome(
            c, outcome([&] { return normalize_arguments(c.arg<std::string>(0), c.arg<Json>(1)); }));
    });
}

TEST_CASE("validate_callback_url matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "validate_callback_url", [](const fv::golden::Case& c) {
        check_outcome(c, outcome([&] { return Json(validate_callback_url(c.arg<Json>(0))); }));
    });
}

TEST_CASE("validate_secret matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "validate_secret", [](const fv::golden::Case& c) {
        check_outcome(c, outcome([&] { return Json(validate_secret(c.arg<Json>(0))); }));
    });
}

TEST_CASE("_granted_ttl_ms matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "granted_ttl_ms", [](const fv::golden::Case& c) {
        check_outcome(c, outcome([&] { return Json(granted_ttl_ms(untag(c.arg<Json>(0)))); }));
    });
}

TEST_CASE("subscription_id with OpenWhisper's principal matches OpenWhisper", "[golden]") {
    CHECK(principal() == "fiorivoce-mcp-token");
    fv::golden::for_each(kModule, "subscription_id", [](const fv::golden::Case& c) {
        INFO(c.label());
        CHECK(subscription_id(legacy_principal(), c.arg<std::string>(2), c.arg<std::string>(0),
                              c.arg<Json>(1)) == c.result<std::string>());
        CHECK(subscription_id(principal(), c.arg<std::string>(2), c.arg<std::string>(0),
                              c.arg<Json>(1)) != c.result<std::string>());
    });
}

TEST_CASE("descriptors matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "descriptors", [](const fv::golden::Case& c) {
        CHECK(fv::golden::equal(c.result<Json>(), descriptors()));
        // Key order is what events/list sends.
        CHECK(fv::py::dumps(c.result<Json>()) == fv::py::dumps(descriptors()));
    });
}

TEST_CASE("_event_body matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "event_body", [](const fv::golden::Case& c) {
        fv::automation::Event event;
        event.id = c.arg<std::string>(0);
        event.seq = c.arg<std::int64_t>(1);
        event.type = c.arg<std::string>(2);
        event.time = c.arg<std::string>(3);
        event.data = c.arg<Json>(4);
        CHECK(event_body(event) == c.result<std::string>());
    });
}

TEST_CASE("_status_category matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "status_category", [](const fv::golden::Case& c) {
        CHECK(status_category(c.arg<int>(0)) == c.result<std::string>());
    });
}

TEST_CASE("_retry_after matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "retry_after", [](const fv::golden::Case& c) {
        const auto value = c.arg<std::optional<std::string>>(0);
        const auto expected = c.result<std::optional<double>>();
        const auto actual =
            retry_after(value ? std::optional<std::string_view>(*value) : std::nullopt);
        REQUIRE(actual.has_value() == expected.has_value());
        if (expected) {
            CHECK(fv::golden::near(*expected, *actual, 0.0));
        }
    });
}

TEST_CASE("_iso matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "iso", [](const fv::golden::Case& c) {
        CHECK(iso(c.arg<double>(0)) == c.result<std::string>());
    });
}

TEST_CASE("_parse_iso matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "parse_iso", [](const fv::golden::Case& c) {
        CHECK(parse_iso(c.arg<Json>(0)) == c.result<std::optional<std::int64_t>>());
    });
}

TEST_CASE("_delivery matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "delivery", [](const fv::golden::Case& c) {
        check_outcome(c, outcome([&] {
                          const auto [url, secret] = delivery(c.arg<Json>(0), c.arg<bool>(1));
                          return Json::array({url, secret});
                      }));
    });
}

TEST_CASE("_event_name matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "event_name", [](const fv::golden::Case& c) {
        check_outcome(c, outcome([&] { return Json(event_name(c.arg<Json>(0))); }));
    });
}

TEST_CASE("_matches matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "matches", [](const fv::golden::Case& c) {
        CHECK(matches(c.arg<Json>(0), c.arg<Json>(1)) == c.result<bool>());
    });
}

TEST_CASE("EventSubscription.from_record matches OpenWhisper", "[golden]") {
    fv::golden::for_each(kModule, "from_record", [](const fv::golden::Case& c) {
        const auto subscription = EventSubscription::from_record(c.arg<Json>(0));
        const Json expected = c.result<Json>();
        if (expected.is_null()) {
            CHECK_FALSE(subscription.has_value());
        } else {
            REQUIRE(subscription.has_value());
            CHECK(fv::py::dumps(subscription->to_record()) == fv::py::dumps(expected));
        }
    });
}
