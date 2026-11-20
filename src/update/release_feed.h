#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"
#include "base/clock.h"
#include "base/env.h"
#include "base/json.h"
#include "update/check.h"
#include "update/trust.h"

namespace fv {
class SettingsStore;
}

// Release metadata (services/app_update.py: parse_release_payload ... _release_asset_from_
// checksum): the GitHub release API, its API-free fallback while the API is rate limited, and
// the loopback soak feed that replaces both for tests and the release demo.
namespace fv::update {

// Largest asset an update may download.
inline constexpr std::int64_t kMaxAssetBytes = 2LL * 1024 * 1024 * 1024;
// Largest SHA256SUMS.txt read.
inline constexpr std::size_t kMaxChecksumBytes = 64 * 1024;

// OPENWHISPER_CPP_UPDATE_FEED_URL ("<env_prefix>UPDATE_FEED_URL").
std::string update_feed_env();

// The release identity's source, and the feed override when the variable holds an exact
// loopback feed URL (is_loopback_feed_url); any other non-blank value is logged
// ("Ignoring unsafe OPENWHISPER_CPP_UPDATE_FEED_URL value") and ignored.
FeedConfig feed_config(const EnvReader&);
FeedConfig feed_config(const EnvReader&, UpdateSource source);

// The GitHub endpoints of `repo`.
struct GitHubUrls {
    std::string latest_release;      // https://api.github.com/repos/<repo>/releases/latest
    std::string release_list;        // ...releases?per_page=30 (newest first, prereleases too)
    std::string releases_page;       // https://github.com/<repo>/releases
    std::string latest_release_page; // ...releases/latest, which redirects to the stable tag
};
GitHubUrls github_urls(std::string_view repo);
// The configured repository, "" without a GitHub source.
std::string_view feed_repo(const FeedConfig&);

// Whether a check has anywhere to ask: a GitHub source or a feed override.
bool has_update_source(const FeedConfig&);
// "This build of OpenWhisper_CPP has no update source."
std::string no_update_source_message();

// The URL a check reads: the feed override (logged), else the release list on the beta track,
// else the latest stable release. Throws AppUpdateError(no_update_source_message()) when there
// is neither.
std::string latest_release_url(const FeedConfig&, ReleaseTrack);

// https://github.com/<repo>/releases/download/<tag>/<filename>; nullopt without a repository.
std::optional<std::string> expected_asset_url(std::string_view repo, std::string_view tag_name,
                                              std::string_view filename);

// GitHub's exact download URL, or (with a feed override) the same file name on the feed's own
// origin. Throws py::ValueError where urlsplit does.
bool asset_url_allowed(const FeedConfig&, std::string_view url, std::string_view tag_name,
                       std::string_view expected_name);

// "sha256:<64 hex>" as lower-case hex; nullopt for anything else.
std::optional<std::string> digest_to_sha256(const Json& raw);

// The one asset named exactly `expected_name` whose state is unset or "uploaded", whose URL is
// allowed and whose size is in (0, kMaxAssetBytes]; nullopt when there is none or more than one.
// The asset id is kept only for a positive JSON integer and GitHub's exact URL.
std::optional<ReleaseAsset> parse_named_asset(const FeedConfig&, const Json& assets,
                                              std::string_view expected_name,
                                              std::string_view tag_name);

// One GitHub release object. A draft exposes no assets, nor does a prerelease unless
// `allow_prerelease` (the beta track). Throws AppUpdateError("The update server did not report a
// release version.") without a usable tag_name.
ReleaseInfo parse_release_payload(const FeedConfig&, const Json& payload,
                                  bool allow_prerelease = false);

// The beta track's offer from a release list: drafts and tags that are not plain
// [vV]?major.minor.patch skipped, the highest version wins, a stable release wins a tie.
// Throws AppUpdateError for a non-list ("The update server returned a response that could not be
// read.") or a list without a usable release.
ReleaseInfo select_newest_release(const FeedConfig&, const Json& payload);

// SHA256SUMS.txt: {name: lower-case digest}. Each line "<64 hex> <space or *><name>" with a
// name of [A-Za-z0-9_.-]; throws std::invalid_argument for anything else, a duplicate name,
// text that is not UTF-8, or more than kMaxChecksumBytes.
std::map<std::string, std::string> parse_release_checksums(std::string_view raw);

// Whether update_api_retry_at (the GitHub API cooldown) is still in the future.
bool api_retry_pending(const SettingsStore&, const Clock&);

// update_api_retry_at = the latest of now + 60 s, X-RateLimit-Reset (Unix time) and now +
// Retry-After (seconds); a header that cannot be read is skipped. Write errors are logged.
void remember_api_rate_limit(const FetchFailure&, SettingsStore&, const Clock&);

// The newest release of the track (from the feed override when there is one). While the GitHub
// API is rate limited (or cooling down) the stable tag comes from the latest-release redirect and
// only assets listed in that release's SHA256SUMS.txt are offered; without a usable checksum
// file the release can be notified, not installed. Throws AppUpdateError with the user's
// message. Blocking.
ReleaseInfo fetch_latest_release(http::Client&, const FeedConfig&, SettingsStore&, Clock&,
                                 const CancelToken&,
                                 std::optional<ReleaseTrack> track = std::nullopt);

} // namespace fv::update
