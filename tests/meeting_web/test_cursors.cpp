// meeting_web/cursors.h against the malformed cursors of tests/test_meeting_web_history.py, at
// their full size: the golden vectors carry the 100,000 nested brackets as 10,000 to keep the
// vector file small, and the parser must refuse the real depth without exhausting the stack.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/base64.h"
#include "meeting_web/cursors.h"

namespace mw = fv::meeting_web;

namespace {

std::string urlsafe(const std::string& raw) {
    return fv::b64encode(raw, /*urlsafe=*/true, /*pad=*/false);
}

} // namespace

// Cases of tests/test_meeting_web_history.py::test_malformed_transcript_cursors_are_client_errors
// (too-deeply-nested, start-overflows-float)
TEST_CASE("a transcript cursor nested 100,000 deep or overflowing float is a 400", "[cursors]") {
    const std::string nested = urlsafe(std::string(100'000, '['));
    CHECK_THROWS_AS(mw::decode_cursor(nested), mw::CursorError);
    const std::string overflow = urlsafe("[" + std::string(400, '9') + ",\"s\"]");
    CHECK_THROWS_AS(mw::decode_cursor(overflow), mw::CursorError);
    const auto decoded = mw::decode_cursor(urlsafe("[1.5,\"sg_1\"]"));
    REQUIRE(decoded.has_value());
    CHECK(decoded->start_s == 1.5);
    CHECK(decoded->segment_id == "sg_1");
}

// Cases of tests/test_meeting_web_history.py::test_malformed_history_cursors_are_client_errors
// (too-deeply-nested, too long)
TEST_CASE("a history cursor nested 100,000 deep or over 1,024 characters is a 400", "[cursors]") {
    CHECK_THROWS_AS(mw::decode_history_cursor(urlsafe(std::string(100'000, '['))), mw::CursorError);
    CHECK_THROWS_AS(mw::decode_history_cursor(std::string(5000, 'x')), mw::CursorError);
    try {
        (void)mw::decode_history_cursor(std::string(5000, 'x'));
    } catch (const mw::CursorError& error) {
        CHECK(std::string(error.what()) == "invalid meeting cursor");
    }
    const auto decoded = mw::decode_history_cursor(urlsafe(R"(["2026-01-01T09:00:00Z","m_1"])"));
    REQUIRE(decoded.has_value());
    CHECK(decoded->started_at == "2026-01-01T09:00:00Z");
    CHECK(decoded->meeting_id == "m_1");
}
