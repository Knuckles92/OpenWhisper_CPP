#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

// What an earlier update left in the updates folder (services/app_update.py:
// prune_stale_downloads). The setup path exits with the installer it downloaded still on disk, and
// Inno writes setup.log beside it; the next start collects both.
namespace fv::update {

// Files modified within this window are spared: a download may be writing them.
inline constexpr std::chrono::seconds kDownloadKeepWindow{30};

// Deletes the regular files directly in `updates_dir` (created when missing) last modified at
// least kDownloadKeepWindow before `now`, and returns their names in directory order.
// Directories (the native updater's transactions) and links are never touched. A file that cannot
// be deleted (a setup still running keeps its exe locked) is skipped and logged; the next start
// collects it. An unreadable folder yields nothing. `remove` stands in for the deletion in tests
// (it throws std::filesystem::filesystem_error to refuse). Blocking: on the components strand.
std::vector<std::string>
prune_stale_downloads(const std::filesystem::path& updates_dir,
                      std::chrono::system_clock::time_point now,
                      const std::function<void(const std::filesystem::path&)>& remove = {});

} // namespace fv::update
