#include "update/trust.h"

#include <algorithm>
#include <map>

#include "http/user_agent.h"
#include "pyport/errors.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "update/detail/url.h"

namespace fv::update {

namespace {

using detail::UrlParts;
using detail::urlsplit;
using detail::urlunsplit;

constexpr std::string_view kInfiniteLoop =
    "The HTTP server returned a redirect error that would lead to an infinite loop.\n"
    "The last 30x error message was:\n";
constexpr int kMaxRepeats = 4;       // urllib's HTTPRedirectHandler.max_repeats
constexpr int kMaxRedirections = 10; // ...and max_redirections

// _is_loopback_feed_url over urlunsplit((scheme, netloc, "/releases/latest", "", "")): whether
// the URL's origin is a loopback feed's.
bool origin_is_loopback_feed(const UrlParts& parts) {
    UrlParts feed;
    feed.scheme = parts.scheme;
    feed.netloc = parts.netloc;
    feed.path = "/releases/latest";
    return is_loopback_feed_url(urlunsplit(feed));
}

bool is_release_asset_host(std::string_view host) {
    return std::ranges::find(kGithubReleaseAssetHosts, host) != kGithubReleaseAssetHosts.end();
}

FetchFailure url_error(std::string reason) {
    return FetchFailure{
        download::NetworkFailure{download::NetworkFailure::Kind::Url, 0, std::move(reason)}, {}};
}

FetchFailure http_error(int code, std::string reason, http::Headers headers) {
    return FetchFailure{download::NetworkFailure{download::NetworkFailure::Kind::HttpStatus, code,
                                                 std::move(reason)},
                        std::move(headers)};
}

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

} // namespace

bool is_loopback_feed_url(std::string_view url) {
    UrlParts parts;
    std::optional<int> port;
    try {
        parts = urlsplit(url);
        const auto loopback = detail::ip_is_loopback(parts.hostname().value_or(""));
        if (!loopback || !*loopback) {
            return false;
        }
        port = parts.port();
    } catch (const py::ValueError&) {
        return false;
    }
    return (parts.scheme == "http" || parts.scheme == "https") && port.has_value() &&
           !parts.username() && !parts.password() && parts.path == "/releases/latest" &&
           parts.query.empty() && parts.fragment.empty();
}

bool url_well_formed(std::string_view url) {
    const auto parts = urlsplit(url);
    std::optional<int> port;
    try {
        port = parts.port();
    } catch (const py::ValueError&) {
        return false;
    }
    const bool common = !parts.scheme.empty() && parts.hostname() && !parts.username() &&
                        !parts.password() && parts.fragment.empty();
    const bool loopback = origin_is_loopback_feed(parts);
    return common && (!port || *port == 80 || *port == 443 || loopback);
}

bool is_github_asset_api_url(std::string_view repo, std::string_view url) {
    if (repo.empty()) {
        return false;
    }
    const py::Regex pattern(py::regex_escape("https://api.github.com/repos/" + std::string(repo) +
                                             "/releases/assets/") +
                            "[1-9][0-9]*");
    return pattern.fullmatch(url).has_value();
}

bool redirect_url_allowed(std::string_view repo, std::string_view initial_url,
                          std::string_view candidate_url) {
    const auto initial = urlsplit(initial_url);
    const auto candidate = urlsplit(candidate_url);
    if (!url_well_formed(candidate_url)) {
        return false;
    }
    const auto initial_host = initial.hostname().value_or("");
    const auto host = candidate.hostname().value_or("");
    if (origin_is_loopback_feed(initial)) {
        return url_origin(initial_url) == url_origin(candidate_url);
    }
    if (initial.scheme != "https" || candidate.scheme != "https") {
        return false;
    }
    if (const auto port = candidate.port(); port && *port != 443) {
        return false;
    }
    const bool cdn_asset = is_release_asset_host(host) &&
                           candidate.path.starts_with("/github-production-release-asset/");
    if (initial_host == "api.github.com") {
        return host == "api.github.com" ||
               (is_github_asset_api_url(repo, initial_url) && cdn_asset);
    }
    if (initial_host == "github.com") {
        return host == "github.com" || cdn_asset;
    }
    return false;
}

std::pair<std::string, std::string> url_origin(std::string_view url) {
    const auto parts = urlsplit(url);
    return {py::lower(parts.scheme), py::lower(parts.netloc)};
}

bool is_rate_limited(const FetchFailure& failure) {
    if (failure.failure.kind != download::NetworkFailure::Kind::HttpStatus) {
        return false;
    }
    const int code = failure.failure.code;
    return code == 429 ||
           (code == 403 && (http::find_header(failure.headers, "X-RateLimit-Remaining") == "0" ||
                            http::find_header(failure.headers, "Retry-After").has_value()));
}

http::Request update_request(const FeedConfig& feed, std::string_view url,
                             std::string_view method) {
    http::Request request;
    request.method = std::string(method);
    request.url = std::string(url);
    request.headers.emplace_back("User-Agent",
                                 feed.user_agent.empty() ? http::user_agent() : feed.user_agent);
    request.connect_timeout = download::kNetworkTimeout;
    request.idle_timeout = download::kNetworkTimeout;
    request.follow_redirects = false;
    return request;
}

std::expected<http::Response, FetchFailure> open_checked(http::Client& client,
                                                         http::Request request,
                                                         std::string_view repo,
                                                         const CancelToken& cancel) {
    const std::string initial = request.url;
    try {
        if (!redirect_url_allowed(repo, initial, initial)) {
            return std::unexpected(url_error("The update URL is not trusted."));
        }
        int redirects = 0;
        std::map<std::string, int> visited;
        while (true) {
            request.follow_redirects = false;
            auto result = client.send(request, cancel);
            if (!result) {
                return std::unexpected(FetchFailure{download::network_failure(result.error()), {}});
            }
            auto& response = *result;
            if (is_redirect(response.status)) {
                auto location = http::find_header(response.headers, "Location");
                if (!location) {
                    location = http::find_header(response.headers, "URI");
                }
                if (location) {
                    // HTTPRedirectHandler.http_error_302, then our redirect_request.
                    const auto scheme = urlsplit(*location).scheme;
                    if (scheme != "http" && scheme != "https" && scheme != "ftp" &&
                        !scheme.empty()) {
                        return std::unexpected(http_error(response.status,
                                                          response.reason +
                                                              " - Redirection to url '" +
                                                              *location + "' is not allowed",
                                                          response.headers));
                    }
                    const auto next = detail::urljoin(
                        request.url, detail::normalize_redirect_location(*location));
                    ++redirects;
                    if (redirects > kMaxRedirects || !redirect_url_allowed(repo, initial, next)) {
                        return std::unexpected(
                            url_error("The update server redirected to an unsafe URL."));
                    }
                    if (request.method != "GET" && request.method != "HEAD") {
                        return std::unexpected(
                            http_error(response.status, response.reason, response.headers));
                    }
                    const auto seen = visited.find(next);
                    const int times = seen == visited.end() ? 0 : seen->second;
                    if (times >= kMaxRepeats ||
                        static_cast<int>(visited.size()) >= kMaxRedirections) {
                        return std::unexpected(http_error(
                            response.status, std::string(kInfiniteLoop) + response.reason,
                            response.headers));
                    }
                    visited[next] = times + 1;
                    std::erase_if(request.headers, [](const auto& header) {
                        const auto name = py::lower(header.first);
                        return name == "content-length" || name == "content-type";
                    });
                    request.body = std::monostate{};
                    request.url = next;
                    continue;
                }
            }
            if (response.status < 200 || response.status >= 300) {
                return std::unexpected(
                    http_error(response.status, response.reason, response.headers));
            }
            const auto final_url =
                response.effective_url.empty() ? request.url : response.effective_url;
            if (!redirect_url_allowed(repo, initial, final_url)) {
                return std::unexpected(url_error("The update server returned an unsafe URL."));
            }
            response.effective_url = final_url; // response.geturl()
            return std::move(response);
        }
    } catch (const py::ValueError& error) {
        // Raised where urlsplit raises (a malformed bracketed host): str(exc) reaches the user.
        return std::unexpected(FetchFailure{
            download::NetworkFailure{download::NetworkFailure::Kind::Other, 0, error.what()}, {}});
    }
}

std::string rate_limit_message(std::string_view repo) {
    return "GitHub rate-limited this update check. Try again later, or open https://github.com/" +
           std::string(repo) + "/releases.";
}

std::string describe_update_error(const FetchFailure& failure, std::string_view repo) {
    // Without a GitHub source the server is the loopback feed, which has no rate limit to name.
    if (!repo.empty() && is_rate_limited(failure)) {
        return rate_limit_message(repo);
    }
    download::NetworkErrorText text;
    text.server = "update server";
    text.hosts.assign(kUpdateHosts.begin(), kUpdateHosts.end());
    return download::describe_network_error(failure.failure, text);
}

std::string update_check_failure_status(std::string_view error) {
    const auto text = py::lower(error);
    if (text.find("rate-limited") != std::string::npos ||
        text.find("rate limit") != std::string::npos) {
        return std::string(kRateLimitStatus);
    }
    return "Could not check for updates.";
}

} // namespace fv::update
