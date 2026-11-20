// Desktop session detection (OpenWhisper's services/desktop_session.py and the Settings dialog's
// is_native_wayland_session).

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <fstream>

#include "base/desktop_session.h"
#include "base/detail/pytext.h"
#include "base/identity.h"
#include "base/testing/map_env_reader.h"
#include "base/utf8.h"
#include "test_support.h"

using fv::MapEnvReader;

namespace {

void write_text(const std::filesystem::path& path, std::string_view text) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary) << text;
}

} // namespace

TEST_CASE("native wayland session detection", "[test_main_window_compact]") {
    // from tests/test_main_window_compact.py::test_native_wayland_session_detection
    CHECK(fv::is_native_wayland_session("linux", MapEnvReader{{"XDG_SESSION_TYPE", "wayland"}}));
    CHECK(fv::is_native_wayland_session("linux", MapEnvReader{{"WAYLAND_DISPLAY", "wayland-0"}}));
    CHECK_FALSE(fv::is_native_wayland_session("linux", MapEnvReader{{"XDG_SESSION_TYPE", "x11"}}));
    CHECK_FALSE(
        fv::is_native_wayland_session("win32", MapEnvReader{{"XDG_SESSION_TYPE", "wayland"}}));
}

TEST_CASE("the native check strips the session type and the plain one does not") {
    const MapEnvReader padded{{"XDG_SESSION_TYPE", " Wayland\n"}};
    CHECK(fv::is_native_wayland_session("linux", padded));
    CHECK_FALSE(fv::is_wayland_session("linux", padded));
    CHECK(fv::is_wayland_session("linux", MapEnvReader{{"XDG_SESSION_TYPE", "WAYLAND"}}));
    CHECK(fv::is_wayland_session("linux2", MapEnvReader{{"WAYLAND_DISPLAY", "wayland-1"}}));
    CHECK_FALSE(fv::is_wayland_session("linux", MapEnvReader{{"WAYLAND_DISPLAY", ""}}));
    CHECK_FALSE(fv::is_wayland_session("darwin", MapEnvReader{{"WAYLAND_DISPLAY", "wayland-0"}}));
}

TEST_CASE("omarchy theme paths follow the XDG state and config folders") {
    MapEnvReader env{
        {"HOME", "/home/ana"}, {"USERPROFILE", "/home/ana"}, {"XDG_STATE_HOME", "/state"}};
    const auto paths = fv::omarchy_theme_paths(env);
    CHECK(paths[0] ==
          std::filesystem::path("/state") / "omarchy" / "current" / "theme" / "colors.toml");
    CHECK(paths[1] == std::filesystem::path("/home/ana") / ".config" / "omarchy" / "current" /
                          "theme" / "colors.toml");
}

TEST_CASE("the UI variable forces Omarchy or classic on any platform") {
    const auto& app = fv::identity();
    CHECK(fv::use_omarchy_ui("win32", MapEnvReader{{"OPENWHISPER_CPP_UI", "Omarchy"}}, app));
    CHECK_FALSE(fv::use_omarchy_ui(
        "linux", MapEnvReader{{"OPENWHISPER_CPP_UI", "classic"}, {"OMARCHY_PATH", "/opt/omarchy"}},
        app));
    CHECK_FALSE(fv::use_omarchy_ui("win32", MapEnvReader{{"OMARCHY_PATH", "/opt/omarchy"}}, app));
    CHECK(fv::use_omarchy_ui("linux",
                             MapEnvReader{{"OPENWHISPER_CPP_UI", "auto"},
                                          {"HOME", "/nonexistent-home"},
                                          {"OMARCHY_PATH", "/opt/omarchy"}},
                             app));
}

TEST_CASE("Python's strip, splitlines, lower and upper as base needs them") {
    using fv::detail::py_splitlines;
    using fv::detail::py_strip;
    CHECK(py_strip(" \t\x1c x y \xC2\xA0\xE3\x80\x80\n") == "x y");
    CHECK(py_strip("\xC2\xA0") == "");
    CHECK(py_strip("\xC3\xA9") == "\xC3\xA9"); // a letter, not space
    CHECK(py_splitlines("a\r\nb\rc\nd\x0b"
                        "e\xE2\x80\xA8"
                        "f\n") == std::vector<std::string_view>{"a", "b", "c", "d", "e", "f"});
    CHECK(py_splitlines("").empty());
    CHECK(py_splitlines("\n\n") == std::vector<std::string_view>{"", ""});
    CHECK(fv::detail::ascii_lower("WayLand \xE2\x84\xAA") == "wayland k");
    CHECK(fv::detail::ascii_upper("debug \xC4\xB1 \xC5\xBF \xC3\xA9") == "DEBUG I S \xC3\xA9");
}

TEST_CASE("omarchy is detected by its install path, a theme file or os-release") {
    fv::test::TempDir temp;
    const std::array<std::filesystem::path, 2> themes{temp.path() / "state" / "colors.toml",
                                                      temp.path() / "config" / "colors.toml"};
    const auto os_release = temp.path() / "os-release";

    CHECK(fv::detect_omarchy("/opt/omarchy", themes, os_release));
    CHECK_FALSE(fv::detect_omarchy("", themes, os_release));

    write_text(os_release, "NAME=\"Arch Linux\"\nID=arch\n");
    CHECK_FALSE(fv::detect_omarchy("", themes, os_release));
    write_text(os_release, "NAME=Omarchy\r\n  ID=\"omarchy\"  \r\n");
    CHECK(fv::detect_omarchy("", themes, os_release));
    write_text(os_release, "\"ID=omarchy\"\n");
    CHECK(fv::detect_omarchy("", themes, os_release));
    std::filesystem::remove(os_release);

    write_text(themes[1], "accent = \"#7aa2f7\"\n");
    CHECK(fv::detect_omarchy("", themes, os_release));
}
