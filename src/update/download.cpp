#include "update/download.h"

#include <cctype>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <system_error>
#include <utility>
#include <variant>

#include "base/identity.h"
#include "base/log.h"
#include "base/platform_info.h"
#include "base/utf8.h"
#include "download/verified_files.h"
#include "pyport/errors.h"
#include "pyport/regex.h"
#include "pyport/text.h"
#include "update/detail/url.h"
#include "update/release_feed.h"
#include "update/trust.h"
#include "update/update_contract.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update");

constexpr int kMaxRepeats = 4;       // urllib's HTTPRedirectHandler.max_repeats
constexpr int kMaxRedirections = 10; // ...and max_redirections
constexpr std::string_view kInfiniteLoop =
    "The HTTP server returned a redirect error that would lead to an infinite loop.\n"
    "The last 30x error message was:\n";

using download::NetworkFailure;

bool is_redirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

download::SendResult url_failure(std::string reason) {
    return std::unexpected(NetworkFailure{NetworkFailure::Kind::Url, 0, std::move(reason)});
}

download::SendResult http_failure(int code, std::string reason) {
    return std::unexpected(
        NetworkFailure{NetworkFailure::Kind::HttpStatus, code, std::move(reason)});
}

// Passes the final response to the caller's sink and keeps a redirect, or a final URL the trust
// rules refuse, from reaching it.
class HopSink final : public http::BodySink {
public:
    HopSink(http::BodySink& inner, std::string_view repo, std::string_view initial,
            std::string_view url)
        : inner_(inner), repo_(repo), initial_(initial), url_(url) {}

    std::optional<http::Response> redirect;
    bool unsafe_final = false;
    std::exception_ptr failure;

    bool on_head(const http::Response& head) override {
        if (is_redirect(head.status) && (http::find_header(head.headers, "Location") ||
                                         http::find_header(head.headers, "URI"))) {
            redirect = head;
            return false;
        }
        if (head.status >= 200 && head.status < 300) {
            const auto final_url = head.effective_url.empty() ? url_ : head.effective_url;
            try {
                if (!redirect_url_allowed(repo_, initial_, final_url)) {
                    unsafe_final = true;
                    return false;
                }
            } catch (...) {
                failure = std::current_exception();
                return false;
            }
        }
        return inner_.on_head(head);
    }

    bool write(std::span<const char> data) override { return inner_.write(data); }

private:
    http::BodySink& inner_;
    std::string_view repo_;
    std::string_view initial_;
    std::string_view url_;
};

bool valid_asset_name(std::string_view name) {
    if (name.empty() || name.find('/') != std::string_view::npos) {
        return false;
    }
#ifdef _WIN32
    // ntpath.basename also splits at a backslash and after a drive ("C:name").
    if (name.find('\\') != std::string_view::npos) {
        return false;
    }
    if (name.size() >= 2 && name[1] == ':' &&
        std::isalpha(static_cast<unsigned char>(name[0])) != 0) {
        return false;
    }
#endif
    return true;
}

// _regular_file_stat: nullopt when nothing is there.
std::optional<std::uintmax_t> regular_file_size(const std::filesystem::path& path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found) {
        return std::nullopt;
    }
    if (error) {
        throw AppUpdateError("The downloaded update could not be inspected.");
    }
    if (status.type() != std::filesystem::file_type::regular) {
        throw AppUpdateError("The update download path is not a regular file.");
    }
    const auto links = std::filesystem::hard_link_count(path, error);
    if (error) {
        throw AppUpdateError("The downloaded update could not be inspected.");
    }
    if (links != 1) {
        throw AppUpdateError("The update download path is not a regular file.");
    }
    const auto size = std::filesystem::file_size(path, error);
    if (error) {
        throw AppUpdateError("The downloaded update could not be inspected.");
    }
    return size;
}

std::string file_sha256(const std::filesystem::path& path) {
    try {
        return download::sha256_file(path, true);
    } catch (const download::NotARegularFile&) {
        throw AppUpdateError("The update download path is not a regular file.");
    }
}

// _reuse_verified_download: true when `destination` already holds the asset; otherwise it is
// cleared.
bool reuse_verified_download(const std::filesystem::path& destination, const ReleaseAsset& asset,
                             std::string_view digest, const download::ProgressCallback& progress,
                             const CancelToken& cancel) {
    const auto size = regular_file_size(destination);
    if (!size) {
        return false;
    }
    const auto existing =
        *size == static_cast<std::uintmax_t>(asset.size_bytes) ? file_sha256(destination) : "";
    if (cancel.cancelled()) {
        throw AppUpdateError("The download was canceled.");
    }
    if (existing == digest) {
        if (progress) {
            progress(download::kVerifying, asset.size_bytes, asset.size_bytes);
        }
        return true;
    }
    std::error_code error;
    if (!std::filesystem::remove(destination, error) || error) {
        throw AppUpdateError("The old update download could not be replaced.");
    }
    return false;
}

// The shared downloader's wording, as the updater has always shown it.
download::Messages update_messages() {
    download::Messages text;
    text.invalid_size = "The update package has an invalid size.";
    text.unsafe_partial = "The update download could not be created safely.";
    text.partial_not_regular = "The update partial is not a regular file.";
    text.partial_unreadable = "The partial update could not be read.";
    text.bad_resume = "The update server returned an invalid resume response.";
    text.bad_length = "The update server returned an invalid download size.";
    text.unexpected_length = "The update server returned an unexpected download size.";
    text.too_much_data = "The update server returned more data than expected.";
    text.finalize = "The verified update could not be finalized. Please retry.";
    return text;
}

// The headers of the last refused answer, for the rate-limit test of describe_update_error.
struct RefusedHeads {
    std::mutex mutex;
    int status = 0;
    http::Headers headers;
};

class RecordingSink final : public http::BodySink {
public:
    RecordingSink(http::BodySink& inner, RefusedHeads& refused)
        : inner_(inner), refused_(refused) {}
    bool on_head(const http::Response& head) override {
        if (head.status < 200 || head.status >= 300) {
            std::lock_guard lock(refused_.mutex);
            refused_.status = head.status;
            refused_.headers = head.headers;
        }
        return inner_.on_head(head);
    }
    bool write(std::span<const char> data) override { return inner_.write(data); }

private:
    http::BodySink& inner_;
    RefusedHeads& refused_;
};

// _download_verified: bounded resume, exact size and SHA-256, and the asset-API route.
void download_verified_update(http::Client& client, const FeedConfig& feed, std::string_view url,
                              std::string_view digest, std::int64_t size_bytes,
                              const std::filesystem::path& destination,
                              const download::ProgressCallback& progress, const CancelToken& cancel,
                              std::optional<std::int64_t> asset_id) {
    const std::string repo(feed_repo(feed));
    const auto api_url = asset_api_url(repo, asset_id);
    auto refused = std::make_shared<RefusedHeads>();

    download::DownloadOptions options;
    options.max_bytes = kMaxAssetBytes;
    options.messages = update_messages();
    options.user_agent = feed.user_agent;
    options.describe_error = [refused, repo](const NetworkFailure& failure) {
        FetchFailure fetch{failure, {}};
        if (failure.kind == NetworkFailure::Kind::HttpStatus) {
            std::lock_guard lock(refused->mutex);
            if (refused->status == failure.code) {
                fetch.headers = refused->headers;
            }
        }
        return describe_update_error(fetch, repo);
    };
    options.send = [&client, refused, repo](const http::Request& request, const CancelToken& token,
                                            http::BodySink& sink) {
        RecordingSink recording(sink, *refused);
        return send_checked(client, request, repo, token, recording);
    };
    options.prepare = [api_url](http::Request& request) {
        if (api_url && request.url == *api_url) {
            request.headers.emplace_back("Accept", "application/octet-stream");
        }
    };
    options.switch_route = [api_url](int status) -> std::optional<std::string> {
        // Only a gateway failure of GitHub's download route has another way to the same bytes.
        if (!api_url || (status != 500 && status != 502 && status != 503 && status != 504)) {
            return std::nullopt;
        }
        kLog.info("GitHub release download returned HTTP {}; trying asset API", status);
        return api_url;
    };

    const download::ProgressCallback report =
        progress ? progress : [](std::string_view, std::int64_t, std::int64_t) {
        };
    try {
        download::download_verified(client, url, destination, size_bytes, digest, report, cancel,
                                    false, options);
    } catch (const download::DownloadError& error) {
        throw AppUpdateError(error.what());
    }
}

} // namespace

std::filesystem::path updates_root(const Paths& paths) {
    return paths.local_app_dir() / kUpdatesDirname;
}

DownloadContext download_context(http::Client& client, const FeedConfig& feed,
                                 const InstallProbe& install, std::filesystem::path updates_dir) {
    return DownloadContext{client, feed, install, std::move(updates_dir),
                           std::string(python_platform())};
}

std::string cannot_install_message() {
    return "This copy of " + std::string(identity().product) +
           " cannot install the Windows setup package.";
}

std::optional<std::string> asset_api_url(std::string_view repo, std::optional<std::int64_t> id) {
    if (repo.empty() || !id || *id <= 0) {
        return std::nullopt;
    }
    return "https://api.github.com/repos/" + std::string(repo) + "/releases/assets/" +
           std::to_string(*id);
}

std::string validated_asset_digest(const ReleaseAsset& asset) {
    const auto digest = py::lower(asset.sha256.value_or(""));
    const py::Regex hex("[0-9a-f]{64}");
    if (!hex.fullmatch(digest)) {
        throw AppUpdateError("The update package is missing an integrity digest.");
    }
    if (!valid_asset_name(asset.name)) {
        throw AppUpdateError("The update package has an invalid filename.");
    }
    if (asset.size_bytes <= 0 || asset.size_bytes > kMaxAssetBytes) {
        throw AppUpdateError("The update package has an invalid size.");
    }
    return digest;
}

std::filesystem::path download_release_asset(http::Client& client, const FeedConfig& feed,
                                             const ReleaseAsset& asset,
                                             const std::filesystem::path& destination_dir,
                                             const download::ProgressCallback& progress,
                                             const CancelToken& cancel) {
    const auto digest = validated_asset_digest(asset);
    if (cancel.cancelled()) {
        throw AppUpdateError("The download was canceled.");
    }
    const auto destination = destination_dir / path_from_utf8(asset.name);
    if (reuse_verified_download(destination, asset, digest, progress, cancel)) {
        return destination;
    }
    download_verified_update(client, feed, asset.url, digest, asset.size_bytes, destination,
                             progress, cancel, asset.github_asset_id);
    return destination;
}

std::filesystem::path download_installer(const DownloadContext& context, const ReleaseInfo& release,
                                         const download::ProgressCallback& progress,
                                         const CancelToken& cancel) {
    if (!asset_ready(release.setup_asset) ||
        detect_channel(context.install) != InstallChannel::Installer) {
        throw AppUpdateError(cannot_install_message());
    }
    if (context.platform != "win32") {
        throw AppUpdateError(cannot_install_message());
    }
    std::filesystem::create_directories(context.updates_dir);
    return download_release_asset(context.client, context.feed, *release.setup_asset,
                                  context.updates_dir, progress, cancel);
}

download::SendResult send_checked(http::Client& client, const http::Request& start,
                                  std::string_view repo, const CancelToken& cancel,
                                  http::BodySink& sink) {
    http::Request request = start;
    const std::string initial = request.url;
    try {
        if (!redirect_url_allowed(repo, initial, initial)) {
            return url_failure("The update URL is not trusted.");
        }
        int redirects = 0;
        std::map<std::string, int> visited;
        while (true) {
            request.follow_redirects = false;
            HopSink hop(sink, repo, initial, request.url);
            auto result = client.send(request, cancel, &hop);
            if (hop.failure) {
                std::rethrow_exception(hop.failure);
            }
            if (!result) {
                return std::unexpected(std::move(result.error()));
            }
            if (hop.unsafe_final) {
                return url_failure("The update server returned an unsafe URL.");
            }
            if (!hop.redirect) {
                result->effective_url = request.url; // response.geturl()
                return std::move(*result);
            }
            // HTTPRedirectHandler.http_error_302, then _SafeRedirectHandler.redirect_request.
            const auto& head = *hop.redirect;
            auto location = http::find_header(head.headers, "Location");
            if (!location) {
                location = http::find_header(head.headers, "URI");
            }
            const auto scheme = detail::urlsplit(*location).scheme;
            if (scheme != "http" && scheme != "https" && scheme != "ftp" && !scheme.empty()) {
                return http_failure(head.status, head.reason + " - Redirection to url '" +
                                                     *location + "' is not allowed");
            }
            const auto next =
                detail::urljoin(request.url, detail::normalize_redirect_location(*location));
            ++redirects;
            if (redirects > kMaxRedirects || !redirect_url_allowed(repo, initial, next)) {
                return url_failure("The update server redirected to an unsafe URL.");
            }
            if (request.method != "GET" && request.method != "HEAD") {
                return http_failure(head.status, head.reason);
            }
            const auto seen = visited.find(next);
            const int times = seen == visited.end() ? 0 : seen->second;
            if (times >= kMaxRepeats || static_cast<int>(visited.size()) >= kMaxRedirections) {
                return http_failure(head.status, std::string(kInfiniteLoop) + head.reason);
            }
            visited[next] = times + 1;
            std::erase_if(request.headers, [](const auto& header) {
                const auto name = py::lower(header.first);
                return name == "content-length" || name == "content-type";
            });
            request.body = std::monostate{};
            request.url = next;
        }
    } catch (const py::ValueError& error) {
        // Raised where urlsplit raises (a malformed bracketed host): str(exc) reaches the user.
        return std::unexpected(NetworkFailure{NetworkFailure::Kind::Other, 0, error.what()});
    }
}

} // namespace fv::update
