#include "meeting/platform_policy.h"

#include "base/log.h"
#include "pyport/number.h"
#include "pyport/text.h"

namespace fv::meeting {

namespace {

const log::Logger logger("meeting.platform");

// The names platform.machine() gives for the architecture this build targets: Windows reports
// PROCESSOR_ARCHITECTURE's spelling, Linux uname's.
constexpr std::string_view build_machine() {
#if defined(_WIN32)
#if defined(_M_ARM64)
    return "ARM64";
#elif defined(_M_X64)
    return "AMD64";
#else
    return "x86";
#endif
#else
#if defined(__aarch64__)
    return "aarch64";
#elif defined(__x86_64__)
    return "x86_64";
#else
    return "";
#endif
#endif
}

constexpr std::string_view build_platform() {
#if defined(_WIN32)
    return "win32";
#elif defined(__APPLE__)
    return "darwin";
#elif defined(__linux__)
    return "linux";
#else
    return "";
#endif
}

std::string_view platform_or_current(std::string_view platform) {
    return platform.empty() ? std::string_view(current_platform().platform) : platform;
}

} // namespace

const PlatformInfo& current_platform() {
    static const PlatformInfo info{std::string(build_platform()), std::string(build_machine()),
                                   std::string()};
    return info;
}

std::optional<std::string> normalize_linux_machine(std::string_view machine) {
    const std::string raw = py::lower(py::strip(machine));
    if (raw == "x86_64" || raw == "amd64" || raw == "x64") {
        return "linux_x86_64";
    }
    if (raw == "aarch64" || raw == "arm64") {
        return "linux_aarch64";
    }
    return std::nullopt;
}

int macos_major(std::string_view release) {
    if (release.empty()) {
        return 0;
    }
    const std::string major = py::split_on(release, ".").front();
    const auto value = py::py_int(major);
    return value ? static_cast<int>(*value) : 0;
}

bool linux_meeting_implementation_ready(std::string_view machine) {
    return normalize_linux_machine(machine).has_value();
}

bool meeting_mode_supported(const PlatformInfo& info) {
    const std::string_view platform = platform_or_current(info.platform);
    if (platform.starts_with("win")) {
        return true;
    }
    if (platform == "darwin") {
        // Unknown macOS versions fail closed: ScreenCaptureKit audio needs a known Ventura+
        // baseline rather than an optimistic guess.
        return macos_major(info.macos_release) >= kMinMacosMajor;
    }
    // The Linux implementation exists, but public support stays gated until the manual
    // x86_64/aarch64 hardware release matrix is attested.
    return false;
}

bool meeting_setup_available(const PlatformInfo& info) {
    return meeting_mode_supported(info) ||
           (platform_or_current(info.platform).starts_with("linux") &&
            linux_meeting_implementation_ready(info.machine));
}

std::string meeting_unsupported_os_name(std::string_view platform) {
    platform = platform_or_current(platform);
    if (platform == "darwin") {
        return "macOS";
    }
    if (platform.starts_with("linux")) {
        return "Linux";
    }
    return "this platform";
}

bool system_audio_permission_required(std::string_view platform) {
    return platform_or_current(platform) == "darwin";
}

bool system_audio_permission_granted(const PlatformInfo& info) {
    if (!system_audio_permission_required(info.platform)) {
        return true;
    }
    logger.warning("Could not preflight the Screen Recording grant: not supported in this build");
    return false;
}

bool request_system_audio_permission(const PlatformInfo& info) {
    if (!system_audio_permission_required(info.platform)) {
        return true;
    }
    logger.warning("Could not request the Screen Recording grant: not supported in this build");
    return false;
}

} // namespace fv::meeting
