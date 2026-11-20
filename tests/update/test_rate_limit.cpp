// The API-free fallback while GitHub's API is rate limited, verified assets, and the persisted
// cooldown: OpenWhisper's tests/test_app_update_rate_limit.py. The fake answers with real
// redirects, which the check follows itself, so request lists include the redirect's hop.

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <chrono>
#include <string>

#include "base/testing/fake_clock.h"
#include "settings/testing/memory_store.h"
#include "update/check.h"
#include "update/release_feed.h"
#include "update/schedule.h"
#include "update/testing/fake_feed.h"
#include "update/trust.h"
#include "update_support.h"

namespace update = fv::update;
using fv::Json;
using update::testing::FakeFeed;
using namespace std::chrono_literals;
using namespace fv::update::test;

namespace {

constexpr std::string_view kTag = "v2.7.0";
const std::string kDigest(64, 'b');

std::vector<std::string> names() {
    return {update::setup_asset_name("2.7.0"), update::archive_asset_name("2.7.0")};
}

std::string checksums() {
    std::string text;
    for (const auto& name : names()) {
        text += kDigest + "  " + name + "\n";
    }
    return text;
}

// The `network` fixture: the API refuses with a primary rate limit, the latest-release page
// redirects to the v2.7.0 tag, SHA256SUMS.txt lists both assets, each asset HEAD says 123 bytes.
struct Network {
    FakeFeed feed;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock{at("2026-08-21T12:00:00+00:00")};

    Network() {
        feed.api_error = FakeFeed::rate_limited(clock.utc() + 30min);
        feed.page = feed.tag_page(kTag);
        feed.checksums = checksums();
        feed.asset_length = "123";
    }

    update::ReleaseInfo fetch() {
        return update::fetch_latest_release(feed, feed.config(), settings, clock, {});
    }
    std::string sums() const { return feed.download_base(kTag) + "SHA256SUMS.txt"; }
    std::vector<std::pair<std::string, std::string>> fallback_calls() const {
        return {{feed.urls().latest_release, "GET"},
                {feed.urls().latest_release_page, "HEAD"},
                {feed.tag_page(kTag), "HEAD"},
                {sums(), "GET"}};
    }
};

} // namespace

// from tests/test_app_update_rate_limit.py::test_fallback_retains_verified_windows_assets
TEST_CASE("fallback retains verified windows assets", "[test_app_update_rate_limit]") {
    Network network;
    const auto release = network.fetch();
    REQUIRE(release.setup_asset);
    REQUIRE(release.native_asset);
    CHECK(release.setup_asset->sha256 == kDigest);
    CHECK(release.native_asset->sha256 == kDigest);
    CHECK(release.setup_asset->size_bytes == 123);
    CHECK(release.setup_asset->url == network.feed.download_base(kTag) + names()[0]);
    CHECK(release.html_url == network.feed.tag_page(kTag));
    CHECK(release.notes ==
          "[Read the release notes on GitHub](" + network.feed.tag_page(kTag) + ").");
    CHECK(update::resolve_release_apply_mode(update::InstallChannel::Installer, release, "win32") ==
          update::ApplyMode::Setup);
    // Package bodies are never downloaded during a check: the assets are only asked for heads.
    auto expected = network.fallback_calls();
    for (const auto& name : names()) {
        expected.emplace_back(network.feed.download_base(kTag) + name, "HEAD");
    }
    CHECK(network.feed.calls() == expected);
}

// from tests/test_app_update_rate_limit.py::test_primary_api_cooldown_survives_reloading_settings
TEST_CASE("primary api cooldown survives reloading settings", "[test_app_update_rate_limit]") {
    Network network;
    network.fetch();
    const auto retry_at =
        update::parse_last_check_at(network.settings.value("update_api_retry_at"));
    REQUIRE(retry_at);
    CHECK(update::to_sys_time(*retry_at) >=
          std::chrono::floor<std::chrono::seconds>(network.clock.utc() + 30min));
    fv::MemorySettingsStore reloaded(network.settings.contents());
    update::fetch_latest_release(network.feed, network.feed.config(), reloaded, network.clock, {});
    const auto calls = network.feed.calls();
    CHECK(std::count(
              calls.begin(), calls.end(),
              std::pair<std::string, std::string>{network.feed.urls().latest_release, "GET"}) == 1);
}

// from
// tests/test_app_update_rate_limit.py::test_secondary_rate_limit_sets_cooldown_and_uses_fallback
TEST_CASE("secondary rate limit sets cooldown and uses fallback", "[test_app_update_rate_limit]") {
    for (const auto& [headers, wait] :
         {std::pair<fv::http::Headers, std::chrono::seconds>{{{"Retry-After", "120"}}, 120s},
          std::pair<fv::http::Headers, std::chrono::seconds>{
              {{"Retry-After", "invalid"}, {"X-RateLimit-Reset", "invalid"}}, 60s}}) {
        Network network;
        const auto before = network.clock.utc();
        network.feed.api_error = FakeFeed::http_error(429, "Limited", headers);
        CHECK(network.fetch().version == "2.7.0");
        const auto retry_at =
            update::parse_last_check_at(network.settings.value("update_api_retry_at"));
        REQUIRE(retry_at);
        CHECK(update::to_sys_time(*retry_at) >= before + wait);
    }
}

// from tests/test_app_update_rate_limit.py::test_expired_api_cooldown_allows_api_retry
TEST_CASE("expired api cooldown allows api retry", "[test_app_update_rate_limit]") {
    Network network;
    network.settings.save_setting("update_api_retry_at", iso(network.clock.utc() - 1s));
    network.fetch();
    CHECK(network.feed.calls().at(0) ==
          std::pair<std::string, std::string>{network.feed.urls().latest_release, "GET"});
}

// from tests/test_app_update_rate_limit.py::test_other_api_errors_do_not_start_rate_limit_fallback
TEST_CASE("other api errors do not start rate limit fallback", "[test_app_update_rate_limit]") {
    for (const auto& error : {FakeFeed::http_error(403, "Forbidden"),
                              FakeFeed::http_error(500, "Server Error"), FakeFeed::offline()}) {
        Network network;
        network.feed.api_error = error;
        CHECK_THROWS_AS(network.fetch(), update::AppUpdateError);
        CHECK(network.feed.calls() == std::vector<std::pair<std::string, std::string>>{
                                          {network.feed.urls().latest_release, "GET"}});
        CHECK_FALSE(network.settings.contents().contains("update_api_retry_at"));
    }
}

// from
// tests/test_app_update_rate_limit.py::test_missing_or_untrusted_checksums_allow_notification_only
TEST_CASE("missing or untrusted checksums allow notification only",
          "[test_app_update_rate_limit]") {
    const std::vector<std::optional<std::string>> bodies{
        "",                                                        // empty
        "garbage",                                                 // malformed
        checksums() + checksums(),                                 // duplicate
        "\xff",                                                    // invalid-utf8
        std::string(update::kMaxChecksumBytes + 1, 'x'),           // oversized
        std::nullopt,                                              // missing (404)
        kDigest + "  ../../update.exe\n",                          // unsafe-filename
        kDigest + "  " + update::setup_asset_name("2.6.0") + "\n", // wrong-version
    };
    for (const auto& body : bodies) {
        Network network;
        network.feed.checksums = body;
        const auto release = network.fetch();
        CHECK(release.version == "2.7.0");
        CHECK_FALSE(release.setup_asset);
        CHECK_FALSE(release.native_asset);
        CHECK_FALSE(update::can_apply(update::InstallChannel::Installer, release, "win32"));
        CHECK(network.feed.calls() == network.fallback_calls());
    }
}

// from
// tests/test_app_update_rate_limit.py::test_unknown_or_invalid_asset_sizes_never_enable_install
TEST_CASE("unknown or invalid asset sizes never enable install", "[test_app_update_rate_limit]") {
    const std::vector<std::pair<std::optional<std::string>, int>> cases{
        {std::nullopt, 200},
        {"bad", 200},
        {"0", 200},
        {"-1", 200},
        {std::to_string(update::kMaxAssetBytes + 1), 200},
        {"123", 206},
    };
    for (const auto& [length, status] : cases) {
        Network network;
        network.feed.asset_length = length;
        network.feed.asset_status = status;
        const auto release = network.fetch();
        CHECK_FALSE(release.setup_asset);
        CHECK_FALSE(release.native_asset);
    }
}

// from tests/test_app_update_rate_limit.py::test_fallback_requires_exact_repository_stable_tag
// The redirect itself is followed here, so a hop off github.com is refused as an unsafe redirect
// before the tag is read; Python's test replaced the opener and saw the final URL directly.
TEST_CASE("fallback requires exact repository stable tag", "[test_app_update_rate_limit]") {
    const FakeFeed shape;
    const auto page = shape.tag_page(kTag);
    const auto attacker = "https://github.com/attacker/app/releases/tag/v2.7.0";
    for (const auto& target : {shape.urls().latest_release_page, page + "-rc.1",
                               std::string(attacker), page + "?redirect=1"}) {
        Network network;
        if (target == shape.urls().latest_release_page) {
            // GitHub answers without redirecting: the page itself names no tag.
            network.feed.routes[target] = {};
        } else {
            network.feed.page = target;
            network.feed.routes[target] = {};
        }
        try {
            network.fetch();
            FAIL("no error for " << target);
        } catch (const update::AppUpdateError& error) {
            CHECK(std::string(error.what()) == "GitHub did not report a stable release version.");
        }
        CHECK(network.feed.calls().size() <= 3);
        CHECK_FALSE(network.settings.contents().contains("update_last_check_at"));
    }
    Network network;
    network.feed.page = "https://example.com/" + std::string(shape.repo()) + "/releases/tag/v2.7.0";
    try {
        network.fetch();
        FAIL("no error");
    } catch (const update::AppUpdateError& error) {
        CHECK(std::string(error.what()) ==
              "Could not reach the update server (The update server redirected to an unsafe "
              "URL.).");
    }
    CHECK(network.feed.calls().size() == 2);
}

// from
// tests/test_app_update_rate_limit.py::test_failed_checks_throttle_auto_retry_without_claiming_success
TEST_CASE("failed checks throttle auto retry without claiming success",
          "[test_app_update_rate_limit]") {
    FakeFeed feed;
    feed.api_error = FakeFeed::offline();
    fv::MemorySettingsStore settings;
    fv::FakeClock clock(at("2026-08-21T12:00:00+00:00"));
    const auto config = feed.config();
    CHECK_THROWS_AS(update::check_for_update(feed, config, settings, {}, clock, {}, true),
                    update::AppUpdateError);
    const auto stored = settings.contents();
    CHECK_FALSE(stored.contains("update_last_check_at"));
    const auto failed_at = update::parse_last_check_at(stored.at("update_last_failed_check_at"));
    REQUIRE(failed_at);
    const auto failed = update::to_sys_time(*failed_at);
    const auto installer = update::InstallChannel::Installer;
    CHECK(update::seconds_until_auto_check(stored, failed + 59min, installer, false, true) == 60.0);
    CHECK(update::seconds_until_auto_check(stored, failed + 1h, installer, false, true) == 0.0);
}

// from tests/test_app_update_rate_limit.py::test_loopback_feed_bypasses_production_cooldown
TEST_CASE("loopback feed bypasses production cooldown", "[test_app_update_rate_limit]") {
    const std::string url = "http://127.0.0.1:8765/releases/latest";
    FakeFeed feed;
    feed.routes[url] = FakeFeed::json(Json{{"tag_name", "v2.8.0"}});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    settings.save_setting("update_api_retry_at", iso(clock.utc() + 1h));
    auto config = feed.config();
    config.override_url = url;
    CHECK(update::fetch_latest_release(feed, config, settings, clock, {}).version == "2.8.0");
    CHECK(feed.urls_requested() == std::vector<std::string>{url});
}

// from tests/test_app_update_rate_limit.py::test_head_redirect_preserves_method_and_trust_checks
TEST_CASE("head redirect preserves method and trust checks", "[test_app_update_rate_limit]") {
    FakeFeed feed;
    const auto page = feed.tag_page(kTag);
    feed.page = page;
    const auto opened = update::open_checked(
        feed, update::update_request(feed.config(), feed.urls().latest_release_page, "HEAD"),
        feed.repo(), {});
    REQUIRE(opened);
    CHECK(opened->effective_url == page);
    const auto calls = feed.calls();
    REQUIRE(calls.size() == 2);
    CHECK(calls[1] == std::pair<std::string, std::string>{page, "HEAD"});

    feed.page = "https://example.com/download";
    const auto refused = update::open_checked(
        feed, update::update_request(feed.config(), feed.urls().latest_release_page, "HEAD"),
        feed.repo(), {});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().failure.reason == "The update server redirected to an unsafe URL.");
    CHECK(feed.calls().size() == 3);
}
