// from tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>

#include "meeting/finalization_view.h"
#include "meeting/platform_policy.h"

using namespace fv::meeting;

namespace {

PlatformInfo on(std::string platform, std::string machine = "", std::string macos_release = "") {
    return {std::move(platform), std::move(machine), std::move(macos_release)};
}

} // namespace

TEST_CASE("windows and modern macos are supported", "[test_meeting_unsupported_platform]") {
    // from
    // tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy::test_windows_and_modern_macos_are_supported
    CHECK(meeting_mode_supported(on("win32")));
    CHECK(meeting_mode_supported(on("win64")));
    CHECK(meeting_mode_supported(on("darwin", "arm64", "13.0")));
    // The Linux implementation is ready, but public promotion stays gated.
    CHECK_FALSE(meeting_mode_supported(on("linux", "x86_64")));
    CHECK_FALSE(meeting_mode_supported(on("linux2", "aarch64")));
    CHECK_FALSE(meeting_mode_supported(on("linux", "i686")));
    CHECK_FALSE(meeting_mode_supported(on("freebsd14")));
}

TEST_CASE("macos before screencapturekit audio is unsupported",
          "[test_meeting_unsupported_platform]") {
    // from
    // tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy::test_macos_before_screencapturekit_audio_is_unsupported
    CHECK_FALSE(meeting_mode_supported(on("darwin", "x86_64", "12.7.1")));
    CHECK(meeting_mode_supported(on("darwin", "arm64", "13.0")));
}

TEST_CASE("undetectable macos version fails closed", "[test_meeting_unsupported_platform]") {
    // from
    // tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy::test_undetectable_macos_version_fails_closed
    // Unknown macOS versions must not be promoted as supported.
    CHECK_FALSE(meeting_mode_supported(on("darwin", "", "")));
    CHECK_FALSE(meeting_mode_supported(on("darwin", "", "Ventura")));
}

TEST_CASE("linux implementation ready is separate from public support",
          "[test_meeting_unsupported_platform]") {
    // from
    // tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy::test_linux_implementation_ready_is_separate_from_public_support
    CHECK(linux_meeting_implementation_ready("x86_64"));
    CHECK(linux_meeting_implementation_ready("aarch64"));
    CHECK_FALSE(linux_meeting_implementation_ready("i686"));
    CHECK_FALSE(meeting_mode_supported(on("linux", "x86_64")));
    CHECK(meeting_setup_available(on("linux", "x86_64")));
    CHECK_FALSE(meeting_setup_available(on("linux", "i686")));
}

TEST_CASE("os display names", "[test_meeting_unsupported_platform]") {
    // from
    // tests/test_meeting_unsupported_platform.py::TestMeetingPlatformPolicy::test_os_display_names
    CHECK(meeting_unsupported_os_name("darwin") == "macOS");
    CHECK(meeting_unsupported_os_name("linux") == "Linux");
    CHECK(meeting_unsupported_os_name("linux2") == "Linux");
    CHECK(meeting_unsupported_os_name("freebsd14") == "this platform");
}

TEST_CASE("the running platform is a supported one", "[meeting][platform]") {
#if defined(_WIN32)
    CHECK(current_platform().platform == "win32");
    CHECK(meeting_mode_supported());
    CHECK(meeting_setup_available());
    CHECK_FALSE(system_audio_permission_required());
    CHECK(system_audio_permission_granted());
    CHECK_FALSE(meeting_audio_shows_platform_warning());
#elif defined(__linux__)
    CHECK(current_platform().platform == "linux");
    CHECK_FALSE(meeting_mode_supported());
#endif
    // An empty platform reads as the running one, as Python's `platform or sys.platform`.
    CHECK(meeting_mode_supported(on("")) == meeting_mode_supported());
}

TEST_CASE("macos grants are never assumed in this build", "[meeting][platform]") {
    CHECK(system_audio_permission_required("darwin"));
    CHECK_FALSE(system_audio_permission_granted(on("darwin", "arm64", "14.0")));
    CHECK_FALSE(request_system_audio_permission(on("darwin", "arm64", "14.0")));
    CHECK(system_audio_permission_granted(on("linux", "x86_64")));
}
