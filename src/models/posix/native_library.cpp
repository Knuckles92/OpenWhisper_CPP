#include <dlfcn.h>

#include "models/detail/native_library.h"

namespace fv::models::detail {

std::vector<std::filesystem::path> nvml_candidate_paths() {
#if defined(__linux__)
    return {"libnvidia-ml.so.1", "libnvidia-ml.so"};
#else
    return {};
#endif
}

std::optional<NativeLibrary> open_native_library(const std::filesystem::path& path) {
    // A bare soname is searched for by the dynamic loader, as ctypes.CDLL does.
    void* handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return std::nullopt;
    }
    NativeLibrary loaded;
    loaded.handle = std::shared_ptr<void>(handle, [](void* opened) { dlclose(opened); });
    loaded.symbol = [](void* opened, const char* name) -> void* {
        return dlsym(opened, name);
    };
    return loaded;
}

} // namespace fv::models::detail
