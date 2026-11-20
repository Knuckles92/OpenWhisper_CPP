#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

#include "base/cancel.h"
#include "download/verified_download.h"
#include "http/client.h"
#include "update/release_identity.h"

// Network trust rules of update requests (services/app_update.py: _parsed_url_is_well_formed
// ... _describe_update_error). Every request starts at a trusted endpoint, follows redirects by
// hand, at most kMaxRedirects of them, and re-validates every hop and the final URL against the
// endpoint it started from.
namespace fv::update {

inline constexpr int kMaxRedirects = 5;
inline constexpr std::array<std::string_view, 1> kGithubReleaseAssetHosts{
    "release-assets.githubusercontent.com"};
// Every host an update reaches, named to IT when a certificate fails to verify.
inline constexpr std::array<std::string_view, 3> kUpdateHosts{
    "api.github.com", "github.com", "release-assets.githubusercontent.com"};

inline constexpr std::string_view kRateLimitStatus =
    "Could not check for updates (GitHub rate limit).";

// The exact local soak-feed endpoint: http(s), a loopback IP literal, an explicit port, no user
// info, path exactly "/releases/latest", no query or fragment.
bool is_loopback_feed_url(std::string_view url);

// _parsed_url_is_well_formed(urlsplit(url)): a scheme and a host, no user info, no fragment,
// and port 80, 443 or none unless the URL's origin is a loopback feed's. Throws py::ValueError
// where urlsplit does.
bool url_well_formed(std::string_view url);

// https://api.github.com/repos/<repo>/releases/assets/<positive id>, exactly.
bool is_github_asset_api_url(std::string_view repo, std::string_view url);

// Whether a request that started at `initial_url` may go to (or end at) `candidate_url`. From a
// loopback feed: the same origin only. Otherwise https on 443 only: from api.github.com to
// api.github.com, or from an asset API URL to the release-asset CDN; from github.com to
// github.com or the CDN; nothing else. Throws py::ValueError where urlsplit does.
bool redirect_url_allowed(std::string_view repo, std::string_view initial_url,
                          std::string_view candidate_url);

// (scheme, netloc), both lower case.
std::pair<std::string, std::string> url_origin(std::string_view url);

// What a failed update request raised: the network failure (an HTTP error carries its status,
// reason and headers) as describe_network_error reads it.
struct FetchFailure {
    download::NetworkFailure failure;
    http::Headers headers; // of an HTTP error response
};

// 429, or 403 with X-RateLimit-Remaining "0" or any Retry-After.
bool is_rate_limited(const FetchFailure&);

// A request as services.verified_download.open_url builds it: `method`, the User-Agent (the
// feed's, or http::user_agent()), 30 s connect and idle timeouts, redirects not followed.
http::Request update_request(const FeedConfig&, std::string_view url,
                             std::string_view method = "GET");

// _open: refuses an untrusted start ("The update URL is not trusted."), follows redirects
// itself (301, 302, 303, 307, 308 as urllib does, HEAD staying HEAD, headers kept), refuses an
// unsafe hop ("The update server redirected to an unsafe URL.") or final URL ("The update server
// returned an unsafe URL."), and turns a final status outside 2xx into an HTTP error. The
// response's effective_url is the URL it finally came from.
// `repo` is the configured GitHub repository ("" for none). Blocking.
std::expected<http::Response, FetchFailure> open_checked(http::Client&, http::Request,
                                                         std::string_view repo, const CancelToken&);

// The user's message for a failed update request: the rate-limit text for a rate limit, else
// describe_network_error naming the update server and kUpdateHosts.
std::string describe_update_error(const FetchFailure&, std::string_view repo);
// "GitHub rate-limited this update check. Try again later, or open <releases page>."
std::string rate_limit_message(std::string_view repo);

// The status-bar line of a failed automatic check: kRateLimitStatus when `error` speaks of a
// rate limit, else "Could not check for updates."
std::string update_check_failure_status(std::string_view error);

} // namespace fv::update
