// The opt-in beta release track: who is offered prereleases, and how. OpenWhisper's
// tests/test_app_update_release_track.py.

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
#include "update_support.h"

namespace update = fv::update;
using fv::Json;
using update::ReleaseTrack;
using update::UpdateStatus;
using update::testing::FakeFeed;
using namespace std::chrono_literals;
using namespace fv::update::test;

namespace {

const std::string kDigest(64, 'c'); // FakeFeed::ReleaseSpec's default digest

// The `github` fixture: latest is the stable 2.8.9, the list holds the 2.9.0 prerelease too, and
// the API-free fallback names the stable tag and checksums its setup.
struct GitHub {
    FakeFeed feed;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock{at("2026-08-21T12:00:00+00:00")};

    GitHub() {
        feed.latest = stable();
        feed.list = Json::array({beta(), stable()});
        feed.asset_length = "4096";
        refresh_checksums();
    }
    Json stable() const { return feed.release({"2.8.9"}); }
    Json beta() const { return feed.release({"2.9.0", true}); }
    // The fallback's SHA256SUMS.txt lists the setup of whatever latest is.
    void refresh_checksums() {
        const auto tag = feed.latest["tag_name"].get<std::string>();
        feed.checksums = kDigest + "  " + update::setup_asset_name(tag.substr(1)) + "\n";
    }
    void opt_in(bool enabled = true) {
        settings.save_setting("update_beta_track_enabled", enabled);
    }
    update::ReleaseInfo fetch(std::optional<ReleaseTrack> track = std::nullopt) {
        return update::fetch_latest_release(feed, feed.config(), settings, clock, {}, track);
    }
    update::UpdateCheckResult check(const std::string& version) {
        const auto config = feed.config();
        const update::InstallProbe build_tree;
        const update::CheckContext context{feed, config, settings, build_tree, clock, {}, version};
        return update::check_for_update(context, false);
    }
    bool requested(const std::string& url) const {
        const auto urls = feed.urls_requested();
        return std::ranges::find(urls, url) != urls.end();
    }
    void rate_limit() { feed.api_error = FakeFeed::rate_limited(clock.utc() + 30min); }
};

update::FeedConfig github_config() {
    return FakeFeed().config();
}

} // namespace

// from tests/test_app_update_release_track.py::TestReleaseTrackSetting::test_default_is_stable
TEST_CASE("default is stable", "[test_app_update_release_track]") {
    fv::MemorySettingsStore settings;
    CHECK(update::release_track(settings) == ReleaseTrack::Stable);
}

// from tests/test_app_update_release_track.py::TestReleaseTrackSetting::test_opt_in_selects_beta
TEST_CASE("opt in selects beta", "[test_app_update_release_track]") {
    fv::MemorySettingsStore settings;
    settings.save_setting("update_beta_track_enabled", true);
    CHECK(update::release_track(settings) == ReleaseTrack::Beta);
}

// from
// tests/test_app_update_release_track.py::TestReleaseTrackSetting::test_non_bool_value_is_stable
TEST_CASE("non bool value is stable", "[test_app_update_release_track]") {
    CHECK(update::release_track(Json{{"update_beta_track_enabled", "yes"}}) ==
          ReleaseTrack::Stable);
}

// from
// tests/test_app_update_release_track.py::TestReleaseTrackSetting::test_unreadable_settings_are_stable
TEST_CASE("unreadable settings are stable", "[test_app_update_release_track]") {
    fv::MemorySettingsStore settings;
    settings.file().set_bytes("{not json");
    CHECK(update::release_track(settings) == ReleaseTrack::Stable);
}

// from
// tests/test_app_update_release_track.py::TestStableTrackNeverSeesPrereleases::test_stable_reads_only_latest_endpoint
TEST_CASE("stable reads only latest endpoint", "[test_app_update_release_track]") {
    GitHub github;
    const auto release = github.fetch();
    CHECK(release.version == "2.8.9");
    CHECK_FALSE(release.prerelease);
    CHECK(github.feed.urls_requested() ==
          std::vector<std::string>{github.feed.urls().latest_release});
}

// from
// tests/test_app_update_release_track.py::TestStableTrackNeverSeesPrereleases::test_stable_ignores_a_newer_prerelease
TEST_CASE("stable ignores a newer prerelease", "[test_app_update_release_track]") {
    GitHub github;
    const auto result = github.check("2.8.9");
    CHECK(result.status == UpdateStatus::UpToDate);
    CHECK(result.release_track == ReleaseTrack::Stable);
    CHECK_FALSE(github.requested(github.feed.urls().release_list));
}

// from
// tests/test_app_update_release_track.py::TestStableTrackNeverSeesPrereleases::test_prerelease_payload_is_not_installable_off_the_beta_track
TEST_CASE("prerelease payload is not installable off the beta track",
          "[test_app_update_release_track]") {
    const FakeFeed feed;
    const auto release =
        update::parse_release_payload(feed.config(), feed.release({"2.9.0", true}));
    CHECK(release.prerelease);
    CHECK_FALSE(release.setup_asset);
}

// from
// tests/test_app_update_release_track.py::TestStableTrackNeverSeesPrereleases::test_drafts_are_never_installable
TEST_CASE("drafts are never installable", "[test_app_update_release_track]") {
    const FakeFeed feed;
    FakeFeed::ReleaseSpec spec{"3.0.0", true};
    spec.draft = true;
    const auto draft = feed.release(spec);
    CHECK_FALSE(update::parse_release_payload(feed.config(), draft, true).setup_asset);
    CHECK_FALSE(update::parse_release_payload(feed.config(), draft).setup_asset);
}

// from tests/test_app_update_release_track.py::TestBetaTrackSelection::test_newer_prerelease_wins
TEST_CASE("newer prerelease wins", "[test_app_update_release_track]") {
    const FakeFeed feed;
    const auto release = update::select_newest_release(
        feed.config(), Json::array({feed.release({"2.8.9"}), feed.release({"2.9.0", true})}));
    CHECK(release.version == "2.9.0");
    CHECK(release.prerelease);
    REQUIRE(release.setup_asset);
    CHECK(release.setup_asset->sha256 == kDigest);
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_newer_stable_wins_whatever_the_list_order
TEST_CASE("newer stable wins whatever the list order", "[test_app_update_release_track]") {
    const FakeFeed feed;
    const auto beta = feed.release({"2.9.0", true});
    const auto newer_stable = feed.release({"2.9.1"});
    for (const auto& order :
         {Json::array({beta, newer_stable}), Json::array({newer_stable, beta})}) {
        const auto release = update::select_newest_release(feed.config(), order);
        CHECK(release.version == "2.9.1");
        CHECK_FALSE(release.prerelease);
    }
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_drafts_and_suffixed_tags_are_skipped
TEST_CASE("drafts and suffixed tags are skipped", "[test_app_update_release_track]") {
    const FakeFeed feed;
    FakeFeed::ReleaseSpec draft{"3.0.0"};
    draft.draft = true;
    auto suffixed = feed.release({"3.0.0", true});
    suffixed["tag_name"] = "v3.0.0-rc.1";
    CHECK(update::select_newest_release(feed.config(), Json::array({feed.release(draft), suffixed,
                                                                    feed.release({"2.9.0", true})}))
              .version == "2.9.0");
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_stable_wins_a_version_tie
TEST_CASE("stable wins a version tie", "[test_app_update_release_track]") {
    // The 2.6 line used two-digit patches: v2.6.01 and v2.6.1 compare equal.
    const FakeFeed feed;
    const auto release = update::select_newest_release(
        feed.config(), Json::array({feed.release({"2.6.01", true}), feed.release({"2.6.1"})}));
    CHECK(release.tag_name == "v2.6.1");
    CHECK_FALSE(release.prerelease);
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_no_usable_release_is_an_error
TEST_CASE("no usable release is an error", "[test_app_update_release_track]") {
    for (const auto& payload :
         {Json::array(), Json{{"message", "x"}}, Json::array({Json{{"draft", true}}})}) {
        CHECK_THROWS_AS(update::select_newest_release(github_config(), payload),
                        update::AppUpdateError);
    }
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_prerelease_assets_keep_url_allow_listing
TEST_CASE("prerelease assets keep url allow listing", "[test_app_update_release_track]") {
    const FakeFeed feed;
    FakeFeed::ReleaseSpec spec{"2.9.0", true};
    spec.url = "https://example.com/" + update::setup_asset_name("2.9.0");
    CHECK_FALSE(update::select_newest_release(feed.config(), Json::array({feed.release(spec)}))
                    .setup_asset);
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_prerelease_without_digest_is_notify_only
TEST_CASE("prerelease without digest is notify only", "[test_app_update_release_track]") {
    const FakeFeed feed;
    FakeFeed::ReleaseSpec spec{"2.9.0", true};
    spec.digest.reset();
    const auto release =
        update::select_newest_release(feed.config(), Json::array({feed.release(spec)}));
    REQUIRE(release.setup_asset);
    CHECK_FALSE(release.setup_asset->sha256);
    CHECK(update::resolve_release_apply_mode(update::InstallChannel::Installer, release, "win32") ==
          update::ApplyMode::NotifyOnly);
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_verified_prerelease_can_be_applied
TEST_CASE("verified prerelease can be applied", "[test_app_update_release_track]") {
    const FakeFeed feed;
    const auto release =
        update::select_newest_release(feed.config(), Json::array({feed.release({"2.9.0", true})}));
    CHECK(update::resolve_release_apply_mode(update::InstallChannel::Installer, release, "win32") ==
          update::ApplyMode::Setup);
}

// from
// tests/test_app_update_release_track.py::TestBetaTrackSelection::test_beta_reads_the_release_list
TEST_CASE("beta reads the release list", "[test_app_update_release_track]") {
    GitHub github;
    github.opt_in();
    CHECK(github.fetch().version == "2.9.0");
    CHECK(github.feed.urls_requested() ==
          std::vector<std::string>{github.feed.urls().release_list});
}

// from
// tests/test_app_update_release_track.py::TestBetaRateLimitFallback::test_rate_limited_beta_check_falls_back_to_stable
TEST_CASE("rate limited beta check falls back to stable", "[test_app_update_release_track]") {
    GitHub github;
    github.opt_in();
    github.rate_limit();
    const auto release = github.fetch();
    // GitHub has no API-free prerelease pointer: the newest stable, verified.
    CHECK(release.version == "2.8.9");
    CHECK_FALSE(release.prerelease);
    REQUIRE(release.setup_asset);
    CHECK(release.setup_asset->sha256 == kDigest);
    const auto urls = github.feed.urls_requested();
    REQUIRE(urls.size() >= 2);
    CHECK(urls[0] == github.feed.urls().release_list);
    CHECK(urls[1] == github.feed.urls().latest_release_page);
    CHECK(github.settings.contents().contains("update_api_retry_at"));
}

// from
// tests/test_app_update_release_track.py::TestBetaRateLimitFallback::test_cooldown_skips_the_api_until_it_expires
TEST_CASE("cooldown skips the api until it expires", "[test_app_update_release_track]") {
    GitHub github;
    github.opt_in();
    github.rate_limit();
    github.fetch();
    const auto before = github.feed.urls_requested().size();
    github.feed.api_error.reset();
    CHECK(github.fetch().version == "2.8.9");
    const auto urls = github.feed.urls_requested();
    CHECK(std::find(urls.begin() + static_cast<std::ptrdiff_t>(before), urls.end(),
                    github.feed.urls().release_list) == urls.end());

    github.settings.save_setting("update_api_retry_at", iso(github.clock.utc() - 1s));
    CHECK(github.fetch().version == "2.9.0");
}

// from
// tests/test_app_update_release_track.py::TestBetaRateLimitFallback::test_other_http_errors_are_not_masked
TEST_CASE("other http errors are not masked", "[test_app_update_release_track]") {
    GitHub github;
    github.opt_in();
    github.feed.api_error = FakeFeed::http_error(500, "Server Error");
    CHECK_THROWS_AS(github.fetch(), update::AppUpdateError);
    CHECK_FALSE(github.requested(github.feed.urls().latest_release_page));
}

// from tests/test_app_update_release_track.py::TestSwitchingTracks::test_join_then_leave
TEST_CASE("join then leave", "[test_app_update_release_track]") {
    GitHub github;
    CHECK(github.check("2.8.9").status == UpdateStatus::UpToDate);

    github.opt_in();
    const auto joined = github.check("2.8.9");
    CHECK(joined.status == UpdateStatus::UpdateAvailable);
    REQUIRE(joined.release);
    CHECK(joined.release->version == "2.9.0");
    CHECK(joined.release_track == ReleaseTrack::Beta);

    // After installing the beta and leaving the track, nothing is offered: no downgrade to
    // 2.8.9, and no further prereleases.
    github.opt_in(false);
    github.feed.list =
        Json::array({github.feed.release({"2.9.1", true}), github.beta(), github.stable()});
    const auto left = github.check("2.9.0");
    CHECK(left.status == UpdateStatus::Development);
    REQUIRE(left.release);
    CHECK(left.release->version == "2.8.9");
    CHECK(left.release_track == ReleaseTrack::Stable);
}

// from
// tests/test_app_update_release_track.py::TestSwitchingTracks::test_former_beta_user_rejoins_stable_on_promotion
TEST_CASE("former beta user rejoins stable on promotion", "[test_app_update_release_track]") {
    GitHub github;
    // The maintainer promotes the same 2.9.0 build: equal, so up to date.
    github.feed.latest = github.feed.release({"2.9.0"});
    CHECK(github.check("2.9.0").status == UpdateStatus::UpToDate);
    // The next stable release is offered as usual.
    github.feed.latest = github.feed.release({"2.9.1"});
    const auto result = github.check("2.9.0");
    CHECK(result.status == UpdateStatus::UpdateAvailable);
    REQUIRE(result.release);
    CHECK(result.release->version == "2.9.1");
}

// from
// tests/test_app_update_release_track.py::TestSwitchingTracks::test_switch_makes_the_next_automatic_check_due
TEST_CASE("switch makes the next automatic check due", "[test_app_update_release_track]") {
    fv::MemorySettingsStore settings;
    const auto now = at("2026-08-21T12:00:00.500000+00:00");
    const auto retry_at = iso(now + 5min);
    settings.update(Json{{"update_last_check_at", iso(now)}, {"update_api_retry_at", retry_at}});
    const auto installer = update::InstallChannel::Installer;
    CHECK(*update::seconds_until_auto_check(*settings.snapshot(), now, installer, false, true) >
          0.0);
    update::release_track_changed(settings);
    const auto stored = settings.contents();
    CHECK_FALSE(stored.contains("update_last_check_at"));
    // The rate-limit cooldown is kept: switching is no way around it.
    CHECK(stored.at("update_api_retry_at") == retry_at);
    CHECK(update::seconds_until_auto_check(stored, now, installer, false, true) == 0.0);
}

// from
// tests/test_app_update_release_track.py::TestLoopbackFeed::test_feed_prerelease_installable_only_on_beta
TEST_CASE("feed prerelease installable only on beta", "[test_app_update_release_track]") {
    const std::string url = "http://127.0.0.1:8765/releases/latest";
    FakeFeed feed;
    FakeFeed::ReleaseSpec spec{"2.9.0", true};
    spec.url = "http://127.0.0.1:8765/download/" + update::setup_asset_name("2.9.0");
    feed.routes[url] = FakeFeed::json(feed.release(spec));
    auto config = feed.config();
    config.override_url = url;
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    CHECK_FALSE(
        update::fetch_latest_release(feed, config, settings, clock, {}, ReleaseTrack::Stable)
            .setup_asset);
    const auto beta =
        update::fetch_latest_release(feed, config, settings, clock, {}, ReleaseTrack::Beta);
    CHECK(beta.setup_asset);
    CHECK(beta.prerelease);
}

// The C++ half of tests/test_app_update_release_track.py::test_soak_feed_can_serve_a_prerelease
// (M3.3 ports the feed server's half): the document tools/release/serve_update_feed.py's
// build_payload serves, read with that feed configured so that its asset URL is trusted. A
// prerelease then has no setup asset on the stable track only because of the track.
TEST_CASE("a soak feed's prerelease is never installable on the stable track", "[update]") {
    const std::string base = "http://127.0.0.1:8765";
    const auto name = update::setup_asset_name("2.9.0");
    const auto served = [&](bool prerelease) {
        Json asset{{"name", name},
                   {"size", 5},
                   {"state", "uploaded"},
                   {"digest", "sha256:" + kDigest},
                   {"browser_download_url", base + "/download/" + name}};
        return Json{{"tag_name", "v2.9.0"},
                    {"name", "FioriVoce 2.9.0"},
                    {"html_url", base + "/releases/tag/v2.9.0"},
                    {"body", "n"},
                    {"draft", false},
                    {"prerelease", prerelease},
                    {"assets", Json::array({asset})}};
    };
    update::FeedConfig config;
    config.override_url = base + "/releases/latest";

    const auto stable = update::parse_release_payload(config, served(false));
    CHECK_FALSE(stable.prerelease);
    REQUIRE(stable.setup_asset);
    CHECK(stable.setup_asset->url == base + "/download/" + name);
    CHECK(stable.setup_asset->sha256 == kDigest);

    const auto beta = served(true);
    const auto on_stable = update::parse_release_payload(config, beta);
    CHECK(on_stable.prerelease);
    CHECK(on_stable.version == "2.9.0");
    CHECK_FALSE(on_stable.setup_asset);
    CHECK_FALSE(on_stable.native_asset);
    CHECK(update::resolve_release_apply_mode(update::InstallChannel::Installer, on_stable,
                                             "win32") == update::ApplyMode::NotifyOnly);
    CHECK(update::parse_release_payload(config, beta, true).setup_asset);

    // Through a check on a registered install: the stable track is told of the beta and offered
    // nothing to install; the beta track may install it.
    FakeFeed feed;
    feed.routes[*config.override_url] = FakeFeed::json(beta);
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    update::InstallProbe install;
    install.registered = true;
    install.install_dir = install.running_dir = "C:/Programs/App";
    const update::CheckContext context{feed, config, settings, install, clock, {}, "2.8.9"};
    const auto offered = update::check_for_update(context, false);
    CHECK(offered.status == UpdateStatus::UpdateAvailable);
    CHECK_FALSE(offered.can_apply);
    CHECK(offered.apply_mode == update::ApplyMode::NotifyOnly);
    settings.save_setting("update_beta_track_enabled", true);
    const auto beta_offer = update::check_for_update(context, false);
    CHECK(beta_offer.release_track == ReleaseTrack::Beta);
#ifdef _WIN32
    CHECK(beta_offer.can_apply);
    CHECK(beta_offer.apply_mode == update::ApplyMode::Setup);
#endif
}

TEST_CASE("a track change that cannot be saved is logged, not thrown", "[update]") {
    fv::MemorySettingsStore settings(Json{{"update_last_check_at", "2026-08-21T12:00:00+00:00"}});
    settings.file().fail_next_replace("disk full");
    CHECK_NOTHROW(update::release_track_changed(settings));
    CHECK(settings.contents().contains("update_last_check_at"));
}
