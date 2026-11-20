#pragma once

#include <optional>
#include <string>
#include <string_view>

// Which platforms Meeting Mode is promoted on (meeting/platform.py). Meeting Mode captures a
// microphone and system audio; system audio has a first-class path on Windows (WASAPI loopback)
// and on macOS 13+ (ScreenCaptureKit, behind the Screen Recording grant). Linux x86_64/aarch64
// has an implementation whose public promotion stays gated, so setup is available there while
// support is not. Everything is pure over a PlatformInfo; only current_platform() reads the
// running system.
namespace fv::meeting {

// ScreenCaptureKit gained system-audio capture in macOS 13 (Ventura).
inline constexpr int kMinMacosMajor = 13;

// The pane holding the Screen Recording grant ("Screen & System Audio Recording" since macOS 14).
inline constexpr std::string_view kScreenRecordingSettingsUrl =
    "x-apple.systempreferences:com.apple.preference.security?Privacy_ScreenCapture";

struct PlatformInfo {
    // sys.platform: "win32", "darwin", "linux". Empty reads as the running platform, as Python's
    // `platform or sys.platform`.
    std::string platform;
    // platform.machine(): "AMD64", "x86_64", "arm64", ...
    std::string machine;
    // platform.mac_ver()[0]: "13.0"; empty off macOS or when unknown.
    std::string macos_release;
};

// The running system: the platform and machine this build targets, and no macOS release (no
// macOS build exists, so a darwin answer always fails closed).
const PlatformInfo& current_platform();

// "linux_x86_64" or "linux_aarch64" for a Linux machine name in any case, nullopt otherwise.
std::optional<std::string> normalize_linux_machine(std::string_view machine);

// The major macOS version of a release string, 0 when it cannot be read.
int macos_major(std::string_view release);

// The Linux implementation covers this architecture (distinct from public support).
bool linux_meeting_implementation_ready(std::string_view machine);

// Meeting Mode is publicly promoted here: Windows always, macOS 13+ (an unknown version fails
// closed), Linux never until its hardware matrix is attested.
bool meeting_mode_supported(const PlatformInfo& info = current_platform());

// Setup is offered: a supported platform, or a Linux host with the implementation.
bool meeting_setup_available(const PlatformInfo& info = current_platform());

// The OS name in unsupported-platform copy: "macOS", "Linux" or "this platform".
std::string meeting_unsupported_os_name(std::string_view platform = current_platform().platform);

// This OS gates system-audio capture behind a user grant (macOS only).
bool system_audio_permission_required(std::string_view platform = current_platform().platform);

// Whether system audio can be captured without prompting: true off macOS, where no grant
// applies. The macOS preflight and request (Quartz) are not built, so on macOS both are false.
bool system_audio_permission_granted(const PlatformInfo& info = current_platform());
bool request_system_audio_permission(const PlatformInfo& info = current_platform());

} // namespace fv::meeting
