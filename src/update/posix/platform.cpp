#include "update/detail/platform.h"

#include <sys/stat.h>

#include <system_error>

namespace fv::update::detail {

std::optional<UninstallValues> read_uninstall_key(RegistryHive, std::string_view, int) {
    // No registry: nothing is ever installed through the Windows setup here.
    return std::nullopt;
}

std::vector<int> registry_views() {
    return {0};
}

std::filesystem::path executable_path() {
    std::error_code error;
    auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::filesystem::path() : path;
}

std::optional<std::string> file_identity(const std::filesystem::path& path) {
    struct stat info {};
    if (::stat(path.c_str(), &info) != 0) {
        return std::nullopt;
    }
    const long long mtime_ns =
        static_cast<long long>(info.st_mtim.tv_sec) * 1'000'000'000LL + info.st_mtim.tv_nsec;
    return std::to_string(static_cast<unsigned long long>(info.st_ino)) + ":" +
           std::to_string(static_cast<long long>(info.st_size)) + ":" + std::to_string(mtime_ns);
}

} // namespace fv::update::detail
