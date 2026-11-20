// Verified setup downloads: OpenWhisper's tests/test_app_update.py (TestDownloadInstaller and the
// download half of TestNetworkErrorCopy), and the checks around the asset itself.

#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <string>

#include "base/hash.h"
#include "download_support.h"
#include "http/testing/fake_http_client.h"
#include "testkit/testkit.h"
#include "update/download.h"
#include "update/update_contract.h"

namespace update = fv::update;
using fv::http::FakeHttpClient;
using namespace fv::update::test;

namespace {

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string failure_of(const std::function<void()>& action) {
    try {
        action();
    } catch (const update::AppUpdateError& error) {
        return error.what();
    }
    FAIL("no AppUpdateError");
    return {};
}

} // namespace

// from
// tests/test_app_update.py::TestNetworkErrorCopy::test_certificate_failure_during_download_gives_guidance
TEST_CASE("certificate failure during download gives guidance", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    client.set_handler([](const fv::http::Request&) {
        fv::http::ScriptedResponse response;
        response.error = FakeHttpClient::certificate_failure();
        return response;
    });
    const auto feed = github_feed();
    const auto probe = installed_probe();
    const auto context = update::DownloadContext{client, feed, probe, root.path(), "win32"};
    const auto message =
        failure_of([&] { update::download_installer(context, release_with(setup_asset())); });
    CHECK(contains(message, "update server's certificate could not be verified"));
    CHECK(contains(message,
                   "allow api.github.com, github.com and release-assets.githubusercontent.com."));
    CHECK_FALSE(contains(message, "CERTIFICATE_VERIFY_FAILED"));
}

// from tests/test_app_update.py::TestDownloadInstaller::test_refuses_source_channel
TEST_CASE("refuses source channel", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    const auto feed = github_feed();
    // A build tree on a computer that also has the app installed elsewhere.
    auto probe = installed_probe("C:/Users/u/Programs/App");
    probe.running_dir = "D:/build/bin";
    const auto context = update::DownloadContext{client, feed, probe, root.path(), "win32"};
    const auto message =
        failure_of([&] { update::download_installer(context, release_with(setup_asset())); });
    CHECK(contains(message, "cannot install"));
    CHECK(message == update::cannot_install_message());
    CHECK(client.requests().empty());

    // Off Windows the setup cannot run either.
    const auto installed = installed_probe();
    const auto linux_context =
        update::DownloadContext{client, feed, installed, root.path(), "linux"};
    CHECK(failure_of([&] {
              update::download_installer(linux_context, release_with(setup_asset()));
          }) == update::cannot_install_message());
    CHECK(client.requests().empty());
}

// from tests/test_app_update.py::TestDownloadInstaller::test_hash_mismatch_discards_file
TEST_CASE("hash mismatch discards file", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    const std::string payload = "not-the-installer";
    FakeHttpClient client;
    client.set_handler([&](const fv::http::Request&) { return ok(payload); });
    auto asset = setup_asset("2.2.0", payload);
    asset.sha256 = std::string(64, 'a');
    const auto feed = github_feed();
    const auto probe = installed_probe();
    const auto context = update::DownloadContext{client, feed, probe, root.path(), "win32"};
    const auto message =
        failure_of([&] { update::download_installer(context, release_with(asset, "2.2.0")); });
    CHECK(contains(message, "integrity"));
    CHECK(folder_names(root.path()).empty());
}

// from tests/test_app_update.py::TestDownloadInstaller::test_matching_hash_keeps_existing_file
TEST_CASE("matching hash keeps existing file", "[test_app_update]") {
    fv::testkit::TempDataRoot root;
    const std::string body = "verified-setup";
    const auto asset = setup_asset("2.2.0", body);
    const auto existing = root.path() / update::setup_asset_name("2.2.0");
    write_bytes(existing, body);
    FakeHttpClient client;
    const auto feed = github_feed();
    const auto probe = installed_probe();
    const auto context = update::DownloadContext{client, feed, probe, root.path(), "win32"};
    std::vector<std::string> phases;
    const auto path = update::download_installer(
        context, release_with(asset, "2.2.0"),
        [&](std::string_view phase, std::int64_t done, std::int64_t total) {
            phases.push_back(std::string(phase) + " " + std::to_string(done) + "/" +
                             std::to_string(total));
        });
    CHECK(path == existing);
    CHECK(client.requests().empty());
    CHECK(phases == std::vector<std::string>{"verifying 14/14"});
}

TEST_CASE("a stale file of the same name is replaced by a verified download", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    const auto destination = root.path() / asset.name;
    write_bytes(destination, "an older, different download");
    FakeHttpClient client;
    client.set_handler([](const fv::http::Request&) { return ok(kBody); });
    const auto path = update::download_release_asset(client, github_feed(), asset, root.path());
    CHECK(path == destination);
    CHECK(read_bytes(destination) == kBody);
    CHECK(client.requests().size() == 1);
}

TEST_CASE("an asset must carry a digest, a plain file name and a sane size", "[update]") {
    auto asset = setup_asset();
    CHECK(update::validated_asset_digest(asset) == fv::sha256_hex(kBody));
    asset.sha256 = fv::sha256_hex(kBody);
    for (auto& c : *asset.sha256) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    CHECK(update::validated_asset_digest(asset) == fv::sha256_hex(kBody));

    auto bad = setup_asset();
    bad.sha256.reset();
    CHECK(failure_of([&] { update::validated_asset_digest(bad); }) ==
          "The update package is missing an integrity digest.");
    bad = setup_asset();
    bad.sha256 = "sha256:" + fv::sha256_hex(kBody);
    CHECK(failure_of([&] { update::validated_asset_digest(bad); }) ==
          "The update package is missing an integrity digest.");
    for (const auto* name : {"", "../setup.exe", "dir/setup.exe"}) {
        bad = setup_asset();
        bad.name = name;
        CHECK(failure_of([&] { update::validated_asset_digest(bad); }) ==
              "The update package has an invalid filename.");
    }
#ifdef _WIN32
    for (const auto* name : {"dir\\setup.exe", "C:setup.exe"}) {
        bad = setup_asset();
        bad.name = name;
        CHECK(failure_of([&] { update::validated_asset_digest(bad); }) ==
              "The update package has an invalid filename.");
    }
#endif
    for (const std::int64_t size :
         {std::int64_t{0}, std::int64_t{-1}, std::int64_t{2} * 1024 * 1024 * 1024 + 1}) {
        bad = setup_asset();
        bad.size_bytes = size;
        CHECK(failure_of([&] { update::validated_asset_digest(bad); }) ==
              "The update package has an invalid size.");
    }
}

TEST_CASE("a download that is canceled before it starts asks nothing", "[update]") {
    fv::testkit::TempDataRoot root;
    FakeHttpClient client;
    fv::CancelSource cancel;
    cancel.cancel();
    CHECK(failure_of([&] {
              update::download_release_asset(client, github_feed(), setup_asset(), root.path(), {},
                                             cancel.token());
          }) == "The download was canceled.");
    CHECK(client.requests().empty());
}

TEST_CASE("the update folder and the asset API come from the identity and the feed", "[update]") {
    CHECK(update::asset_api_url("example/app", 7) ==
          "https://api.github.com/repos/example/app/releases/assets/7");
    CHECK_FALSE(update::asset_api_url("", 7));
    CHECK_FALSE(update::asset_api_url("example/app", 0));
    CHECK_FALSE(update::asset_api_url("example/app", std::nullopt));
    CHECK(update::cannot_install_message() ==
          "This copy of OpenWhisper_CPP cannot install the Windows setup package.");
}

// The updater's own wording for the shared downloader's failures (app_update.py:
// _DOWNLOAD_MESSAGES, _regular_file_stat), map rows A16-14 and A16-49.
TEST_CASE("an update download fails in the updater's own words", "[update]") {
    fv::testkit::TempDataRoot root;
    const auto asset = setup_asset();
    const auto destination = root.path() / asset.name;
    FakeHttpClient client;
    const auto download = [&] {
        update::download_release_asset(client, github_feed(), asset, root.path());
    };
    SECTION("a folder where the download goes") {
        std::filesystem::create_directories(destination);
        CHECK(failure_of(download) == "The update download path is not a regular file.");
        CHECK(client.requests().empty());
    }
    SECTION("a partial file with a second link") {
        const auto partial = root.path() / (asset.name + ".part");
        write_bytes(partial, "verified");
        std::error_code error;
        std::filesystem::create_hard_link(partial, root.path() / "elsewhere", error);
        if (error) {
            SKIP("hard links are not available here");
        }
        CHECK(failure_of(download) == "The update partial is not a regular file.");
    }
    SECTION("a server that announces another size") {
        client.set_handler([](const fv::http::Request&) {
            return ok(kBody, {{"Content-Length", std::to_string(kBody.size() + 1)}});
        });
        CHECK(failure_of(download) == "The update server returned an unexpected download size.");
    }
    SECTION("a server that sends more than the release lists") {
        client.set_handler([](const fv::http::Request&) {
            return ok(std::string(kBody) + "!", {{"Content-Type", "application/octet-stream"}});
        });
        CHECK(failure_of(download) == "The update server returned more data than expected.");
    }
}
