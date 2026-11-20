#include "walkthrough/storage.h"

#include <format>
#include <system_error>

#include "base/utf8.h"

namespace fv::walkthrough {

namespace {

// root/<stem><suffix> for number 1, root/<stem>-<number><suffix> after it.
std::filesystem::path numbered(const std::filesystem::path& folder, const std::string& stem,
                               std::string_view suffix, int number) {
    const std::string name = number == 1 ? std::format("{}{}", stem, suffix)
                                         : std::format("{}-{}{}", stem, number, suffix);
    return folder / path_from_utf8(name);
}

} // namespace

std::filesystem::path captures_root(const Paths& paths) {
    return paths.data_root() / path_from_utf8(CAPTURES_FOLDER);
}

std::string stamp(std::chrono::local_seconds when) {
    const auto day = std::chrono::floor<std::chrono::days>(when);
    const std::chrono::year_month_day date{day};
    const std::chrono::hh_mm_ss time{when - day};
    return std::format("{:04}-{:02}-{:02}_{:02}-{:02}-{:02}", static_cast<int>(date.year()),
                       static_cast<unsigned>(date.month()), static_cast<unsigned>(date.day()),
                       time.hours().count(), time.minutes().count(), time.seconds().count());
}

std::filesystem::path new_capture_folder(const std::filesystem::path& root,
                                         std::chrono::local_seconds started_at) {
    std::filesystem::create_directories(root);
    const std::string stem = stamp(started_at);
    for (int number = 1;; ++number) {
        const auto folder = numbered(root, stem, "", number);
        std::error_code error;
        if (std::filesystem::create_directory(folder, error)) {
            return folder;
        }
        // Python's mkdir raises FileExistsError for a folder or a file of that name alike.
        if (!error || error == std::errc::file_exists) {
            continue;
        }
        if (std::filesystem::exists(std::filesystem::symlink_status(folder))) {
            continue;
        }
        throw std::filesystem::filesystem_error("Could not create the walkthrough folder", folder,
                                                error);
    }
}

std::filesystem::path new_screenshot_path(const std::filesystem::path& root,
                                          std::chrono::local_seconds taken_at) {
    const auto folder = root / path_from_utf8(SCREENSHOTS_FOLDER);
    std::filesystem::create_directories(folder);
    const std::string stem = stamp(taken_at);
    for (int number = 1;; ++number) {
        const auto path = numbered(folder, stem, ".png", number);
        std::error_code error;
        // Path.exists(): a dangling link reads as free.
        if (!std::filesystem::exists(path, error)) {
            return path;
        }
    }
}

} // namespace fv::walkthrough
