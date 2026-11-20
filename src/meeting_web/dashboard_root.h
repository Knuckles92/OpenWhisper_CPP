#pragma once

#include <filesystem>

// Where the dashboard's built React bundle lives (OpenWhisper's config.bundle_root()/webui/dist):
// the root build copies webui/dist beside the executables, and the installer stages it the same
// way, so the folder of the running executable is the one place to look.
// Affinity: thread-safe (functions).
namespace fv::meeting_web {

// The folder of the running executable; empty when it cannot be read.
std::filesystem::path executable_dir();

// <executable_dir()>/webui/dist.
std::filesystem::path default_dist_root();

} // namespace fv::meeting_web
