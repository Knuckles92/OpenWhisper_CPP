#pragma once

#include <array>
#include <filesystem>
#include <span>
#include <string_view>

#include "base/env.h"
#include "base/identity.h"

// Desktop capabilities read from the environment and the file system, without Qt or an X
// connection (OpenWhisper's services/desktop_session.py). Every answer is false off Linux.
namespace fv {

// services/desktop_session.py is_wayland_session: Linux and (XDG_SESSION_TYPE, lowercased, is
// "wayland", or WAYLAND_DISPLAY is not empty). `platform` is sys.platform ("linux", "win32").
bool is_wayland_session(std::string_view platform, const EnvReader&);
bool is_wayland_session();

// The Settings dialog's is_native_wayland_session: as is_wayland_session, but XDG_SESSION_TYPE is
// also stripped of white space first.
bool is_native_wayland_session(std::string_view platform, const EnvReader&);
bool is_native_wayland_session();

// Omarchy 4's state file, then Omarchy 3's config file:
// ${XDG_STATE_HOME:-~/.local/state}/omarchy/current/theme/colors.toml and
// ${XDG_CONFIG_HOME:-~/.config}/omarchy/current/theme/colors.toml.
std::array<std::filesystem::path, 2> omarchy_theme_paths(const EnvReader&);

// <env_prefix>UI = "omarchy" or "classic" (any case) forces the answer; otherwise Linux and
// detect_omarchy(OMARCHY_PATH, omarchy_theme_paths()), cached per pair of inputs as Python's
// lru_cache(maxsize=4) caches it, so the file system is probed once per session.
bool use_omarchy_ui(std::string_view platform, const EnvReader&, const AppIdentity&);
bool use_omarchy_ui();

// Uncached: OMARCHY_PATH is not empty, a theme file exists, or os-release has a line
// `ID=omarchy` (quotes allowed).
bool detect_omarchy(std::string_view install_path,
                    std::span<const std::filesystem::path> theme_paths,
                    const std::filesystem::path& os_release = "/etc/os-release");

} // namespace fv
