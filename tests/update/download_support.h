#pragma once

// Shared by the download, resilience and runtime tests: a GitHub-shaped setup asset, the routes
// a download takes, and an installed copy's probe.

#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "http/client.h"
#include "http/testing/fake_http_client.h"
#include "update/check.h"
#include "update/download.h"
#include "update/release_identity.h"
#include "update/update_contract.h"

namespace fv::update::test {

inline constexpr std::string_view kRepo = "example/app";
inline constexpr std::string_view kBody = "verified release installer";
inline constexpr std::int64_t kAssetId = 123456;
inline constexpr std::string_view kCdnUrl = "https://release-assets.githubusercontent.com/"
                                            "github-production-release-asset/1/2?signature=test";

inline std::string api_url() {
    return "https://api.github.com/repos/" + std::string(kRepo) + "/releases/assets/" +
           std::to_string(kAssetId);
}

inline FeedConfig github_feed() {
    FeedConfig feed;
    feed.source = UpdateSource{UpdateSource::Kind::GitHub, std::string(kRepo)};
    return feed;
}

inline std::string download_url(std::string_view version, std::string_view name) {
    return "https://github.com/" + std::string(kRepo) + "/releases/download/v" +
           std::string(version) + "/" + std::string(name);
}

// The setup asset of `version` holding `body`, with GitHub's asset id.
inline ReleaseAsset setup_asset(std::string_view version = "2.8.6", std::string_view body = kBody,
                                std::optional<std::int64_t> id = kAssetId) {
    ReleaseAsset asset;
    asset.name = setup_asset_name(version);
    asset.url = download_url(version, asset.name);
    asset.size_bytes = static_cast<std::int64_t>(body.size());
    asset.sha256 = sha256_hex(body);
    asset.github_asset_id = id;
    return asset;
}

inline ReleaseInfo release_with(ReleaseAsset setup, std::string version = "2.8.6") {
    ReleaseInfo release;
    release.version = version;
    release.tag_name = "v" + version;
    release.html_url = "https://github.com/" + std::string(kRepo) + "/releases/tag/v" + version;
    release.notes = "notes";
    release.setup_asset = std::move(setup);
    return release;
}

// A copy installed by the setup and running from its install folder.
inline InstallProbe installed_probe(std::filesystem::path dir = "C:/Users/u/Programs/App") {
    InstallProbe probe;
    probe.registered = true;
    probe.install_dir = dir;
    probe.running_dir = std::move(dir);
    return probe;
}

inline UpdateCheckResult available(ReleaseInfo release) {
    UpdateCheckResult result;
    result.status = UpdateStatus::UpdateAvailable;
    result.current_version = "0.1.0";
    result.channel = InstallChannel::Installer;
    result.release = std::move(release);
    result.can_apply = true;
    result.apply_mode = ApplyMode::Setup;
    return result;
}

inline http::ScriptedResponse ok(std::string_view body, http::Headers headers = {}) {
    http::ScriptedResponse response;
    response.body = std::string(body);
    response.headers = headers.empty()
                           ? http::Headers{{"Content-Length", std::to_string(body.size())}}
                           : std::move(headers);
    return response;
}

inline http::ScriptedResponse status(int code, std::string reason = "Error") {
    http::ScriptedResponse response;
    response.status = code;
    response.reason = std::move(reason);
    return response;
}

inline http::ScriptedResponse found(std::string location) {
    http::ScriptedResponse response;
    response.status = 302;
    response.reason = "Found";
    response.headers = {{"Location", std::move(location)}};
    return response;
}

inline std::string read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

inline void write_bytes(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

inline std::vector<std::string> folder_names(const std::filesystem::path& dir) {
    std::vector<std::string> out;
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        out.push_back(entry.path().filename().string());
    }
    return out;
}

// A request's Accept header, or "".
inline std::string accept_of(const http::Request& request) {
    return http::find_header(request.headers, "Accept").value_or("");
}

} // namespace fv::update::test
