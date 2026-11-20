#include "update/check.h"

#include "base/platform_info.h"
#include "base/utf8.h"
#include "http/version.h"
#include "pyport/number.h"
#include "pyport/text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "update/release_feed.h"
#include "update/schedule.h"

namespace fv::update {

namespace {

// A folder as written, without "." or ".." steps or a trailing separator.
std::filesystem::path folder_key(const std::filesystem::path& folder) {
    auto normal = folder.lexically_normal();
    if (!normal.has_filename() && normal.has_relative_path()) {
        normal = normal.parent_path();
    }
    return normal;
}

bool same_folder(const std::filesystem::path& a, const std::filesystem::path& b) {
    if (a.empty() || b.empty()) {
        return false;
    }
#ifdef _WIN32
    // NTFS folder names do not differ by case, outside ASCII either.
    return py::lower(path_to_utf8(folder_key(a))) == py::lower(path_to_utf8(folder_key(b)));
#else
    return folder_key(a) == folder_key(b);
#endif
}

} // namespace

std::string_view channel_name(InstallChannel channel) {
    return channel == InstallChannel::Installer ? "installer" : "source";
}

std::string_view track_name(ReleaseTrack track) {
    return track == ReleaseTrack::Beta ? "beta" : "stable";
}

std::string_view status_name(UpdateStatus status) {
    switch (status) {
    case UpdateStatus::UpdateAvailable:
        return "update_available";
    case UpdateStatus::Development:
        return "development";
    case UpdateStatus::UpToDate:
        break;
    }
    return "up_to_date";
}

std::string_view phase_name(DownloadPhase phase) {
    switch (phase) {
    case DownloadPhase::Downloading:
        return "downloading";
    case DownloadPhase::Verifying:
        return "verifying";
    case DownloadPhase::Extracting:
        return "extracting";
    case DownloadPhase::Opening:
        return "opening";
    case DownloadPhase::Restarting:
        return "restarting";
    case DownloadPhase::RollingBack:
        break;
    }
    return "rolling_back";
}

const ReleaseAsset* ReleaseInfo::asset() const {
    if (native_asset) {
        return &*native_asset;
    }
    return setup_asset ? &*setup_asset : nullptr;
}

InstallChannel detect_channel(const InstallProbe& probe) {
    if (probe.registered && same_folder(probe.install_dir, probe.running_dir)) {
        return InstallChannel::Installer;
    }
    return InstallChannel::Source;
}

std::string channel_label(InstallChannel channel, std::string_view platform) {
    if (channel == InstallChannel::Installer) {
        if (platform.starts_with("linux")) {
            return "Linux package";
        }
        if (platform == "darwin") {
            return "macOS application";
        }
        return "Windows installer";
    }
    return "Source copy";
}

std::string channel_label(InstallChannel channel) {
    return channel_label(channel, python_platform());
}

std::array<long long, 3> parse_version(std::string_view raw) {
    auto text = normalize_version(raw);
    for (const char separator : {'-', '+', ' '}) {
        if (const auto at = text.find(separator); at != std::string::npos) {
            text.resize(at);
        }
    }
    std::array<long long, 3> numbers{};
    std::size_t count = 0;
    for (const auto& part : py::split_on(text, ".")) {
        if (part.empty()) {
            continue;
        }
        if (count == numbers.size()) {
            break;
        }
        numbers[count++] = py::py_int(part).value_or(0);
    }
    return numbers;
}

UpdateStatus compare_versions(std::string_view current, std::string_view latest) {
    const auto current_tuple = parse_version(current);
    const auto latest_tuple = parse_version(latest);
    if (current_tuple < latest_tuple) {
        return UpdateStatus::UpdateAvailable;
    }
    if (current_tuple > latest_tuple) {
        return UpdateStatus::Development;
    }
    return UpdateStatus::UpToDate;
}

bool asset_ready(const ReleaseAsset* asset) {
    return asset != nullptr && !asset->url.empty() && asset->sha256 && !asset->sha256->empty() &&
           asset->size_bytes > 0;
}

bool asset_ready(const std::optional<ReleaseAsset>& asset) {
    return asset_ready(asset ? &*asset : nullptr);
}

ApplyMode resolve_release_apply_mode(InstallChannel channel,
                                     const std::optional<ReleaseInfo>& release,
                                     std::string_view platform) {
    if (channel != InstallChannel::Installer) {
        return ApplyMode::NotifyOnly;
    }
    // The system package manager owns a Linux install, and the Mac update path is not ported:
    // both may notify, never install.
    if (platform != "win32") {
        return ApplyMode::NotifyOnly;
    }
    if (release && asset_ready(release->setup_asset)) {
        return ApplyMode::Setup;
    }
    return ApplyMode::NotifyOnly;
}

ApplyMode resolve_release_apply_mode(InstallChannel channel,
                                     const std::optional<ReleaseInfo>& release) {
    return resolve_release_apply_mode(channel, release, python_platform());
}

ApplyMode resolve_release_apply_mode(const InstallProbe& install,
                                     const std::optional<ReleaseInfo>& release, bool native_install,
                                     std::string_view platform) {
    const auto channel = detect_channel(install);
    if (native_install && channel == InstallChannel::Installer && !install.all_users &&
        platform == "win32" && release && asset_ready(release->native_asset)) {
        return ApplyMode::Native;
    }
    return resolve_release_apply_mode(channel, release, platform);
}

bool can_apply(InstallChannel channel, const std::optional<ReleaseInfo>& release,
               std::string_view platform) {
    const auto mode = resolve_release_apply_mode(channel, release, platform);
    return mode == ApplyMode::Native || mode == ApplyMode::Setup;
}

bool can_apply(InstallChannel channel, const std::optional<ReleaseInfo>& release) {
    return can_apply(channel, release, python_platform());
}

UpdateCheckResult check_for_update(const CheckContext& context, bool persist) {
    if (!has_update_source(context.feed)) {
        throw AppUpdateError(no_update_source_message());
    }
    const auto channel = detect_channel(context.install);
    const auto track = release_track(context.settings);
    std::optional<ReleaseInfo> release;
    try {
        release = fetch_latest_release(context.client, context.feed, context.settings,
                                       context.clock, context.cancel, track);
    } catch (const AppUpdateError&) {
        if (persist) {
            save_check_timestamp(context.settings, keys::update_last_failed_check_at.key(),
                                 context.clock.utc());
        }
        throw;
    }
    UpdateCheckResult result;
    result.status = compare_versions(context.current_version, release->version);
    if (persist) {
        mark_check_completed(context.settings, context.clock);
    }
    const std::string_view platform =
        context.platform.empty() ? python_platform() : std::string_view(context.platform);
    result.apply_mode =
        resolve_release_apply_mode(context.install, release, context.native_install, platform);
    result.current_version = context.current_version;
    result.channel = channel;
    result.release = std::move(release);
    result.can_apply =
        result.apply_mode == ApplyMode::Native || result.apply_mode == ApplyMode::Setup;
    result.release_track = track;
    result.platform = platform;
    return result;
}

UpdateCheckResult check_for_update(http::Client& client, const FeedConfig& feed,
                                   SettingsStore& settings, const InstallProbe& install,
                                   Clock& clock, const CancelToken& cancel, bool persist) {
    const CheckContext context{
        client, feed, settings, install, clock, cancel, std::string(http::kAppVersion)};
    return check_for_update(context, persist);
}

void mark_check_completed(SettingsStore& settings, Clock& clock) {
    save_check_timestamp(settings, keys::update_last_check_at.key(), clock.utc());
}

void persist_prompt_choices(SettingsStore& settings, bool notify_enabled, bool check_enabled) {
    Json patch = Json::object();
    patch[std::string(keys::update_notify_enabled.key())] = notify_enabled;
    patch[std::string(keys::update_check_enabled.key())] = check_enabled;
    settings.update(patch);
}

} // namespace fv::update
