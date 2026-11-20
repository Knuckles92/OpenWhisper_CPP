#pragma once

#include <filesystem>

namespace fv::agents::detail {

// os.path.realpath(folder) in its long form: links resolved, and on Windows every 8.3 short name
// replaced by its long one (Claude Code refuses a short path for --add-dir). A folder that
// doesn't exist resolves as far as it can.
std::filesystem::path long_real_path(const std::filesystem::path& folder);

} // namespace fv::agents::detail
