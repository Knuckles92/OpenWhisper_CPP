#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

namespace fv::models::detail {

// A loaded library kept alive by `handle`; `symbol` is null when the library lacks the name.
struct NativeLibrary {
    std::shared_ptr<void> handle;
    void* (*symbol)(void* handle, const char* name) = nullptr;
};

// The NVML candidates of this platform, in the order to try them.
std::vector<std::filesystem::path> nvml_candidate_paths();

// The library at `path` (a bare name is searched for as the system loader searches), or nullopt
// when it does not load.
std::optional<NativeLibrary> open_native_library(const std::filesystem::path& path);

} // namespace fv::models::detail
