#pragma once

#include <cstdint>
#include <optional>

namespace fv::bench::detail {

// The process's peak resident set (Windows: peak working set) in bytes so far; nullopt where the
// platform does not say. benchmark_local_asr.py sampled the process tree every 50 ms with
// psutil; the runtime runs in this process, so the OS's own peak counts the same memory.
std::optional<std::uint64_t> peak_resident_bytes();

} // namespace fv::bench::detail
