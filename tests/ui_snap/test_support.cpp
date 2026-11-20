// Where Snap can capture (tests/test_snap_session.py::TestSupport) and the golden table of
// ui_qt/snap/support.py over every platform value.
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "golden/golden.h"
#include "ui_snap/support.h"
#include "ui_snap/testing/fake_desktop.h"

using namespace fv::ui_snap;

namespace {

Platform desktop(std::string system, bool hyprland = false, bool grim = false, bool wayland = false,
                 bool granted = false) {
    return Platform{std::move(system), hyprland, grim, wayland, granted};
}

} // namespace

// from tests/test_snap_session.py::TestSupport::test_windows_is_supported
TEST_CASE("windows is supported", "[test_snap_session]") {
    CHECK(snapSupport(desktop("win32")) == std::pair{true, QString()});
}

// from tests/test_snap_session.py::TestSupport::test_x11_is_supported
TEST_CASE("x11 is supported", "[test_snap_session]") {
    CHECK(snapSupport(desktop("linux")) == std::pair{true, QString()});
}

// from tests/test_snap_session.py::TestSupport::test_hyprland_with_grim_is_supported
TEST_CASE("hyprland with grim is supported", "[test_snap_session]") {
    const Platform hyprland = desktop("linux", true, true, true);
    CHECK(snapSupport(hyprland) == std::pair{true, QString()});
    CHECK(usesGrim(hyprland));
}

// from tests/test_snap_session.py::TestSupport::test_hyprland_without_grim_says_to_install_it
TEST_CASE("hyprland without grim says to install it", "[test_snap_session]") {
    const auto [supported, reason] = snapSupport(desktop("linux", true, false, true));
    CHECK_FALSE(supported);
    CHECK(reason.contains(QStringLiteral("grim")));
}

// from tests/test_snap_session.py::TestSupport::test_other_wayland_desktops_are_not_supported_yet
TEST_CASE("other wayland desktops are not supported yet", "[test_snap_session]") {
    const Platform wayland = desktop("linux", false, false, true);
    const auto [supported, reason] = snapSupport(wayland);
    CHECK_FALSE(supported);
    CHECK(reason.contains(QStringLiteral("Wayland")));
    CHECK_FALSE(usesGrim(wayland));
}

// from tests/test_snap_session.py::TestSupport::test_other_systems_are_not_supported_yet
TEST_CASE("other systems are not supported yet", "[test_snap_session]") {
    const auto [supported, reason] = snapSupport(desktop("freebsd14"));
    CHECK_FALSE(supported);
    CHECK(reason.contains(QStringLiteral("system")));
}

// from tests/test_snap_session.py::TestSupport::test_macos_with_screen_recording_is_supported
TEST_CASE("macos with screen recording is supported", "[test_snap_session]") {
    CHECK(snapSupport(desktop("darwin", false, false, false, true)) == std::pair{true, QString()});
}

// from
// tests/test_snap_session.py::TestSupport::test_macos_without_screen_recording_says_where_to_grant_it
TEST_CASE("macos without screen recording says where to grant it", "[test_snap_session]") {
    const auto [supported, reason] = snapSupport(desktop("darwin"));
    CHECK_FALSE(supported);
    CHECK(reason.contains(QStringLiteral("Screen Recording")));
    CHECK(reason == macReason());
}

TEST_CASE("this desktop decides by the posed system", "[test_snap_session]") {
    fv::ui_snap::testing::ScopedFakeDesktop posed("win32");
    CHECK(currentPlatform().system == "win32");
    CHECK(snapSupport() == std::pair{true, QString()});
    CHECK_FALSE(usesGrim());
    requestAccess(); // macOS only, which is not ported: nothing to ask here.
}

TEST_CASE("a nested fake desktop puts the outer pose and fakes back", "[test_snap_session]") {
    const std::string real = systemPlatform();
    const QString realQt = qtPlatformName();
    {
        fv::ui_snap::testing::ScopedFakeDesktop outer("linux", QStringLiteral("xcb"));
        {
            fv::ui_snap::testing::ScopedFakeDesktop inner("darwin", QStringLiteral("cocoa"));
            CHECK(systemPlatform() == "darwin");
            CHECK(qtPlatformName() == QStringLiteral("cocoa"));
            CHECK(&win32Desktop() == &inner.win32);
        }
        CHECK(systemPlatform() == "linux");
        CHECK(qtPlatformName() == QStringLiteral("xcb"));
        CHECK(&win32Desktop() == &outer.win32);
        CHECK(&windowLister() == &outer.lister);
        CHECK(&screenGrabber() == &outer.grabber);
    }
    CHECK(systemPlatform() == real);
    CHECK(qtPlatformName() == realQt);
    CHECK(&win32Desktop() == &realWin32Desktop());
}

TEST_CASE("snap_support matches OpenWhisper on every platform", "[golden][test_snap_session]") {
    fv::golden::for_each("ui_qt.snap.support", "snap_support", [](const fv::golden::Case& c) {
        const Platform platform = desktop(c.arg<std::string>(0), c.arg<bool>(1), c.arg<bool>(2),
                                          c.arg<bool>(3), c.arg<bool>(4));
        const auto [supported, reason] = snapSupport(platform);
        const fv::Json expected = c.result<fv::Json>();
        CHECK(supported == expected[0].get<bool>());
        CHECK(fv::golden::equal_rebranded(expected[1].get<std::string>(), reason.toStdString()));
    });
}

TEST_CASE("uses_grim matches OpenWhisper on every platform", "[golden][test_snap_session]") {
    fv::golden::for_each("ui_qt.snap.support", "uses_grim", [](const fv::golden::Case& c) {
        const Platform platform = desktop(c.arg<std::string>(0), c.arg<bool>(1), c.arg<bool>(2),
                                          c.arg<bool>(3), c.arg<bool>(4));
        CHECK(usesGrim(platform) == c.result<bool>());
    });
}
