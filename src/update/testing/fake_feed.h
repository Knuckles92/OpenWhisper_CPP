#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/json.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "update/release_feed.h"
#include "update/update_contract.h"

// GitHub as an update check sees it, for tests: the release API (latest and list), the
// latest-release redirect, release pages, SHA256SUMS.txt and the asset HEADs of one repository,
// plus exact-URL routes for anything else (a loopback feed, a hostile redirect). Answers are
// GitHub-shaped JSON, real 302 redirects (the check follows them itself) and scripted errors
// (403 with rate-limit headers, 429, 500, a certificate failure). Unrouted URLs answer 404.
// Built on http::FakeHttpClient, so body sinks, chunking, cancel and size limits behave as there.
// Affinity: configure between requests; requests may come from any thread.
namespace fv::update::testing {

class FakeFeed final : public http::Client {
public:
    explicit FakeFeed(std::string repo = "example/app") : repo_(std::move(repo)) {
        inner_.set_handler([this](const http::Request& request) { return answer(request); });
    }
    // The handler holds `this`.
    FakeFeed(const FakeFeed&) = delete;
    FakeFeed& operator=(const FakeFeed&) = delete;

    // What `GET .../releases/latest` returns (unless api_error is set).
    Json latest = Json::object();
    // What `GET .../releases?per_page=30` returns (unless api_error is set).
    Json list = Json::array();
    // Returned for both API URLs instead of the JSON when set.
    std::optional<http::ScriptedResponse> api_error;
    // Where `.../releases/latest` (the page) redirects; empty: the tag page of latest's tag_name.
    std::string page;
    // The body of SHA256SUMS.txt; nullopt answers 404.
    std::optional<std::string> checksums;
    // Returned for SHA256SUMS.txt instead when set.
    std::optional<http::ScriptedResponse> checksums_error;
    // The Content-Length of an asset HEAD (nullopt: none sent) and its status.
    std::optional<std::string> asset_length = "4096";
    int asset_status = 200;
    // Exact URLs answered before anything above.
    std::map<std::string, http::ScriptedResponse> routes;

    const std::string& repo() const { return repo_; }
    GitHubUrls urls() const { return github_urls(repo_); }
    UpdateSource source() const { return UpdateSource{UpdateSource::Kind::GitHub, repo_}; }
    // A feed configuration of this repository (no override, the default User-Agent).
    FeedConfig config() const {
        FeedConfig config;
        config.source = source();
        return config;
    }
    std::string download_base(std::string_view tag) const {
        return "https://github.com/" + repo_ + "/releases/download/" + std::string(tag) + "/";
    }
    std::string tag_page(std::string_view tag) const {
        return urls().releases_page + "/tag/" + std::string(tag);
    }

    // A release object with one setup asset, as GitHub's API returns it.
    struct ReleaseSpec {
        std::string version;
        bool prerelease = false;
        bool draft = false;
        std::optional<std::string> digest = std::string(64, 'c'); // hex; nullopt sends null
        std::optional<std::string> url;                           // default: GitHub's URL
        std::int64_t size = 4096;
        std::int64_t id = 1000;
    };
    Json release(const ReleaseSpec& spec) const {
        const auto tag = "v" + spec.version;
        const auto name = setup_asset_name(spec.version);
        Json asset = Json::object();
        asset["id"] = spec.id;
        asset["name"] = name;
        asset["size"] = spec.size;
        asset["state"] = "uploaded";
        asset["digest"] = spec.digest ? Json("sha256:" + *spec.digest) : Json(nullptr);
        asset["browser_download_url"] = spec.url.value_or(download_base(tag) + name);
        Json release = Json::object();
        release["tag_name"] = tag;
        release["html_url"] = tag_page(tag);
        release["body"] = "Notes for " + spec.version;
        release["draft"] = spec.draft;
        release["prerelease"] = spec.prerelease;
        release["assets"] = Json::array({asset});
        return release;
    }

    static http::ScriptedResponse json(const Json& body) {
        http::ScriptedResponse response;
        response.headers = {{"Content-Type", "application/json"}};
        response.body = body.dump();
        return response;
    }
    static http::ScriptedResponse redirect(std::string location, int status = 302) {
        http::ScriptedResponse response;
        response.status = status;
        response.reason = status == 301 ? "Moved Permanently" : "Found";
        response.headers = {{"Location", std::move(location)}};
        return response;
    }
    static http::ScriptedResponse http_error(int status, std::string reason,
                                             http::Headers headers = {}) {
        http::ScriptedResponse response;
        response.status = status;
        response.reason = std::move(reason);
        response.headers = std::move(headers);
        return response;
    }
    // GitHub's primary rate limit: 403, no requests left, reset at `reset` (Unix seconds).
    static http::ScriptedResponse rate_limited(std::chrono::system_clock::time_point reset) {
        const auto seconds =
            std::chrono::duration_cast<std::chrono::seconds>(reset.time_since_epoch()).count();
        return http_error(
            403, "Forbidden",
            {{"X-RateLimit-Remaining", "0"}, {"X-RateLimit-Reset", std::to_string(seconds)}});
    }
    static http::ScriptedResponse failure(http::Error error) {
        http::ScriptedResponse response;
        response.error = std::move(error);
        return response;
    }
    static http::ScriptedResponse certificate_failure() {
        return failure(http::FakeHttpClient::certificate_failure());
    }
    static http::ScriptedResponse offline() {
        return failure(http::Error{http::ErrorKind::Connect, "offline"});
    }

    std::expected<http::Response, http::Error> send(const http::Request& request,
                                                    const CancelToken& cancel,
                                                    http::BodySink* stream = nullptr) override {
        return inner_.send(request, cancel, stream);
    }

    // Every request so far as (url, method), in order.
    std::vector<std::pair<std::string, std::string>> calls() const {
        std::vector<std::pair<std::string, std::string>> out;
        for (const auto& request : inner_.requests()) {
            out.emplace_back(request.url, request.method);
        }
        return out;
    }
    std::vector<http::Request> requests() const { return inner_.requests(); }
    std::vector<std::string> urls_requested() const {
        std::vector<std::string> out;
        for (const auto& request : inner_.requests()) {
            out.push_back(request.url);
        }
        return out;
    }

private:
    http::ScriptedResponse answer(const http::Request& request) {
        if (const auto found = routes.find(request.url); found != routes.end()) {
            return found->second;
        }
        const auto github = urls();
        if (request.url == github.latest_release || request.url == github.release_list) {
            if (api_error) {
                return *api_error;
            }
            return json(request.url == github.latest_release ? latest : list);
        }
        if (request.url == github.latest_release_page) {
            if (!page.empty()) {
                return redirect(page);
            }
            const auto tag = latest.is_object() && latest.contains("tag_name")
                                 ? latest["tag_name"].get<std::string>()
                                 : std::string();
            return redirect(tag_page(tag));
        }
        if (request.url.starts_with(github.releases_page + "/tag/")) {
            return {};
        }
        const auto downloads = "https://github.com/" + repo_ + "/releases/download/";
        if (request.url.starts_with(downloads)) {
            if (request.url.ends_with("/SHA256SUMS.txt")) {
                if (checksums_error) {
                    return *checksums_error;
                }
                if (!checksums) {
                    return http_error(404, "Not Found");
                }
                http::ScriptedResponse response;
                response.body = *checksums;
                return response;
            }
            http::ScriptedResponse response;
            response.status = asset_status;
            response.reason = asset_status == 200 ? "OK" : "Partial Content";
            if (asset_length) {
                response.headers = {{"Content-Length", *asset_length}};
            }
            return response;
        }
        return http_error(404, "Not Found");
    }

    std::string repo_;
    http::FakeHttpClient inner_;
};

} // namespace fv::update::testing
