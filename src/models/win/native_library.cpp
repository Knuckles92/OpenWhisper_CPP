#include <windows.h>

#include <string>

#include "base/env.h"
#include "base/utf8.h"
#include "engine/shared_library.h"
#include "models/detail/native_library.h"

namespace fv::models::detail {

std::vector<std::filesystem::path> nvml_candidate_paths() {
    std::vector<std::filesystem::path> paths;
    // gpu_info.py loads "nvml.dll" by name, which the driver installs into System32 (since 2019).
    // SharedLibrary loads by absolute path only, so System32 is named outright.
    wchar_t system[MAX_PATH] = {};
    const UINT length = GetSystemDirectoryW(system, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        paths.push_back(std::filesystem::path(std::wstring(system, length)) / L"nvml.dll");
    }
    const std::string program_files =
        system_env().get("ProgramFiles").value_or(std::string("C:\\Program Files"));
    paths.push_back(fv::base_utf8::path_from_utf8(program_files) / "NVIDIA Corporation" / "NVSMI" /
                    "nvml.dll");
    return paths;
}

std::optional<NativeLibrary> open_native_library(const std::filesystem::path& path) {
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error)) {
        return std::nullopt;
    }
    try {
        auto library = std::make_shared<SharedLibrary>(path);
        NativeLibrary loaded;
        loaded.handle = std::shared_ptr<void>(library, library.get());
        loaded.symbol = [](void* handle, const char* name) -> void* {
            return static_cast<SharedLibrary*>(handle)->symbol(name);
        };
        return loaded;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

} // namespace fv::models::detail
