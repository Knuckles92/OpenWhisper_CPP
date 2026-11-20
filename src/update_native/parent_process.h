#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>

#include "base/cancel.h"

namespace fv::update_native {
// Waits on one process handle after matching both image and raw Windows FILETIME creation ticks.
// A missing PID is already exited; inaccessible or mismatched live processes fail closed.
bool wait_for_parent(std::uint32_t pid, const std::filesystem::path& expected_exe,
                     std::uint64_t creation_ticks, std::chrono::milliseconds timeout,
                     const CancelToken& cancel = {});
// The creation time wait_for_parent compares for this process: raw FILETIME ticks on Windows,
// the start time /proc reports (clock ticks since boot) on Linux. Throws UpdateError when the
// system does not tell.
std::int64_t current_process_creation_time();
} // namespace fv::update_native
