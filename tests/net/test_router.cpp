// The pure parts of the server: Starlette's path patterns and router, and the request and
// response text rules of uvicorn and urllib.parse.

#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <string>

#include "net/detail/router.h"
#include "net/detail/wire.h"

using namespace fv::net;
using namespace fv::net::detail;

namespace {

Route route(std::string method, std::string_view pattern) {
    return Route{std::move(method), RoutePattern(pattern), RouteOptions{}, {}};
}

} // namespace

TEST_CASE("path patterns match as Starlette's compiled regexes do") {
    CHECK(RoutePattern("/v1/transcriptions").match("/v1/transcriptions") == Params{});
    CHECK_FALSE(RoutePattern("/v1/transcriptions").match("/v1/transcriptions/"));
    CHECK(RoutePattern("/v1/transcriptions/{record_id}").match("/v1/transcriptions/42") ==
          Params{{"record_id", "42"}});
    CHECK_FALSE(RoutePattern("/v1/transcriptions/{record_id}").match("/v1/transcriptions/"));
    CHECK_FALSE(RoutePattern("/v1/transcriptions/{record_id}").match("/v1/transcriptions/4/2"));
    CHECK(RoutePattern("/v1/meetings/{meeting_id}/segments/{segment_id}")
              .match("/v1/meetings/m1/segments/s2") ==
          Params{{"meeting_id", "m1"}, {"segment_id", "s2"}});
    CHECK(RoutePattern("/files/{name}.json").match("/files/a.b.json") == Params{{"name", "a.b"}});
    CHECK(RoutePattern("/static/{rest:path}").match("/static/a/b/c.js") ==
          Params{{"rest", "a/b/c.js"}});
    CHECK(RoutePattern("/static/{rest:path}").match("/static/") == Params{{"rest", ""}});
    CHECK(RoutePattern("/n/{id:int}").match("/n/123") == Params{{"id", "123"}});
    CHECK_FALSE(RoutePattern("/n/{id:int}").match("/n/12a"));
    // Braces that are not a parameter are literal text.
    CHECK(RoutePattern("/a/{1x}").match("/a/{1x}") == Params{});
    CHECK(RoutePattern("/a/{{id}}").match("/a/{7}") == Params{{"id", "7"}});
}

TEST_CASE("the router takes the first full match, then 405, then a slash redirect") {
    Router router;
    router.add(route("POST", "/v1/filters/{lease_id}/renew"));
    router.add(route("DELETE", "/v1/filters/{lease_id}"));
    router.add(route("GET", "/v1/filters/{lease_id}"));
    router.add(route("GET", "/v1/events"));

    auto full = router.match("GET", "/v1/filters/abc");
    REQUIRE(full.route != nullptr);
    CHECK(full.route->method == "GET");
    CHECK(full.params == Params{{"lease_id", "abc"}});

    auto head = router.match("HEAD", "/v1/events");
    REQUIRE(head.route != nullptr);
    CHECK(head.route->method == "GET");

    auto wrong = router.match("PUT", "/v1/filters/abc");
    CHECK(wrong.route == nullptr);
    CHECK(wrong.path_known);
    CHECK(wrong.allowed == std::vector<std::string>{"DELETE"});

    auto slash = router.match("GET", "/v1/events/");
    CHECK(slash.route == nullptr);
    CHECK_FALSE(slash.path_known);
    CHECK(slash.redirect_path == "/v1/events");

    auto missing = router.match("GET", "/nowhere");
    CHECK_FALSE(missing.path_known);
    CHECK_FALSE(missing.redirect_path);
    CHECK_FALSE(router.match("GET", "/").redirect_path);
}

TEST_CASE("unquote and parse_query decode as urllib.parse does") {
    CHECK(unquote("/a%20b") == "/a b");
    CHECK(unquote("/a+b") == "/a+b");
    CHECK(unquote("a+b", true) == "a b");
    CHECK(unquote("%zz%4") == "%zz%4");
    CHECK(unquote("%C3%A9") == "\xC3\xA9");
    CHECK(unquote("%E9") == "\xEF\xBF\xBD");                               // not UTF-8: U+FFFD
    CHECK(unquote("%E2%82") == "\xEF\xBF\xBD");                            // truncated: one U+FFFD
    CHECK(unquote("%ED%A0%80") == "\xEF\xBF\xBD\xEF\xBF\xBD\xEF\xBF\xBD"); // a surrogate
    CHECK(unquote("%F0%9F%98%80") == "\xF0\x9F\x98\x80");

    CHECK(parse_query("") == Query{});
    CHECK(parse_query("a=1&b=&c&&d=x%3Dy&a=2") ==
          Query{{"a", "1"}, {"b", ""}, {"c", ""}, {"d", "x=y"}, {"a", "2"}});
    CHECK(parse_query("q=a+b%2Bc;d") == Query{{"q", "a b+c;d"}});
}

TEST_CASE("quote keeps the safe characters and encodes the rest") {
    const std::string_view safe = ":/%#?=@[]!$&'()*+,;";
    CHECK(quote("http://h:1/a b/\xC3\xA9?x=1&y=%20", safe) == "http://h:1/a%20b/%C3%A9?x=1&y=%20");
    CHECK(quote("a~b_c.d-e", "") == "a~b_c.d-e");
}

TEST_CASE("status phrases and dates are Python's") {
    CHECK(status_phrase(200) == "OK");
    CHECK(status_phrase(413) == "Request Entity Too Large");
    CHECK(status_phrase(418) == "I'm a Teapot");
    CHECK(status_phrase(299).empty());
    using namespace std::chrono;
    const auto when = sys_days{2026y / November / 15} + 19h + 5min + 3s;
    CHECK(http_date(when) == "Sun, 15 Nov 2026 19:05:03 GMT");
    CHECK(http_date(sys_days{2000y / January / 1}) == "Sat, 01 Jan 2000 00:00:00 GMT");
}

TEST_CASE("header finds the first value without regard to case") {
    const Headers headers{{"Content-Type", "a"}, {"content-type", "b"}, {"X-Other", "c"}};
    CHECK(header(headers, "content-type") == "a");
    CHECK(header(headers, "X-OTHER") == "c");
    CHECK_FALSE(header(headers, "missing").has_value());
}
