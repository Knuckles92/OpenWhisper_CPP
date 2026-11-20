#include "update/update_copy.h"

#include <format>

#include "base/identity.h"
#include "base/platform_info.h"

namespace fv::update {

namespace {

// services.format_utils.format_size_bytes: decimal units; std::format's fixed precision rounds
// the exact binary value, as Python's format() does.
std::string format_size_bytes(std::int64_t size_bytes) {
    if (size_bytes >= 1'000'000'000) {
        return std::format("{:.2f} GB", static_cast<double>(size_bytes) / 1'000'000'000);
    }
    if (size_bytes >= 1'000'000) {
        return std::format("{:.0f} MB", static_cast<double>(size_bytes) / 1'000'000);
    }
    if (size_bytes >= 1'000) {
        return std::format("{:.0f} KB", static_cast<double>(size_bytes) / 1'000);
    }
    return std::format("{} B", size_bytes);
}

bool offers_update(const UpdateCheckResult* result) {
    return result != nullptr && result->status == UpdateStatus::UpdateAvailable;
}

} // namespace

std::string title_text(const UpdateCheckResult* result) {
    if (result == nullptr) {
        return "Could not check for updates";
    }
    const std::string product(identity().product);
    if (result->status == UpdateStatus::UpdateAvailable) {
        const auto& release = result->release;
        const std::string latest = release ? release->version : "a new version";
        const std::string beta = release && release->prerelease ? " beta" : "";
        return product + " " + latest + beta + " is available";
    }
    if (result->status == UpdateStatus::Development) {
        return "This is a development build";
    }
    return product + " is up to date";
}

std::string body_text(const UpdateCheckResult* result, std::string_view error,
                      std::string_view platform) {
    if (result == nullptr) {
        return error.empty() ? std::string("The update server could not be reached.")
                             : std::string(error);
    }
    std::string text = "You have " + result->current_version + " (" +
                       channel_label(result->channel, platform) + ").";
    const auto& release = result->release;
    if (result->status == UpdateStatus::UpdateAvailable && release) {
        if (const auto size = download_size_bytes(result); size != 0) {
            text += "  Download size: " + format_size_bytes(size) + ".";
        }
    } else if (result->status == UpdateStatus::Development && release) {
        text += "  This copy is newer than the latest release (" + release->version + ").";
    } else if (release) {
        text += "  The latest release is " + release->version + ".";
    }
    return text;
}

std::string body_text(const UpdateCheckResult* result, std::string_view error) {
    if (result != nullptr && !result->platform.empty()) {
        return body_text(result, error, result->platform);
    }
    return body_text(result, error, python_platform());
}

std::int64_t download_size_bytes(const UpdateCheckResult* result) {
    if (result == nullptr || !result->release) {
        return 0;
    }
    const auto& release = *result->release;
    const ReleaseAsset* asset = nullptr;
    if (result->apply_mode == ApplyMode::Native) {
        asset = release.native_asset ? &*release.native_asset : nullptr;
    } else if (result->apply_mode == ApplyMode::Setup) {
        asset = release.setup_asset ? &*release.setup_asset : nullptr;
    }
    // A notify-only copy downloads nothing; showing the setup size would name the wrong download.
    return asset != nullptr ? asset->size_bytes : 0;
}

std::string hint_text(const UpdateCheckResult* result) {
    (void)result;
    return {};
}

std::string primary_label(const UpdateCheckResult* result) {
    if (result == nullptr) {
        return "Open releases page";
    }
    if (result->status != UpdateStatus::UpdateAvailable) {
        return {};
    }
    return result->can_apply ? "Download and install" : "Open release notes";
}

std::string later_label(const UpdateCheckResult* result) {
    return offers_update(result) ? "Later" : "Close";
}

std::string progress_detail_text(const UpdateCheckResult* result, DownloadPhase phase,
                                 std::int64_t done, std::int64_t total) {
    // Only the download counts bytes; the other phases count archive members, which would read as
    // a nonsense size, so they name the release being installed instead.
    if (phase == DownloadPhase::Downloading && total > 0) {
        return format_size_bytes(done) + " of " + format_size_bytes(total);
    }
    if (result != nullptr && result->release) {
        return std::string(identity().product) + " " + result->release->version;
    }
    return {};
}

std::optional<std::string> phase_text(DownloadPhase phase) {
    switch (phase) {
    case DownloadPhase::Downloading:
        return "Downloading the update…";
    case DownloadPhase::Verifying:
        return "Verifying the download…";
    case DownloadPhase::Extracting:
        return "Preparing the update…";
    case DownloadPhase::Restarting:
        return "Restarting to finish the update…";
    case DownloadPhase::RollingBack:
        return "Restoring the previous version…";
    case DownloadPhase::Opening:
        break;
    }
    return std::nullopt;
}

} // namespace fv::update
