#include "update/leftovers.h"

#include <system_error>

#include "base/log.h"
#include "base/utf8.h"

namespace fv::update {

namespace {

constexpr log::Logger kLog("services.app_update");

std::chrono::system_clock::time_point to_system(std::filesystem::file_time_type time) {
#ifdef _WIN32
    // MSVC's file_clock has no to_sys; clock_cast goes through utc_clock.
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::clock_cast<std::chrono::system_clock>(time));
#else
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::file_clock::to_sys(time));
#endif
}

} // namespace

std::vector<std::string>
prune_stale_downloads(const std::filesystem::path& updates_dir,
                      std::chrono::system_clock::time_point now,
                      const std::function<void(const std::filesystem::path&)>& remove) {
    const auto cutoff = now - kDownloadKeepWindow;
    std::vector<std::string> removed;
    std::vector<std::filesystem::directory_entry> entries;
    try {
        std::filesystem::create_directories(updates_dir);
        for (const auto& entry : std::filesystem::directory_iterator(updates_dir)) {
            entries.push_back(entry);
        }
    } catch (const std::filesystem::filesystem_error& error) {
        kLog.debug("Could not read the updates directory: {}", error.what());
        return removed;
    }
    for (const auto& entry : entries) {
        const auto path = entry.path();
        try {
            if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(path))) {
                continue;
            }
            const auto modified = to_system(std::filesystem::last_write_time(path));
            if (modified > cutoff) {
                continue;
            }
            if (remove) {
                remove(path);
            } else {
                std::filesystem::remove(path);
            }
        } catch (const std::filesystem::filesystem_error& error) {
            // A Windows installer that is running stays locked; the next startup collects it.
            kLog.debug("Could not delete {}: {}", path_to_utf8(path), error.what());
            continue;
        }
        removed.push_back(path_to_utf8(path.filename()));
    }
    return removed;
}

} // namespace fv::update
