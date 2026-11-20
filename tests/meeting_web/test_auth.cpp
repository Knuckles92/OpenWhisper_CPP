// meeting_web/auth.h: the role and token tests of tests/test_meeting_web_auth.py.

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

#include "meeting_web/auth.h"

namespace mw = fv::meeting_web;

namespace {

// tests/fakes/meeting_web.py
constexpr std::string_view kHostToken = "host-token-aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kGuestToken = "guest-token-bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

} // namespace

// from tests/test_meeting_web_auth.py::TestResolveRole::test_host_and_guest_match
TEST_CASE("host and guest match", "[test_meeting_web_auth]") {
    CHECK(mw::resolve_role(kHostToken, kHostToken, kGuestToken) == "host");
    CHECK(mw::resolve_role(kGuestToken, kHostToken, kGuestToken) == "guest");
}

// from tests/test_meeting_web_auth.py::TestResolveRole::test_wrong_token_rejected
TEST_CASE("wrong token rejected", "[test_meeting_web_auth]") {
    CHECK_FALSE(mw::resolve_role("nope", kHostToken, kGuestToken).has_value());
    CHECK_FALSE(mw::resolve_role("", kHostToken, kGuestToken).has_value());
    CHECK_FALSE(mw::resolve_role(std::nullopt, kHostToken, kGuestToken).has_value());
}

// from tests/test_meeting_web_auth.py::TestResolveRole::test_host_preferred_when_tokens_collide
TEST_CASE("host preferred when tokens collide", "[test_meeting_web_auth]") {
    CHECK(mw::resolve_role("same", "same", "same") == "host");
}

// from tests/test_meeting_web_auth.py::TestResolveRole::test_generate_tokens_are_distinct
TEST_CASE("generate tokens are distinct", "[test_meeting_web_auth]") {
    const auto [a, b] = mw::generate_token_pair();
    CHECK(a != b);
    CHECK(mw::generate_token().size() >= 32);
}

TEST_CASE("a token is 43 characters of base64url without padding", "[auth]") {
    std::set<std::string> seen;
    for (int i = 0; i < 50; ++i) {
        const auto token = mw::generate_token();
        CHECK(token.size() == 43);
        for (const char c : token) {
            const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                 (c >= '0' && c <= '9') || c == '-' || c == '_';
            CHECK(allowed);
        }
        seen.insert(token);
    }
    CHECK(seen.size() == 50);
}

TEST_CASE("an empty expected token never matches", "[auth]") {
    CHECK_FALSE(mw::tokens_match("", ""));
    CHECK_FALSE(mw::tokens_match("x", ""));
    CHECK(mw::tokens_match("x", "x"));
    CHECK_FALSE(mw::tokens_match("xy", "x"));
    CHECK_FALSE(mw::tokens_match("x", "xy"));
    CHECK_FALSE(mw::resolve_role("x", std::nullopt, std::nullopt).has_value());
}
