// check_for_update, its error wording and persistence: OpenWhisper's tests/test_app_update.py
// (TestNetworkErrorCopy, TestCheckForUpdate, TestPersistPromptChoices), the acceptance check of
// the update-feed package, and a check against a loopback feed through the real HTTP client.

#include <catch2/catch_test_macros.hpp>

#include <cctype>
#include <optional>
#include <string>

#include "../http/loopback_server.h"
#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "download_support.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "update/check.h"
#include "update/release_feed.h"
#include "update/schedule.h"
#include "update/testing/fake_feed.h"
#include "update/trust.h"
#include "update_support.h"

namespace update = fv::update;
using fv::Json;
using fv::http::FakeHttpClient;
using fv::http::ScriptedResponse;
using fv::testing::LoopbackServer;
using update::testing::FakeFeed;
using namespace fv::update::test;

namespace {

update::InstallProbe registered_install() {
    update::InstallProbe probe;
    probe.registered = true;
    probe.install_dir = "C:/Programs/App";
    probe.running_dir = "C:/Programs/App";
    return probe;
}

// _check_failure: the message fetch_latest_release raises when every request fails with
// `answer`.
std::string check_failure(ScriptedResponse answer) {
    FakeHttpClient client;
    client.set_handler([answer](const fv::http::Request&) { return answer; });
    FakeFeed shape;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    try {
        update::fetch_latest_release(client, shape.config(), settings, clock, {});
    } catch (const update::AppUpdateError& error) {
        return error.what();
    }
    FAIL("no AppUpdateError");
    return {};
}

bool contains(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

std::string lower(std::string text) {
    for (auto& c : text) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return text;
}

} // namespace

// from tests/test_app_update.py::TestNetworkErrorCopy::test_http_403_is_rate_limit
TEST_CASE("http 403 is rate limit", "[test_app_update]") {
    const auto message =
        check_failure(FakeFeed::http_error(403, "Forbidden", {{"X-RateLimit-Remaining", "0"}}));
    CHECK(contains(lower(message), "rate-limited"));
    CHECK(contains(message, FakeFeed().urls().releases_page));
    CHECK(message == update::rate_limit_message("example/app"));
}

// from
// tests/test_app_update.py::TestNetworkErrorCopy::test_other_http_403_is_not_mislabeled_as_rate_limit
TEST_CASE("other http 403 is not mislabeled as rate limit", "[test_app_update]") {
    const auto message = check_failure(FakeFeed::http_error(403, "Forbidden"));
    CHECK(message == "The update server returned an error (403 Forbidden).");
    CHECK_FALSE(contains(lower(message), "rate-limited"));
}

// from tests/test_app_update.py::TestNetworkErrorCopy::test_http_429_is_rate_limit
TEST_CASE("http 429 is rate limit", "[test_app_update]") {
    CHECK(contains(lower(check_failure(FakeFeed::http_error(429, "Too Many Requests"))),
                   "rate-limited"));
}

// from tests/test_app_update.py::TestNetworkErrorCopy::test_other_http_error_keeps_status_code
TEST_CASE("other http error keeps status code", "[test_app_update]") {
    const auto message = check_failure(FakeFeed::http_error(500, "Server Error"));
    CHECK(contains(message, "500"));
    CHECK_FALSE(contains(lower(message), "rate-limited"));
}

// from
// tests/test_app_update.py::TestNetworkErrorCopy::test_wrapped_certificate_failure_gives_guidance
TEST_CASE("wrapped certificate failure gives guidance", "[test_app_update]") {
    const auto message = check_failure(FakeFeed::certificate_failure());
    CHECK(contains(message, "update server's certificate could not be verified"));
    CHECK(contains(message,
                   "allow api.github.com, github.com and release-assets.githubusercontent.com."));
    CHECK_FALSE(contains(message, "CERTIFICATE_VERIFY_FAILED"));
}

// from tests/test_app_update.py::TestNetworkErrorCopy::test_failure_status_distinguishes_rate_limit
TEST_CASE("failure status distinguishes rate limit", "[test_app_update]") {
    CHECK(update::update_check_failure_status("GitHub rate-limited this update check.") ==
          "Could not check for updates (GitHub rate limit).");
    CHECK(update::update_check_failure_status("Could not reach the update server.") ==
          "Could not check for updates.");
}

TEST_CASE("an unreachable server and an untrusted hop are worded as urllib's errors", "[update]") {
    CHECK(check_failure(FakeFeed::offline()) == "Could not reach the update server (offline).");
    CHECK(check_failure(FakeFeed::redirect("https://example.com/elsewhere")) ==
          "Could not reach the update server (The update server redirected to an unsafe URL.).");
}

// from tests/test_app_update.py::TestCheckForUpdate::test_builds_result_and_persists_timestamp
// (OpenWhisper ran it on a git checkout; a development build has no git summary to report)
TEST_CASE("builds result and persists timestamp", "[test_app_update]") {
    FakeFeed feed;
    feed.latest = feed.release({"2.1.1"});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock(at("2026-08-21T12:00:00.250000+00:00"));
    const auto config = feed.config();
    const update::InstallProbe build_tree;
    const update::CheckContext context{feed, config, settings, build_tree, clock, {}, "2.1.1"};
    const auto result = update::check_for_update(context);
    CHECK(result.status == update::UpdateStatus::UpToDate);
    CHECK(result.channel == update::InstallChannel::Source);
    CHECK_FALSE(result.can_apply);
    CHECK(result.apply_mode == update::ApplyMode::NotifyOnly);
    CHECK(settings.value("update_last_check_at") == "2026-08-21T12:00:00.250000+00:00");
    CHECK_FALSE(settings.contents().contains("update_last_failed_check_at"));
}

// from tests/test_app_update.py::TestCheckForUpdate::test_behind_latest_is_update_available
TEST_CASE("behind latest is update available", "[test_app_update]") {
    FakeFeed feed;
    feed.latest = feed.release({"2.1.1"});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const auto config = feed.config();
    const update::InstallProbe build_tree;
    const update::CheckContext context{feed, config, settings, build_tree, clock, {}, "2.0.0"};
    const auto result = update::check_for_update(context, false);
    CHECK(result.status == update::UpdateStatus::UpdateAvailable);
    REQUIRE(result.release);
    CHECK(result.release->version == "2.1.1");
    CHECK(result.current_version == "2.0.0");
    CHECK(settings.contents() == Json::object());
}

// from tests/test_app_update.py::TestPersistPromptChoices::test_writes_independent_prefs
TEST_CASE("writes independent prefs", "[test_app_update]") {
    fv::MemorySettingsStore settings;
    update::persist_prompt_choices(settings, false, true);
    const auto saved = settings.contents();
    CHECK(saved.at("update_notify_enabled") == false);
    CHECK(saved.at("update_check_enabled") == true);
    CHECK(settings.file().writes() == 1);
}

TEST_CASE("a check on a registered install offers a verified setup", "[update]") {
    FakeFeed feed;
    feed.latest = feed.release({"0.2.0"});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock(at("2026-08-21T12:00:00.123456+00:00"));
    const auto result = update::check_for_update(feed, feed.config(), settings,
                                                 registered_install(), clock, {}, true);
    CHECK(result.status == update::UpdateStatus::UpdateAvailable);
    CHECK(result.channel == update::InstallChannel::Installer);
#ifdef _WIN32
    CHECK(result.can_apply);
    CHECK(result.apply_mode == update::ApplyMode::Setup);
#else
    CHECK(result.apply_mode == update::ApplyMode::NotifyOnly);
#endif
    REQUIRE(result.release);
    REQUIRE(result.release->setup_asset);
    CHECK(result.release->setup_asset->sha256 == std::string(64, 'c'));
    CHECK(result.release_track == update::ReleaseTrack::Stable);
    // datetime.now(timezone.utc).isoformat()
    CHECK(settings.value("update_last_check_at") == "2026-08-21T12:00:00.123456+00:00");
}

// from
// tests/test_app_update.py::TestDualAssetsAndApplyMode::test_native_mode_requires_hkcu_registration
TEST_CASE("native mode requires hkcu registration", "[test_app_update]") {
    auto release = release_with(setup_asset());
    auto archive = setup_asset();
    archive.name = update::archive_asset_name(release.version);
    archive.url = download_url(release.version, archive.name);
    release.native_asset = archive;
    const std::optional<update::ReleaseInfo> found = release;
    const auto per_user = installed_probe();

    // Without an eligible per-user registration the setup is the way, even with an archive.
    CHECK(update::resolve_release_apply_mode(per_user, found, false, "win32") ==
          update::ApplyMode::Setup);
    CHECK(update::resolve_release_apply_mode(per_user, found, true, "win32") ==
          update::ApplyMode::Native);

    // test_hklm_registration_uses_setup: an all-users install is never native.
    auto all_users = per_user;
    all_users.all_users = true;
    CHECK(update::resolve_release_apply_mode(all_users, found, true, "win32") ==
          update::ApplyMode::Setup);
    // A copy that is not the registered install only notifies; so does every other system.
    auto elsewhere = per_user;
    elsewhere.running_dir = "D:/build/bin";
    CHECK(update::resolve_release_apply_mode(elsewhere, found, true, "win32") ==
          update::ApplyMode::NotifyOnly);
    CHECK(update::resolve_release_apply_mode(update::InstallProbe{}, found, true, "win32") ==
          update::ApplyMode::NotifyOnly);
    CHECK(update::resolve_release_apply_mode(per_user, found, true, "linux") ==
          update::ApplyMode::NotifyOnly);
    CHECK(update::resolve_release_apply_mode(per_user, found, true, "darwin") ==
          update::ApplyMode::NotifyOnly);

    // An archive that is not ready (no digest) leaves the setup; without either, a notice.
    auto unverified = release;
    unverified.native_asset->sha256.reset();
    CHECK(update::resolve_release_apply_mode(per_user, unverified, true, "win32") ==
          update::ApplyMode::Setup);
    unverified.setup_asset.reset();
    CHECK(update::resolve_release_apply_mode(per_user, unverified, true, "win32") ==
          update::ApplyMode::NotifyOnly);
    CHECK(update::resolve_release_apply_mode(per_user, std::nullopt, true, "win32") ==
          update::ApplyMode::NotifyOnly);
}

TEST_CASE("without an update source a check fails before any request", "[update]") {
    FakeFeed feed;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const update::FeedConfig none;
    try {
        update::check_for_update(feed, none, settings, registered_install(), clock, {}, true);
        FAIL("no exception");
    } catch (const update::AppUpdateError& error) {
        CHECK(std::string(error.what()) == "This build of FioriVoce has no update source.");
    }
    CHECK(feed.calls().empty());
    CHECK(settings.contents() == Json::object());
}

TEST_CASE("a failed check stores only the failure time", "[update]") {
    FakeFeed feed;
    feed.api_error = FakeFeed::offline();
    fv::MemorySettingsStore settings;
    fv::FakeClock clock(at("2026-08-21T12:00:00+00:00"));
    CHECK_THROWS_AS(update::check_for_update(feed, feed.config(), settings, registered_install(),
                                             clock, {}, true),
                    update::AppUpdateError);
    CHECK(settings.contents() ==
          Json{{"update_last_failed_check_at", "2026-08-21T12:00:00+00:00"}});
    // Without persist nothing is written either way.
    fv::MemorySettingsStore untouched;
    CHECK_THROWS_AS(update::check_for_update(feed, feed.config(), untouched, registered_install(),
                                             clock, {}, false),
                    update::AppUpdateError);
    CHECK(untouched.contents() == Json::object());
}

TEST_CASE("a timestamp that cannot be written does not fail the check", "[update]") {
    FakeFeed feed;
    feed.latest = feed.release({"0.2.0"});
    fv::MemorySettingsStore settings;
    settings.file().fail_next_replace("disk full");
    fv::FakeClock clock;
    const auto config = feed.config();
    const update::InstallProbe build_tree;
    const update::CheckContext context{feed, config, settings, build_tree, clock, {}, "0.1.0"};
    CHECK(update::check_for_update(context).status == update::UpdateStatus::UpdateAvailable);
    CHECK(settings.contents() == Json::object());
}

TEST_CASE("a check reads a loopback feed through the real HTTP client", "[update]") {
    std::string served_setup;
    LoopbackServer server([&](const LoopbackServer::Request& request,
                              LoopbackServer::Reply& reply) {
        if (request.target != "/releases/latest") {
            reply.write(LoopbackServer::response(404, "Not Found", {}, ""));
            return;
        }
        const auto name = update::setup_asset_name("0.2.0");
        const Json payload{
            {"tag_name", "v0.2.0"},
            {"html_url", served_setup + "/notes"},
            {"body", "Notes"},
            {"prerelease", false},
            {"assets",
             Json::array({Json{{"name", name},
                               {"size", 4096},
                               {"digest", "sha256:" + std::string(64, 'd')},
                               {"browser_download_url", served_setup + "/download/" + name}}})}};
        reply.write(LoopbackServer::response(200, "OK", {{"Content-Type", "application/json"}},
                                             payload.dump()));
    });
    served_setup = "http://127.0.0.1:" + std::to_string(server.port());
    fv::http::CurlClientOptions options;
    options.proxy = [](std::string_view) {
        return std::optional<std::string>();
    };
    const auto client = fv::http::make_curl_client(options);
    const fv::MapEnvReader env{{update::update_feed_env(), server.url("/releases/latest")}};
    const auto feed = update::feed_config(env, {});
    REQUIRE(feed.override_url);
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const auto install = registered_install();
    const update::CheckContext context{*client, feed, settings, install, clock, {}, "0.1.0"};
    const auto result = update::check_for_update(context);
    CHECK(result.status == update::UpdateStatus::UpdateAvailable);
    REQUIRE(result.release);
    REQUIRE(result.release->setup_asset);
    CHECK(result.release->setup_asset->url ==
          served_setup + "/download/" + update::setup_asset_name("0.2.0"));
#ifdef _WIN32
    CHECK(result.apply_mode == update::ApplyMode::Setup);
#endif
    REQUIRE(server.requests().size() == 1);
    CHECK(server.requests()[0].header("Accept") == "application/vnd.github+json");
    CHECK(server.requests()[0].header("User-Agent").value_or("").starts_with("FioriVoce/"));
}
