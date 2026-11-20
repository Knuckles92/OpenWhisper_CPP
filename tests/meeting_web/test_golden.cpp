// Golden vectors of meeting/web/auth.py, meeting/web/api.py (cursors, public meeting, digest),
// meeting/web/ws.py (store batch split, seq and guest-id coercion) and meeting/voice_help.py.

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <optional>
#include <string>
#include <vector>

#include "base/identity.h"
#include "base/testing/fake_clock.h"
#include "golden/golden.h"
#include "meeting_web/auth.h"
#include "meeting_web/cursors.h"
#include "meeting_web/public_meeting.h"
#include "meeting_web/voice_help.h"
#include "meeting_web/ws_protocol.h"
#include "pyport/errors.h"
#include "web_test_support.h"

using fv::Json;
using fv::golden::Case;
namespace mw = fv::meeting_web;
using fv::meeting_web::test_support::untag;

namespace {

std::optional<std::string> text_or_none(const Json& value) {
    if (value.is_string()) {
        return value.get<std::string>();
    }
    return std::nullopt;
}

fv::meeting_state::OpResult op_result(const Json& fields) {
    fv::meeting_state::OpResult result;
    result.ok = fields.at("ok").get<bool>();
    result.op = fields.at("op");
    result.reason = text_or_none(fields.at("reason"));
    result.target_id = text_or_none(fields.at("target_id"));
    result.effect = fields.at("effect");
    result.inverse = fields.at("inverse");
    if (fields.at("current_revision").is_number_integer()) {
        result.current_revision = fields.at("current_revision").get<std::int64_t>();
    }
    if (fields.at("seq").is_number_integer()) {
        result.seq = fields.at("seq").get<std::int64_t>();
    }
    return result;
}

void check_float(const Json& expected, double actual) {
    const Json want = untag(expected);
    if (std::isnan(want.get<double>())) {
        CHECK(std::isnan(actual));
    } else {
        CHECK(want.get<double>() == actual);
    }
}

} // namespace

TEST_CASE("resolve_role matches OpenWhisper", "[golden][meeting.web.auth]") {
    fv::golden::for_each("meeting.web.auth", "resolve_role", [](const Case& c) {
        const Json& token = c.args()[0];
        const std::optional<std::string> presented =
            token.is_string() ? std::optional<std::string>(token.get<std::string>()) : std::nullopt;
        const auto role = mw::resolve_stored_role(
            presented ? std::optional<std::string_view>(*presented) : std::nullopt, c.args()[1],
            c.args()[2]);
        CHECK(role == c.result<std::optional<std::string>>());
    });
}

TEST_CASE("_encode_cursor matches OpenWhisper", "[golden][meeting.web.api]") {
    fv::golden::for_each("meeting.web.api", "_encode_cursor", [](const Case& c) {
        const Json segment = untag(c.args()[0]);
        if (c.raises()) {
            try {
                (void)mw::encode_cursor(segment);
                FAIL("no exception");
            } catch (const fv::py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        CHECK(mw::encode_cursor(segment) == c.result<std::string>());
    });
}

TEST_CASE("_decode_cursor matches OpenWhisper", "[golden][meeting.web.api]") {
    fv::golden::for_each("meeting.web.api", "_decode_cursor", [](const Case& c) {
        const auto cursor = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK(c.raises_type() == "HTTPException");
            try {
                (void)mw::decode_cursor(cursor);
                FAIL("no exception");
            } catch (const mw::CursorError& error) {
                CHECK(std::string(error.what()) == "invalid transcript cursor");
            }
            return;
        }
        const auto decoded = mw::decode_cursor(cursor);
        const Json& expected = c.raw().at("result");
        // (None, None) for an empty cursor.
        if (expected.is_null() || expected[0].is_null()) {
            CHECK_FALSE(decoded.has_value());
            return;
        }
        REQUIRE(decoded.has_value());
        check_float(expected[0], decoded->start_s);
        CHECK(decoded->segment_id == expected[1].get<std::string>());
    });
}

TEST_CASE("_history_cursor matches OpenWhisper", "[golden][meeting.web.api]") {
    fv::golden::for_each("meeting.web.api", "_history_cursor", [](const Case& c) {
        const Json meeting = untag(c.args()[0]);
        if (c.raises()) {
            try {
                (void)mw::history_cursor(meeting);
                FAIL("no exception");
            } catch (const fv::py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        CHECK(mw::history_cursor(meeting) == c.result<std::string>());
    });
}

TEST_CASE("_decode_history_cursor matches OpenWhisper", "[golden][meeting.web.api]") {
    fv::golden::for_each("meeting.web.api", "_decode_history_cursor", [](const Case& c) {
        const auto cursor = c.arg<std::string>(0);
        if (c.raises()) {
            CHECK(c.raises_type() == "HTTPException");
            try {
                (void)mw::decode_history_cursor(cursor);
                FAIL("no exception");
            } catch (const mw::CursorError& error) {
                CHECK(std::string(error.what()) == "invalid meeting cursor");
            }
            return;
        }
        const auto decoded = mw::decode_history_cursor(cursor);
        const Json& expected = c.raw().at("result");
        // (None, None) for an empty cursor.
        if (expected.is_null() || expected[0].is_null()) {
            CHECK_FALSE(decoded.has_value());
            return;
        }
        REQUIRE(decoded.has_value());
        CHECK(decoded->started_at == expected[0].get<std::string>());
        CHECK(decoded->meeting_id == expected[1].get<std::string>());
    });
}

TEST_CASE("_public_meeting matches OpenWhisper", "[golden][meeting.web.api]") {
    const fv::FakeClock clock;
    fv::golden::for_each("meeting.web.api", "_public_meeting", [&](const Case& c) {
        const Json meeting = untag(c.args()[0]);
        if (c.raises()) {
            try {
                (void)mw::public_meeting(meeting, nullptr, clock);
                FAIL("no exception");
            } catch (const fv::py::PyError& error) {
                CHECK(error.type_name() == c.raises_type());
            }
            return;
        }
        const Json actual = mw::public_meeting(meeting, nullptr, clock);
        CAPTURE(actual.dump());
        CHECK(fv::golden::matches(c.raw().at("result"), actual, 1e-6));
        // Key order is the wire order.
        std::vector<std::string> expected_keys;
        for (const auto& [key, value] : c.raw().at("result").items()) {
            expected_keys.push_back(key);
        }
        std::vector<std::string> actual_keys;
        for (const auto& [key, value] : actual.items()) {
            actual_keys.push_back(key);
        }
        CHECK(actual_keys == expected_keys);
    });
}

TEST_CASE("_meeting_digest matches OpenWhisper", "[golden][meeting.web.api]") {
    fv::golden::for_each("meeting.web.api", "_meeting_digest", [](const Case& c) {
        const auto digest = mw::meeting_digest(untag(c.args()[0]));
        const Json& expected = c.raw().at("result");
        if (expected.is_null()) {
            CHECK_FALSE(digest.has_value());
            return;
        }
        REQUIRE(digest.has_value());
        CAPTURE(digest->dump());
        CHECK(fv::golden::equal(expected, *digest));
    });
}

TEST_CASE("_on_store_batch matches OpenWhisper", "[golden][meeting.web.ws]") {
    fv::golden::for_each("meeting.web.ws", "_on_store_batch", [](const Case& c) {
        std::vector<fv::meeting_state::OpResult> applied;
        for (const auto& fields : c.args()[1]) {
            applied.push_back(op_result(untag(fields)));
        }
        const auto sent = mw::split_store_batch(c.arg<std::int64_t>(0), applied);
        Json actual = Json::array();
        for (const auto& broadcast : sent) {
            actual.push_back(Json::array({broadcast.host_only, broadcast.message}));
        }
        CAPTURE(actual.dump());
        CHECK(fv::golden::equal(c.raw().at("result"), actual));
    });
}

TEST_CASE("_coerce_seq matches OpenWhisper", "[golden][meeting.web.ws]") {
    fv::golden::for_each("meeting.web.ws", "_coerce_seq", [](const Case& c) {
        CHECK(mw::coerce_seq(untag(c.args()[0])) == c.result<std::optional<std::int64_t>>());
    });
}

TEST_CASE("_clean_guest_id matches OpenWhisper", "[golden][meeting.web.ws]") {
    fv::golden::for_each("meeting.web.ws", "_clean_guest_id", [](const Case& c) {
        const auto value = c.arg<std::optional<std::string>>(0);
        const auto cleaned =
            mw::clean_guest_id(value ? std::optional<std::string_view>(*value) : std::nullopt);
        CHECK(cleaned == c.result<std::optional<std::string>>());
    });
}

TEST_CASE("voice_command_guide matches OpenWhisper", "[golden][meeting.voice_help]") {
    // OpenWhisper's defaults are its own wake names: the replay takes the defaults from the
    // legacy identity; FioriVoce's come from identity() (test_voice_help.cpp).
    fv::golden::for_each("meeting.voice_help", "voice_command_guide", [](const Case& c) {
        const Json guide = mw::voice_command_guide(c.args()[0], fv::legacy_identity());
        CAPTURE(guide.dump());
        CHECK(guide == c.raw().at("result"));
    });
    fv::golden::for_each(
        "meeting.voice_help", "resolve_typesafe_voice_command_names", [](const Case& c) {
            CHECK(mw::resolve_voice_command_names(c.args()[0], fv::legacy_identity()) ==
                  c.result<std::vector<std::string>>());
        });
}
