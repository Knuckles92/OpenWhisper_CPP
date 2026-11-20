// Release payloads and where they are fetched from: OpenWhisper's tests/test_app_update.py
// (TestParseReleasePayload, the asset half of TestDualAssetsAndApplyMode, TestFetchLatestRelease)
// and the asset-id half of tests/test_app_update_download_fallback.py.

#include <catch2/catch_test_macros.hpp>

#include <string>

#include "base/testing/fake_clock.h"
#include "base/testing/map_env_reader.h"
#include "settings/testing/memory_store.h"
#include "update/check.h"
#include "update/release_feed.h"
#include "update/testing/fake_feed.h"
#include "update/update_contract.h"

namespace update = fv::update;
using fv::Json;
using update::InstallChannel;
using update::testing::FakeFeed;

namespace {

constexpr std::string_view kRepo = "example/app";
constexpr std::string_view kFeed = "http://127.0.0.1:8765/releases/latest";
const std::string kDigest = "43d623f9f0a35d3ab7c8366d30bd721fe20c51bd94f3bc37b453c63fd5f8bd13";

update::FeedConfig github() {
    update::FeedConfig feed;
    feed.source = {update::UpdateSource::Kind::GitHub, std::string(kRepo)};
    return feed;
}

update::FeedConfig with_feed() {
    auto feed = github();
    feed.override_url = std::string(kFeed);
    return feed;
}

std::string download(std::string_view tag, std::string_view name) {
    return "https://github.com/" + std::string(kRepo) + "/releases/download/" + std::string(tag) +
           "/" + std::string(name);
}

// LATEST_RELEASE_FIXTURE: the shape of a real /releases/latest payload.
Json latest_fixture() {
    const auto name = update::setup_asset_name("2.1.1");
    return Json{
        {"tag_name", "v2.1.1"},
        {"html_url", "https://github.com/" + std::string(kRepo) + "/releases/tag/v2.1.1"},
        {"body", "### Fixed\n\n- GPU Acceleration component now activates immediately\n"},
        {"prerelease", false},
        {"draft", false},
        {"assets", Json::array({Json{{"name", name},
                                     {"size", 91717696},
                                     {"digest", "sha256:" + kDigest},
                                     {"browser_download_url", download("v2.1.1", name)}}})}};
}

Json github_asset(const std::string& name, const std::string& version = "2.1.1") {
    return Json{{"name", name},
                {"size", 100},
                {"state", "uploaded"},
                {"browser_download_url", download("v" + version, name)},
                {"digest", "sha256:" + std::string(64, 'a')}};
}

Json payload_with(Json assets) {
    auto payload = latest_fixture();
    payload["assets"] = std::move(assets);
    return payload;
}

} // namespace

// from tests/test_app_update.py::TestParseReleasePayload::test_real_latest_shape
TEST_CASE("real latest shape", "[test_app_update]") {
    const auto release = update::parse_release_payload(github(), latest_fixture());
    CHECK(release.version == "2.1.1");
    CHECK(release.tag_name == "v2.1.1");
    REQUIRE(release.setup_asset);
    CHECK(release.setup_asset->name == update::setup_asset_name("2.1.1"));
    CHECK(release.setup_asset->sha256 == kDigest);
    CHECK(release.setup_asset->size_bytes == 91717696);
    CHECK_FALSE(release.native_asset);
    CHECK(release.asset() == &*release.setup_asset);
    CHECK(release.notes == "### Fixed\n\n- GPU Acceleration component now activates immediately");
}

// from tests/test_app_update.py::TestParseReleasePayload::test_sidecar_zip_asset_is_ignored
TEST_CASE("sidecar zip asset is ignored", "[test_app_update]") {
    auto payload = latest_fixture();
    payload["assets"].insert(payload["assets"].begin(),
                             github_asset("meeting-agent-win_amd64-node22-pi1.zip"));
    const auto release = update::parse_release_payload(github(), payload);
    REQUIRE(release.asset() != nullptr);
    CHECK(release.asset()->name == update::setup_asset_name("2.1.1"));
}

// from
// tests/test_app_update.py::TestParseReleasePayload::test_missing_digest_allows_notify_but_not_apply
TEST_CASE("missing digest allows notify but not apply", "[test_app_update]") {
    auto payload = latest_fixture();
    payload["assets"][0].erase("digest");
    const auto release = update::parse_release_payload(github(), payload);
    REQUIRE(release.asset() != nullptr);
    CHECK_FALSE(release.asset()->sha256);
    CHECK_FALSE(update::can_apply(InstallChannel::Installer, release, "win32"));
}

// from tests/test_app_update.py::TestParseReleasePayload::test_missing_tag_raises
TEST_CASE("missing tag raises", "[test_app_update]") {
    CHECK_THROWS_AS(update::parse_release_payload(github(), Json{{"assets", Json::array()}}),
                    update::AppUpdateError);
}

// from
// tests/test_app_update.py::TestParseReleasePayload::test_an_asset_hosted_off_github_is_ignored
TEST_CASE("an asset hosted off github is ignored", "[test_app_update]") {
    auto payload = latest_fixture();
    payload["assets"][0]["browser_download_url"] =
        "http://127.0.0.1:8765/download/" + update::setup_asset_name("2.1.1");
    CHECK_FALSE(update::parse_release_payload(github(), payload).setup_asset);
}

// from
// tests/test_app_update.py::TestParseReleasePayload::test_feed_override_accepts_assets_from_its_own_origin
TEST_CASE("feed override accepts assets from its own origin", "[test_app_update]") {
    auto payload = latest_fixture();
    const auto local = "http://127.0.0.1:8765/download/" + update::setup_asset_name("2.1.1");
    payload["assets"][0]["browser_download_url"] = local;
    const auto release = update::parse_release_payload(with_feed(), payload);
    REQUIRE(release.setup_asset);
    CHECK(release.setup_asset->url == local);
    // Without any GitHub source, the feed alone still vouches for its own files.
    auto feed_only = with_feed();
    feed_only.source = {};
    CHECK(update::parse_release_payload(feed_only, payload).setup_asset);
}

// from
// tests/test_app_update.py::TestParseReleasePayload::test_feed_override_still_rejects_other_origins
TEST_CASE("feed override still rejects other origins", "[test_app_update]") {
    auto payload = latest_fixture();
    payload["assets"][0]["browser_download_url"] =
        "http://127.0.0.1:9999/download/" + update::setup_asset_name("2.1.1");
    CHECK_FALSE(update::parse_release_payload(with_feed(), payload).setup_asset);
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_prefers_exact_archive_name
TEST_CASE("prefers exact archive name", "[test_app_update]") {
    const auto release = update::parse_release_payload(
        github(), payload_with(Json::array({github_asset(update::setup_asset_name("2.1.1")),
                                            github_asset(update::archive_asset_name("2.1.1")),
                                            github_asset("meeting-agent.zip")})));
    REQUIRE(release.native_asset);
    CHECK(release.native_asset->name == update::archive_asset_name("2.1.1"));
    CHECK(release.setup_asset);
    CHECK(release.asset() == &*release.native_asset);
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_duplicate_archive_is_rejected
TEST_CASE("duplicate archive is rejected", "[test_app_update]") {
    const auto archive = github_asset(update::archive_asset_name("2.1.1"));
    const auto release =
        update::parse_release_payload(github(), payload_with(Json::array({archive, archive})));
    CHECK_FALSE(release.native_asset);
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_wrong_download_url_is_rejected
TEST_CASE("wrong download url is rejected", "[test_app_update]") {
    auto asset = github_asset(update::setup_asset_name("2.1.1"));
    asset["browser_download_url"] = "https://evil.example/" + update::setup_asset_name("2.1.1");
    CHECK_FALSE(
        update::parse_release_payload(github(), payload_with(Json::array({asset}))).setup_asset);
}

// from tests/test_app_update.py::TestDualAssetsAndApplyMode::test_draft_release_has_no_apply_assets
TEST_CASE("draft release has no apply assets", "[test_app_update]") {
    auto payload = payload_with(Json::array({github_asset(update::setup_asset_name("2.1.1"))}));
    payload["draft"] = true;
    const auto release = update::parse_release_payload(github(), payload);
    CHECK_FALSE(release.setup_asset);
    CHECK_FALSE(release.native_asset);
}

// from
// tests/test_app_update.py::TestFetchLatestRelease::test_requests_github_latest_not_the_website
TEST_CASE("requests github latest not the website", "[test_app_update]") {
    FakeFeed github_feed(std::string{kRepo});
    github_feed.latest = latest_fixture();
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const auto release = update::fetch_latest_release(github_feed, github(), settings, clock, {});
    const auto calls = github_feed.calls();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0].first == github_feed.urls().latest_release);
    CHECK(calls[0].first.find("github.com") != std::string::npos);
    CHECK(calls[0].first.find("fiorilabs") == std::string::npos);
    CHECK(release.version == "2.1.1");
    const auto request = github_feed.requests().at(0);
    CHECK(fv::http::find_header(request.headers, "Accept") == "application/vnd.github+json");
    CHECK(fv::http::find_header(request.headers, "User-Agent").has_value());
    CHECK_FALSE(request.follow_redirects);
}

// from tests/test_app_update.py::TestFetchLatestRelease::test_feed_override_replaces_github
TEST_CASE("feed override replaces github", "[test_app_update]") {
    FakeFeed feed(std::string{kRepo});
    feed.routes[std::string(kFeed)] = FakeFeed::json(latest_fixture());
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    update::fetch_latest_release(feed, with_feed(), settings, clock, {});
    CHECK(feed.urls_requested() == std::vector<std::string>{std::string(kFeed)});
}

// from tests/test_app_update.py::TestFetchLatestRelease::test_blank_feed_override_is_ignored
TEST_CASE("blank feed override is ignored", "[test_app_update]") {
    const fv::MapEnvReader env{{update::update_feed_env(), "   "}};
    const auto config = update::feed_config(env, github().source);
    CHECK_FALSE(config.override_url);
    CHECK(update::latest_release_url(config, update::ReleaseTrack::Stable) ==
          update::github_urls(kRepo).latest_release);
}

TEST_CASE("the feed variable is honoured only for an exact loopback feed", "[update]") {
    const auto feed = [](std::string value) {
        const fv::MapEnvReader env{{update::update_feed_env(), std::move(value)}};
        return update::feed_config(env, {}).override_url;
    };
    CHECK(feed(std::string(kFeed)) == std::string(kFeed));
    CHECK(feed("  " + std::string(kFeed) + "\n") == std::string(kFeed));
    CHECK_FALSE(feed("https://example.com:8765/releases/latest"));
    CHECK_FALSE(feed("http://localhost:8765/releases/latest"));
    CHECK_FALSE(feed("http://127.0.0.1/releases/latest"));
    CHECK_FALSE(feed("http://127.0.0.1:8765/releases/latest?x=1"));
    CHECK_FALSE(feed("http://u@127.0.0.1:8765/releases/latest"));
    CHECK_FALSE(update::feed_config(fv::MapEnvReader{}, {}).override_url);
    CHECK(update::update_feed_env() == "OPENWHISPER_CPP_UPDATE_FEED_URL");
}

TEST_CASE("without a source or a feed there is no URL to ask", "[update]") {
    const update::FeedConfig none;
    CHECK_FALSE(update::has_update_source(none));
    try {
        update::latest_release_url(none, update::ReleaseTrack::Stable);
        FAIL("no exception");
    } catch (const update::AppUpdateError& error) {
        CHECK(std::string(error.what()) == "This build of OpenWhisper_CPP has no update source.");
    }
    CHECK(update::latest_release_url(github(), update::ReleaseTrack::Beta) ==
          "https://api.github.com/repos/example/app/releases?per_page=30");
}

// from
// tests/test_app_update_download_fallback.py::test_release_metadata_retains_asset_id_for_each_installer
// (the macOS disk image is other-platform)
TEST_CASE("release metadata retains asset id for each installer",
          "[test_app_update_download_fallback]") {
    for (const auto& name :
         {update::setup_asset_name("2.8.6"), update::archive_asset_name("2.8.6")}) {
        const Json payload{
            {"tag_name", "v2.8.6"},
            {"assets",
             Json::array({Json{{"id", 123456},
                               // The API URL is built from the id, never taken from this field.
                               {"url", "https://evil.example/download"},
                               {"name", name},
                               {"size", 26},
                               {"digest", "sha256:" + std::string(64, 'a')},
                               {"browser_download_url", download("v2.8.6", name)}}})}};
        const auto release = update::parse_release_payload(github(), payload);
        const auto& asset = name.ends_with(".exe") ? release.setup_asset : release.native_asset;
        REQUIRE(asset);
        CHECK(asset->github_asset_id == 123456);
    }
}

// from tests/test_app_update_download_fallback.py::test_invalid_asset_ids_do_not_enable_fallback
TEST_CASE("invalid asset ids do not enable fallback", "[test_app_update_download_fallback]") {
    const auto name = update::setup_asset_name("2.8.6");
    for (const auto& id : {Json(nullptr), Json(true), Json(false), Json(0), Json(-1), Json("123"),
                           Json("../latest"), Json(1.5)}) {
        const auto parsed = update::parse_named_asset(
            github(),
            Json::array({Json{{"id", id},
                              {"name", name},
                              {"size", 26},
                              {"browser_download_url", download("v2.8.6", name)}}}),
            name, "v2.8.6");
        REQUIRE(parsed);
        CHECK_FALSE(parsed->github_asset_id);
    }
}

// from
// tests/test_app_update_download_fallback.py::test_loopback_feed_never_enables_production_fallback
TEST_CASE("loopback feed never enables production fallback",
          "[test_app_update_download_fallback]") {
    const auto name = update::setup_asset_name("2.8.6");
    const auto parsed = update::parse_named_asset(
        with_feed(),
        Json::array({Json{{"id", 123456},
                          {"name", name},
                          {"size", 26},
                          {"browser_download_url", "http://127.0.0.1:8765/" + name}}}),
        name, "v2.8.6");
    REQUIRE(parsed);
    CHECK_FALSE(parsed->github_asset_id);
}

TEST_CASE("a feed that is not JSON, or not a release, is unreadable", "[update]") {
    FakeFeed feed(std::string{kRepo});
    fv::MemorySettingsStore settings;
    fv::FakeClock clock;
    const auto message = [&](fv::http::ScriptedResponse answer) {
        feed.routes[feed.urls().latest_release] = std::move(answer);
        try {
            update::fetch_latest_release(feed, github(), settings, clock, {});
        } catch (const update::AppUpdateError& error) {
            return std::string(error.what());
        }
        return std::string("no error");
    };
    fv::http::ScriptedResponse text;
    text.body = "<html>";
    CHECK(message(text) == "The update server returned a response that could not be read.");
    text.body = "\xff\xfe{}";
    CHECK(message(text) == "The update server returned a response that could not be read.");
    text.body = "[1, 2]";
    CHECK(message(text) == "The update server returned a response that could not be read.");
    text.body = "{}";
    CHECK(message(text) == "The update server did not report a release version.");
    CHECK(message(FakeFeed::http_error(500, "Server Error")) ==
          "The update server returned an error (500 Server Error).");
}
