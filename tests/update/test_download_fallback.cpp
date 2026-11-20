// A release download that hits a GitHub gateway error retries through the release-asset API:
// OpenWhisper's tests/test_app_update_download_fallback.py (the download half; the parsing half
// is in test_release_feed.cpp).

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include "download_support.h"
#include "http/testing/fake_http_client.h"
#include "testkit/testkit.h"
#include "update/download.h"

namespace update = fv::update;
using fv::http::FakeHttpClient;
using fv::http::Request;
using fv::http::ScriptedResponse;
using namespace fv::update::test;

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

// Cancels `cancel` after each answer of `inner`.
class CancelAfterClient final : public fv::http::Client {
public:
    CancelAfterClient(fv::http::Client& inner, fv::CancelSource& cancel)
        : inner_(inner), cancel_(cancel) {}
    std::expected<fv::http::Response, fv::http::Error>
    send(const Request& request, const fv::CancelToken& token, fv::http::BodySink* sink) override {
        auto result = inner_.send(request, token, sink);
        cancel_.cancel();
        return result;
    }

private:
    fv::http::Client& inner_;
    fv::CancelSource& cancel_;
};

std::string download_failure(fv::http::Client& client, const update::ReleaseAsset& asset,
                             const std::filesystem::path& dir, const fv::CancelToken& cancel = {}) {
    try {
        update::download_release_asset(client, github_feed(), asset, dir, {}, cancel);
    } catch (const update::AppUpdateError& error) {
        return error.what();
    }
    FAIL("no AppUpdateError");
    return {};
}

// The API answers as GitHub does: a redirect to the CDN, which serves the bytes.
ScriptedResponse api_route(const Request& request, std::string_view body = kBody) {
    if (request.url == api_url()) {
        return found(std::string(kCdnUrl));
    }
    if (request.url == kCdnUrl) {
        return FakeHttpClient::serve(request, body);
    }
    return status(404, "Not Found");
}

// A client whose head says the body came from somewhere else (curl having followed a redirect it
// was told not to).
class ElsewhereClient final : public fv::http::Client {
public:
    std::expected<fv::http::Response, fv::http::Error>
    send(const Request& request, const fv::CancelToken&, fv::http::BodySink* sink) override {
        fv::http::Response head;
        head.status = 200;
        head.reason = "OK";
        head.effective_url = "https://evil.example/file";
        (void)request;
        if (sink != nullptr && sink->on_head(head)) {
            sink->write(std::span<const char>(kBody.data(), kBody.size()));
        }
        return head;
    }
};

class RecordingSink final : public fv::http::BodySink {
public:
    bool on_head(const fv::http::Response& head) override {
        heads.push_back(head.status);
        return true;
    }
    bool write(std::span<const char> data) override {
        body.append(data.data(), data.size());
        return true;
    }
    std::vector<int> heads;
    std::string body;
};

} // namespace

// from
// tests/test_app_update_download_fallback.py::test_gateway_error_downloads_and_verifies_via_api
TEST_CASE("gateway error downloads and verifies via api", "[test_app_update_download_fallback]") {
    const int code = GENERATE(500, 502, 503, 504);
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    FakeHttpClient client;
    client.set_handler([&](const Request& request) {
        return request.url == asset.url ? status(code, "Gateway failure") : api_route(request);
    });
    const auto path = update::download_release_asset(client, github_feed(), asset, root.path());
    CHECK(path == root.path() / asset.name);
    CHECK(read_bytes(path) == kBody);
    const auto requests = client.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].url == asset.url);
    CHECK(accept_of(requests[0]).empty());
    CHECK(requests[1].url == api_url());
    CHECK(accept_of(requests[1]) == "application/octet-stream");
    CHECK(requests[2].url == kCdnUrl);
    CHECK_FALSE(std::filesystem::exists(root.path() / (asset.name + ".part")));
}

// from
// tests/test_app_update_download_fallback.py::test_successful_primary_download_does_not_call_api
TEST_CASE("successful primary download does not call api", "[test_app_update_download_fallback]") {
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    FakeHttpClient client;
    client.set_handler(
        [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
    update::download_release_asset(client, github_feed(), asset, root.path());
    const auto requests = client.requests();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].url == asset.url);
    CHECK_FALSE(requests[0].follow_redirects);
}

// from tests/test_app_update_download_fallback.py::test_other_failures_do_not_switch_routes
TEST_CASE("other failures do not switch routes", "[test_app_update_download_fallback]") {
    // 0: a failure before any answer (an unsafe redirect or a certificate failure).
    const int code = GENERATE(403, 404, 429, 0);
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([&](const Request&) {
        if (code == 0) {
            ScriptedResponse failure;
            failure.error = fv::http::Error{fv::http::ErrorKind::Connect, "unsafe or offline"};
            return failure;
        }
        return status(code, "Refused");
    });
    download_failure(client, setup_asset(), root.path());
    CHECK(client.requests().size() == 1);
}

// from tests/test_app_update_download_fallback.py::test_failures_are_bounded
TEST_CASE("failures are bounded", "[test_app_update_download_fallback]") {
    const auto [asset_id, attempts] =
        GENERATE(std::pair<std::optional<std::int64_t>, std::size_t>{kAssetId, 2},
                 std::pair<std::optional<std::int64_t>, std::size_t>{std::nullopt, 1});
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset("2.8.6", kBody, asset_id);
    FakeHttpClient client;
    client.set_handler([](const Request&) { return status(504, "Gateway Time-out"); });
    const auto message = download_failure(client, asset, root.path());
    CHECK(contains(message, "504"));
    CHECK(message == "The update server returned an error (504 Gateway Time-out).");
    CHECK(client.requests().size() == attempts);
    CHECK_FALSE(std::filesystem::exists(root.path() / asset.name));
}

// from tests/test_app_update_download_fallback.py::test_fallback_preserves_validated_range_resume
TEST_CASE("fallback preserves validated range resume", "[test_app_update_download_fallback]") {
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    write_bytes(root.path() / (asset.name + ".part"), kBody.substr(0, 8));
    FakeHttpClient client;
    client.set_handler([&](const Request& request) {
        return request.url == asset.url ? status(504, "Gateway Time-out") : api_route(request);
    });
    update::download_release_asset(client, github_feed(), asset, root.path());
    const auto requests = client.requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].url == asset.url);
    CHECK(requests[0].range_from == 8u);
    CHECK(accept_of(requests[0]).empty());
    CHECK(requests[1].url == api_url());
    CHECK(requests[1].range_from == 8u);
    CHECK(accept_of(requests[1]) == "application/octet-stream");
    CHECK(requests[2].range_from == 8u);
    CHECK(read_bytes(root.path() / asset.name) == kBody);
}

// from tests/test_app_update_download_fallback.py::test_range_restart_stays_on_api
TEST_CASE("range restart stays on api", "[test_app_update_download_fallback]") {
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    write_bytes(root.path() / (asset.name + ".part"), kBody.substr(0, 8));
    FakeHttpClient client;
    client.set_handler([&](const Request& request) {
        if (request.url == asset.url) {
            return status(504, "Gateway Time-out");
        }
        if (request.url == api_url() && request.range_from) {
            return status(416, "Range Not Satisfiable");
        }
        return api_route(request);
    });
    update::download_release_asset(client, github_feed(), asset, root.path());
    std::vector<std::string> urls;
    for (const auto& request : client.requests()) {
        urls.push_back(request.url);
    }
    CHECK(urls == std::vector<std::string>{asset.url, api_url(), api_url(), std::string(kCdnUrl)});
    const auto last_api = client.requests()[2];
    CHECK_FALSE(last_api.range_from);
    CHECK(accept_of(last_api) == "application/octet-stream");
    CHECK(read_bytes(root.path() / asset.name) == kBody);
}

// from tests/test_app_update_download_fallback.py::test_cancel_after_gateway_error_stops_before_api
TEST_CASE("cancel after gateway error stops before api", "[test_app_update_download_fallback]") {
    fv::testkit::TempDataRoot root;
    fv::CancelSource cancel;
    FakeHttpClient inner;
    inner.set_handler([](const Request&) { return status(504, "Gateway Time-out"); });
    // The cancel lands once the gateway error is in, as Python's opener sets it while raising.
    CancelAfterClient client(inner, cancel);
    const auto message = download_failure(client, setup_asset(), root.path(), cancel.token());
    CHECK(message == "The download was canceled.");
    REQUIRE(inner.requests().size() == 1);
    CHECK(inner.requests()[0].url == setup_asset().url);
    CHECK(folder_names(root.path()).empty());
}

// from tests/test_app_update_download_fallback.py::test_fallback_cannot_bypass_digest_or_size
TEST_CASE("fallback cannot bypass digest or size", "[test_app_update_download_fallback]") {
    const std::string body = GENERATE(std::string(kBody.size(), 'x'), std::string("too short"));
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    FakeHttpClient client;
    client.set_handler([&](const Request& request) {
        return request.url == asset.url ? status(504, "Gateway Time-out")
                                        : api_route(request, body);
    });
    const auto message = download_failure(client, asset, root.path());
    CHECK((contains(message, "integrity") || contains(message, "size")));
    CHECK(folder_names(root.path()).empty());
}

// from
// tests/test_app_update_download_fallback.py::test_asset_api_redirect_uses_existing_trust_checks
TEST_CASE("asset api redirect uses existing trust checks", "[test_app_update_download_fallback]") {
    FakeHttpClient client;
    client.set_handler([](const Request& request) { return api_route(request); });
    Request request;
    request.url = api_url();
    request.headers = {{"Accept", "application/octet-stream"}};
    RecordingSink sink;
    const auto sent = update::send_checked(client, request, kRepo, {}, sink);
    REQUIRE(sent.has_value());
    CHECK(sent->effective_url == kCdnUrl);
    CHECK(sink.body == kBody);
    CHECK(sink.heads == std::vector<int>{200});
    const auto requests = client.requests();
    REQUIRE(requests.size() == 2);
    CHECK(accept_of(requests[1]) == "application/octet-stream");
    CHECK_FALSE(requests[1].follow_redirects);
}

// from tests/test_app_update_download_fallback.py::test_asset_api_does_not_broaden_other_redirects
TEST_CASE("asset api does not broaden other redirects", "[test_app_update_download_fallback]") {
    const std::string cdn(kCdnUrl);
    const auto [initial, target] = GENERATE_COPY(
        std::pair<std::string, std::string>{
            "https://api.github.com/repos/" + std::string(kRepo) + "/releases/latest", cdn},
        std::pair<std::string, std::string>{
            "https://api.github.com/repos/other/repo/releases/assets/" + std::to_string(kAssetId),
            cdn},
        std::pair<std::string, std::string>{api_url() + "?redirect=1", cdn},
        std::pair<std::string, std::string>{api_url(), "https://evil.example/file"},
        std::pair<std::string, std::string>{api_url(), "http:" + cdn.substr(6)},
        std::pair<std::string, std::string>{
            api_url(), "https://release-assets.githubusercontent.com/other/file"},
        std::pair<std::string, std::string>{api_url(),
                                            "https://release-assets.githubusercontent.com:444/"
                                            "github-production-release-asset/1/2?signature=test"},
        std::pair<std::string, std::string>{api_url(),
                                            "https://user@release-assets.githubusercontent.com/"
                                            "github-production-release-asset/1/2?signature=test"});
    FakeHttpClient client;
    client.set_handler(
        [&](const Request& request) { return request.url == initial ? found(target) : ok(kBody); });
    Request request;
    request.url = initial;
    RecordingSink sink;
    const auto sent = update::send_checked(client, request, kRepo, {}, sink);
    REQUIRE_FALSE(sent.has_value());
    const auto* failure = std::get_if<fv::download::NetworkFailure>(&sent.error());
    REQUIRE(failure != nullptr);
    CHECK(failure->kind == fv::download::NetworkFailure::Kind::Url);
    CHECK(contains(failure->reason, "unsafe URL"));
    CHECK(client.requests().size() == 1);
    CHECK(sink.heads.empty());
}

// from tests/test_app_update_download_fallback.py::test_final_response_url_is_also_checked
TEST_CASE("final response url is also checked", "[test_app_update_download_fallback]") {
    ElsewhereClient client;
    Request request;
    request.url = api_url();
    RecordingSink sink;
    const auto sent = update::send_checked(client, request, kRepo, {}, sink);
    REQUIRE_FALSE(sent.has_value());
    const auto* failure = std::get_if<fv::download::NetworkFailure>(&sent.error());
    REQUIRE(failure != nullptr);
    CHECK(failure->reason == "The update server returned an unsafe URL.");
    CHECK(sink.heads.empty());
    CHECK(sink.body.empty());
}

TEST_CASE("a streamed download from an untrusted start sends nothing", "[update]") {
    FakeHttpClient client;
    Request request;
    request.url = "https://example.com/setup.exe";
    RecordingSink sink;
    const auto sent = update::send_checked(client, request, kRepo, {}, sink);
    REQUIRE_FALSE(sent.has_value());
    CHECK(std::get<fv::download::NetworkFailure>(sent.error()).reason ==
          "The update URL is not trusted.");
    CHECK(client.requests().empty());
}

TEST_CASE("more than five redirects are refused", "[update]") {
    FakeHttpClient client;
    client.set_handler([](const Request& request) {
        const auto hop = request.url.back() - '0';
        return found("https://github.com/" + std::string(kRepo) + "/hop" + std::to_string(hop + 1));
    });
    Request request;
    request.url = "https://github.com/" + std::string(kRepo) + "/hop0";
    RecordingSink sink;
    const auto sent = update::send_checked(client, request, kRepo, {}, sink);
    REQUIRE_FALSE(sent.has_value());
    CHECK(std::get<fv::download::NetworkFailure>(sent.error()).reason ==
          "The update server redirected to an unsafe URL.");
    CHECK(client.requests().size() == 6);
}

TEST_CASE("a rate-limited download says so", "[update]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([](const Request&) {
        auto answer = status(403, "Forbidden");
        answer.headers = {{"X-RateLimit-Remaining", "0"}};
        return answer;
    });
    const auto message = download_failure(client, setup_asset(), root.path());
    CHECK(contains(message, "rate-limited"));
    CHECK(contains(message, "https://github.com/example/app/releases"));
}
