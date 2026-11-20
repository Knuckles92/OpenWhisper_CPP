#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "base/cancel.h"
#include "base/clock.h"
#include "update/release_identity.h"
#include "update/update_contract.h"

namespace fv {
class SettingsStore;
namespace http {
class Client;
}
} // namespace fv

// Update checks (services/app_update.py): the result types, install channel, version rules,
// apply mode, and check_for_update itself. The release fetch is update/release_feed.h, the
// network trust rules update/trust.h, the schedule update/schedule.h.
namespace fv::update {

// How the running copy was installed. A development build (run from a build tree) is Source; a
// Python git checkout has no counterpart.
enum class InstallChannel { Installer, Source };
std::string_view channel_name(InstallChannel); // "installer", "source"

// Which releases this copy is offered (not how it was installed).
enum class ReleaseTrack { Stable, Beta };
std::string_view track_name(ReleaseTrack); // "stable", "beta"

// The running version against the newest release of the track.
enum class UpdateStatus { UpdateAvailable, UpToDate, Development };
std::string_view status_name(UpdateStatus); // "update_available", "up_to_date", "development"

// Phases a download or prepare reports, plus the two commit phases.
enum class DownloadPhase { Downloading, Verifying, Extracting, Opening, Restarting, RollingBack };
std::string_view phase_name(DownloadPhase); // "downloading", ... "rolling_back"

// A failure the user reads: the message is a full sentence.
class AppUpdateError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A recognized update asset of a release.
struct ReleaseAsset {
    std::string url;
    std::string name;
    std::int64_t size_bytes = 0;
    std::optional<std::string> sha256;           // lower-case hex; none: notify only
    std::optional<std::int64_t> github_asset_id; // only for GitHub's exact download URL
    friend bool operator==(const ReleaseAsset&, const ReleaseAsset&) = default;
};

// A parsed release (`/releases/latest` or one entry of the release list).
struct ReleaseInfo {
    std::string version; // normalized tag
    std::string tag_name;
    std::string html_url;
    std::string notes;
    std::optional<ReleaseAsset> setup_asset;
    std::optional<ReleaseAsset> native_asset;
    bool prerelease = false;

    // The preferred download for a size: the native archive, else the setup.
    const ReleaseAsset* asset() const;
    friend bool operator==(const ReleaseInfo&, const ReleaseInfo&) = default;
};

struct UpdateCheckResult {
    UpdateStatus status = UpdateStatus::UpToDate;
    std::string current_version;
    InstallChannel channel = InstallChannel::Source;
    std::optional<ReleaseInfo> release;
    bool can_apply = false;
    ApplyMode apply_mode = ApplyMode::NotifyOnly;
    ReleaseTrack release_track = ReleaseTrack::Stable;
    // sys.platform the result was decided for: check_for_update sets CheckContext::platform
    // (this build's when that is empty), so the dialog's copy names the channel of the platform
    // the apply mode was chosen for. A result built by hand describes the product's, Windows.
    std::string platform = "win32";
};

// What the install registration says about the running copy, filled by the registry probe.
struct InstallProbe {
    bool registered = false; // an Inno uninstall entry with our AppId exists
    bool all_users = false;  // ...under HKLM
    std::filesystem::path install_dir, running_dir;
};

// Installer when the running executable lives in the folder a registered install names
// (compared lexically, without regard to case on Windows); otherwise Source, such as a build
// tree on a computer that also has OpenWhisper_CPP installed.
InstallChannel detect_channel(const InstallProbe&);

// "Windows installer", "Linux package" or "macOS application" by `platform` (sys.platform),
// else "Source copy".
std::string channel_label(InstallChannel, std::string_view platform);
std::string channel_label(InstallChannel); // this build's platform

// Lenient (major, minor, patch): strip, drop a leading v, cut at the first "-", "+" or space,
// skip empty segments, a segment int() cannot read is 0, pad to three.
std::array<long long, 3> parse_version(std::string_view raw);
UpdateStatus compare_versions(std::string_view current, std::string_view latest);

// An asset with a URL, a digest and a positive size.
bool asset_ready(const ReleaseAsset*);
bool asset_ready(const std::optional<ReleaseAsset>&);

// Notify only off the installer channel and off Windows; Setup when the setup asset is ready;
// otherwise notify only. These two never select Native: that takes the install's eligibility.
ApplyMode resolve_release_apply_mode(InstallChannel, const std::optional<ReleaseInfo>&,
                                     std::string_view platform);
ApplyMode resolve_release_apply_mode(InstallChannel, const std::optional<ReleaseInfo>&);
// The whole rule (resolve_release_apply_mode with resolve_apply_mode's native branch): Native
// when the release has a ready archive and this copy may replace itself, which takes a per-user
// (HKCU) registration naming the running folder, Windows, and `native_install`, the answer of
// update_native's eligibility check (not elevated, the registered program, its inventory
// present). Anything less falls back to the two-argument rule: an all-users (HKLM) install, an
// elevated process or a copy without a registration updates through the setup or only notifies.
ApplyMode resolve_release_apply_mode(const InstallProbe&, const std::optional<ReleaseInfo>&,
                                     bool native_install, std::string_view platform);
bool can_apply(InstallChannel, const std::optional<ReleaseInfo>&, std::string_view platform);
bool can_apply(InstallChannel, const std::optional<ReleaseInfo>&);

// Everything one check needs. The version is this build's ("0.1.0"), compared as semver.
struct CheckContext {
    http::Client& client;
    const FeedConfig& feed;
    SettingsStore& settings;
    const InstallProbe& install;
    Clock& clock;
    CancelToken cancel;
    std::string current_version;
    bool native_install = false;
    // sys.platform the apply mode is decided for ("win32"); empty: this build's
    // (python_platform()). Tests of the Windows flow on another system set "win32".
    std::string platform;
};

// Compares the running version with the newest release of this copy's track. Leaving the beta
// track never downgrades: a prerelease newer than the latest stable reports Development.
// persist: a success writes update_last_check_at, an AppUpdateError writes
// update_last_failed_check_at (rate limits too) and is rethrown; write failures are logged.
// Without an update source or a loopback feed, throws AppUpdateError("This build of OpenWhisper_CPP
// has no update source.") before any request and without writing anything. Blocking: never on
// the GUI thread.
UpdateCheckResult check_for_update(const CheckContext&, bool persist = true);
// The same with this build's version (the project version CMake was configured with).
UpdateCheckResult check_for_update(http::Client&, const FeedConfig&, SettingsStore&,
                                   const InstallProbe&, Clock&, const CancelToken&,
                                   bool persist = true);

// update_last_check_at = now, as datetime.isoformat() of an aware UTC time. Errors are logged.
void mark_check_completed(SettingsStore&, Clock&);

// The update dialog's opt-outs, in one write: update_notify_enabled then update_check_enabled.
// Write errors propagate (the dialog logs them).
void persist_prompt_choices(SettingsStore&, bool notify_enabled, bool check_enabled);

} // namespace fv::update
