// The downloader's caller hooks: a sender of the caller's own (redirect rules) and a second
// route taken once per transfer after a refused answer (the updater's asset-API fallback).

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

#include "base/hash.h"
#include "download/verified_download.h"
#include "http/testing/fake_http_client.h"
#include "testkit/testkit.h"

using fv::download::DownloadCanceled;
using fv::download::DownloadError;
using fv::download::DownloadOptions;
using fv::download::NetworkFailure;
using fv::http::FakeHttpClient;
using fv::http::Request;
using fv::http::ScriptedResponse;
namespace fs = std::filesystem;

namespace {

const std::string kBody = "route-switched-payload";
const std::string kPrimary = "https://primary.example/file.bin";
const std::string kSecondary = "https://secondary.example/file.bin";

std::string read_file(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

ScriptedResponse refused(int code) {
    ScriptedResponse response;
    response.status = code;
    response.reason = "Refused";
    return response;
}

void run(FakeHttpClient& client, const fs::path& destination, const DownloadOptions& options,
         const fv::CancelToken& cancel = {}) {
    fv::download::download_verified(
        client, kPrimary, destination, static_cast<std::int64_t>(kBody.size()),
        fv::sha256_hex(kBody), [](auto, auto, auto) {}, cancel, false, options);
}

} // namespace

TEST_CASE("a refused answer moves the transfer to the other route once", "[download]") {
    fv::testkit::TempDataRoot root;
    const auto destination = root.path() / "file.bin";
    FakeHttpClient client;
    client.set_handler([](const Request& request) {
        return request.url == kPrimary ? refused(503) : FakeHttpClient::serve(request, kBody);
    });
    std::vector<int> asked;
    DownloadOptions options;
    options.switch_route = [&](int status) -> std::optional<std::string> {
        asked.push_back(status);
        return kSecondary;
    };
    run(client, destination, options);
    CHECK(read_file(destination) == kBody);
    CHECK(asked == std::vector<int>{503});
    REQUIRE(client.requests().size() == 2);
    CHECK(client.requests()[1].url == kSecondary);

    // The second route refusing too ends the transfer: there is no third.
    fs::remove(destination);
    FakeHttpClient failing;
    failing.set_handler([](const Request&) { return refused(503); });
    asked.clear();
    CHECK_THROWS_AS(run(failing, destination, options), DownloadError);
    CHECK(asked == std::vector<int>{503});
    CHECK(failing.requests().size() == 2);
}

TEST_CASE("a route that declines leaves the refusal as it was", "[download]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([](const Request&) { return refused(404); });
    DownloadOptions options;
    options.switch_route = [](int) -> std::optional<std::string> {
        return std::nullopt;
    };
    try {
        run(client, root.path() / "file.bin", options);
        FAIL("the refused download succeeded");
    } catch (const DownloadError& error) {
        REQUIRE(error.failure().has_value());
        CHECK(error.failure()->code == 404);
    }
    CHECK(client.requests().size() == 1);
}

TEST_CASE("a cancel before the other route stops the transfer", "[download]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([](const Request&) { return refused(502); });
    fv::CancelSource cancel;
    DownloadOptions options;
    options.switch_route = [&](int) -> std::optional<std::string> {
        cancel.cancel();
        return kSecondary;
    };
    CHECK_THROWS_AS(run(client, root.path() / "file.bin", options, cancel.token()),
                    DownloadCanceled);
    CHECK(client.requests().size() == 1);
    CHECK_FALSE(fs::exists(root.path() / "file.bin.part"));
}

TEST_CASE("a caller's sender carries every request and may refuse one", "[download]") {
    fv::testkit::TempDataRoot root;
    const auto destination = root.path() / "file.bin";
    FakeHttpClient client;
    client.set_handler(
        [](const Request& request) { return FakeHttpClient::serve(request, kBody); });
    std::vector<std::string> sent;
    DownloadOptions options;
    options.send = [&](const Request& request, const fv::CancelToken& cancel,
                       fv::http::BodySink& sink) -> fv::download::SendResult {
        sent.push_back(request.url);
        auto result = client.send(request, cancel, &sink);
        if (!result) {
            return std::unexpected(result.error());
        }
        return std::move(*result);
    };
    run(client, destination, options);
    CHECK(read_file(destination) == kBody);
    CHECK(sent == std::vector<std::string>{kPrimary});

    // A refusal of the sender's own is worded like any network failure, and a partial already
    // on disk stays for the next attempt.
    fs::remove(destination);
    { std::ofstream(root.path() / "file.bin.part", std::ios::binary) << kBody.substr(0, 5); }
    options.send = [](const Request&, const fv::CancelToken&,
                      fv::http::BodySink&) -> fv::download::SendResult {
        return std::unexpected(NetworkFailure{NetworkFailure::Kind::Url, 0, "unsafe hop"});
    };
    options.describe_error = [](const NetworkFailure& failure) {
        return "described: " + failure.reason;
    };
    try {
        run(client, destination, options);
        FAIL("the refused download succeeded");
    } catch (const DownloadError& error) {
        CHECK(std::string(error.what()) == "described: unsafe hop");
    }
    CHECK(read_file(root.path() / "file.bin.part") == kBody.substr(0, 5));
}
