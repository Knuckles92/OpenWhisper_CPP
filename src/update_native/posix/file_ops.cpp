#include "update_native/detail/file_ops.h"

#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

namespace fv::update_native::detail {
namespace fs = std::filesystem;

bool unsafe_node(const fs::path& path) {
    return fs::is_symlink(fs::symlink_status(path));
}

void flush_file(const fs::path& path) {
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        throw fs::filesystem_error("Opening update file for flush", path,
                                   {errno, std::generic_category()});
    }
    const int result = fsync(fd);
    const int saved = errno;
    close(fd);
    if (result < 0) {
        throw fs::filesystem_error("Flushing update file", path, {saved, std::generic_category()});
    }
}

void flush_directory(const fs::path& path) {
    flush_file(path);
}

void replace_file(const fs::path& source, const fs::path& destination) {
    fs::rename(source, destination);
    flush_directory(destination.parent_path());
}

void publish_new(const fs::path& source, const fs::path& destination) {
#if defined(__linux__) && defined(SYS_renameat2)
    if (syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(),
                RENAME_NOREPLACE) == 0) {
        flush_directory(destination.parent_path());
        return;
    }
#elif defined(__APPLE__)
    if (renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL) == 0) {
        flush_directory(destination.parent_path());
        return;
    }
#else
    throw fs::filesystem_error("Exclusive rename unavailable", source, destination,
                               std::make_error_code(std::errc::operation_not_supported));
#endif
    throw fs::filesystem_error("Publishing a verified update", source, destination,
                               {errno, std::generic_category()});
}
} // namespace fv::update_native::detail
