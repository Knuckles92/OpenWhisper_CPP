#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/paths.h"
#include "download/verified_download.h"
#include "http/client.h"
#include "update/check.h"
#include "update/release_identity.h"

// Verified downloads of release assets (services/app_update.py: updates_dir ...
// download_installer, _file_sha256, _download_verified). Every request follows the update trust
// rules (update/trust.h); a GitHub download that fails with a gateway error is retried once
// through the release-asset API, under the same size and digest checks.
namespace fv::update {

// <local_app_dir>/updates: verified downloads and update transactions. Not moved by
// FIORIVOCE_DATA_DIR, as OpenWhisper's is not.
std::filesystem::path updates_root(const Paths&);

// What a download needs. `platform` is sys.platform's value ("win32" on Windows).
struct DownloadContext {
    http::Client& client;
    const FeedConfig& feed;
    const InstallProbe& install;
    std::filesystem::path updates_dir;
    std::string platform;
};
// The context on this build's platform.
DownloadContext download_context(http::Client&, const FeedConfig&, const InstallProbe&,
                                 std::filesystem::path updates_dir);

// "This copy of FioriVoce cannot install the Windows setup package."
std::string cannot_install_message();

// The asset's lower-case SHA-256 after checking it, its file name and its size: AppUpdateError
// "The update package is missing an integrity digest.", "... has an invalid filename." or "... has
// an invalid size." (more than kMaxAssetBytes counts as invalid).
std::string validated_asset_digest(const ReleaseAsset&);

// Downloads one authorized asset into `destination_dir` (which must exist) as <asset name> and
// verifies its size and SHA-256. A file already there is reused when its size and digest match
// (reported as verifying, done) and deleted otherwise ("The old update download could not be
// replaced." when it cannot be). Throws AppUpdateError with the user's message, "The download was
// canceled." when cancelled. Blocking: on the components strand.
std::filesystem::path download_release_asset(http::Client&, const FeedConfig&, const ReleaseAsset&,
                                             const std::filesystem::path& destination_dir,
                                             const download::ProgressCallback& progress = {},
                                             const CancelToken& cancel = {});

// The release's setup exe into the updates folder (created when missing). Refuses
// (cannot_install_message) unless the setup asset is ready, the copy is a registered install
// running from its install folder, and the platform is Windows. Blocking.
std::filesystem::path download_installer(const DownloadContext&, const ReleaseInfo&,
                                         const download::ProgressCallback& progress = {},
                                         const CancelToken& cancel = {});

// open_checked (update/trust.h) for a body streamed into `sink`: the start URL must be trusted,
// redirects (301, 302, 303, 307, 308) are followed here and each hop is checked against the URL
// the request started at, at most kMaxRedirects of them; only the final response's head and body
// reach the sink. A refused start, hop or final URL is a URLError ("The update URL is not
// trusted.", "The update server redirected to an unsafe URL.", "The update server returned an
// unsafe URL."). Headers (Range, Accept) are kept across hops. `repo` is the configured GitHub
// repository ("" for none). Blocking.
download::SendResult send_checked(http::Client&, const http::Request&, std::string_view repo,
                                  const CancelToken&, http::BodySink&);

// https://api.github.com/repos/<repo>/releases/assets/<id> for a positive id and a repository.
std::optional<std::string> asset_api_url(std::string_view repo, std::optional<std::int64_t> id);

} // namespace fv::update
