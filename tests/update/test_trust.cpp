// Redirects followed by hand: what urllib's opener and _SafeRedirectHandler did for _open.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "http/testing/fake_http_client.h"
#include "update/testing/fake_feed.h"
#include "update/trust.h"

namespace update = fv::update;
using fv::http::FakeHttpClient;
using fv::http::ScriptedResponse;
using update::testing::FakeFeed;

namespace {

const std::string kRepo = "example/app";
const std::string kPage = "https://github.com/example/app/releases/latest";

std::expected<fv::http::Response, update::FetchFailure>
open(FakeHttpClient& client, const std::string& url = kPage, std::string_view method = "GET") {
    return update::open_checked(client, update::update_request({}, url, method), kRepo, {});
}

std::string reason(const std::expected<fv::http::Response, update::FetchFailure>& opened) {
    REQUIRE_FALSE(opened);
    return opened.error().failure.reason;
}

} // namespace

TEST_CASE("an untrusted start is refused before any request", "[update]") {
    FakeHttpClient client;
    CHECK(reason(open(client, "https://example.com/releases/latest")) ==
          "The update URL is not trusted.");
    CHECK(reason(open(client, "http://github.com/example/app/releases/latest")) ==
          "The update URL is not trusted.");
    CHECK(client.requests().empty());
}

TEST_CASE("a relative redirect is joined to the URL it came from", "[update]") {
    FakeHttpClient client;
    client.push(FakeFeed::redirect("/example/app/releases/tag/v1.2.3"));
    client.push({});
    const auto opened = open(client, kPage, "HEAD");
    REQUIRE(opened);
    CHECK(opened->effective_url == "https://github.com/example/app/releases/tag/v1.2.3");
    const auto requests = client.requests();
    REQUIRE(requests.size() == 2);
    CHECK(requests[1].method == "HEAD");
    CHECK(requests[1].url == "https://github.com/example/app/releases/tag/v1.2.3");
    CHECK(requests[1].headers == requests[0].headers);
    CHECK_FALSE(requests[1].follow_redirects);
}

TEST_CASE("a HEAD redirected with 303 stays HEAD", "[update]") {
    FakeHttpClient client;
    client.push(FakeFeed::redirect("https://github.com/x", 303));
    client.push({});
    REQUIRE(open(client, kPage, "HEAD"));
    CHECK(client.requests()[1].method == "HEAD");
}

TEST_CASE("at most five redirects are followed", "[update]") {
    FakeHttpClient client;
    for (int i = 0; i < 5; ++i) {
        client.push(FakeFeed::redirect("https://github.com/hop" + std::to_string(i)));
    }
    client.push({});
    CHECK(open(client));

    FakeHttpClient endless;
    for (int i = 0; i < 6; ++i) {
        endless.push(FakeFeed::redirect("https://github.com/hop" + std::to_string(i)));
    }
    CHECK(reason(open(endless)) == "The update server redirected to an unsafe URL.");
    CHECK(endless.requests().size() == 6);
}

TEST_CASE("a redirect loop is refused as urllib's infinite loop", "[update]") {
    FakeHttpClient client;
    for (int i = 0; i < 5; ++i) {
        client.push(FakeFeed::redirect("https://github.com/same"));
    }
    const auto opened = open(client);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().failure.kind == fv::download::NetworkFailure::Kind::HttpStatus);
    CHECK(opened.error().failure.code == 302);
    CHECK(opened.error().failure.reason.starts_with(
        "The HTTP server returned a redirect error that would lead to an infinite loop."));
}

TEST_CASE("a redirect to another scheme is an HTTP error naming it", "[update]") {
    FakeHttpClient client;
    client.push(FakeFeed::redirect("javascript:alert(1)"));
    const auto opened = open(client);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().failure.code == 302);
    CHECK(opened.error().failure.reason ==
          "Found - Redirection to url 'javascript:alert(1)' is not allowed");
}

TEST_CASE("a redirect without a Location is the HTTP error it is", "[update]") {
    FakeHttpClient client;
    client.push(FakeFeed::http_error(302, "Found"));
    const auto opened = open(client);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().failure.kind == fv::download::NetworkFailure::Kind::HttpStatus);
    CHECK(opened.error().failure.code == 302);
}

TEST_CASE("an unsafe final URL is refused", "[update]") {
    // A client that followed a redirect on its own would report where it ended up.
    class Moved final : public fv::http::Client {
    public:
        std::expected<fv::http::Response, fv::http::Error>
        send(const fv::http::Request&, const fv::CancelToken&, fv::http::BodySink*) override {
            fv::http::Response response;
            response.status = 200;
            response.effective_url = "https://evil.example/file";
            return response;
        }
    } moved;
    const auto opened = update::open_checked(moved, update::update_request({}, kPage), kRepo, {});
    REQUIRE_FALSE(opened);
    CHECK(opened.error().failure.reason == "The update server returned an unsafe URL.");
}

TEST_CASE("a malformed host reaches the user as Python's ValueError text", "[update]") {
    FakeHttpClient client;
    client.push(FakeFeed::redirect("https://[::1/x"));
    const auto opened = open(client);
    REQUIRE_FALSE(opened);
    CHECK(opened.error().failure.kind == fv::download::NetworkFailure::Kind::Other);
    CHECK(opened.error().failure.reason == "Invalid IPv6 URL");
    CHECK(update::describe_update_error(opened.error(), kRepo) == "Invalid IPv6 URL");
}

TEST_CASE("an asset API URL may continue to the release-asset CDN only", "[update]") {
    const auto api = "https://api.github.com/repos/example/app/releases/assets/123456";
    const auto cdn =
        "https://release-assets.githubusercontent.com/github-production-release-asset/1/2";
    CHECK(update::is_github_asset_api_url(kRepo, api));
    CHECK_FALSE(update::is_github_asset_api_url("", api));
    CHECK_FALSE(update::is_github_asset_api_url(kRepo, std::string(api) + "?x"));
    CHECK(update::redirect_url_allowed(kRepo, api, cdn));
    CHECK_FALSE(update::redirect_url_allowed("other/repo", api, cdn));
    CHECK_FALSE(update::redirect_url_allowed(
        kRepo, "https://api.github.com/repos/example/app/releases/latest", cdn));
}

TEST_CASE("without a GitHub source a 429 is a plain HTTP error", "[update]") {
    update::FetchFailure failure;
    failure.failure = {fv::download::NetworkFailure::Kind::HttpStatus, 429, "Too Many Requests"};
    CHECK(update::is_rate_limited(failure));
    CHECK(update::describe_update_error(failure, "") ==
          "The update server returned an error (429 Too Many Requests).");
    CHECK(update::describe_update_error(failure, kRepo) ==
          "GitHub rate-limited this update check. Try again later, or open "
          "https://github.com/example/app/releases.");
}
