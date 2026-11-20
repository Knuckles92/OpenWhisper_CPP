#pragma once

#include <filesystem>
#include <string_view>

namespace fv {

struct AtomicWriteOptions {
    bool fsync = true; // flush the temporary file to disk before it replaces the target
};

// Replaces `target` with `bytes` so that a reader sees the old file or the new one, never part
// of either (OpenWhisper's settings save, services/settings.py:1079-1113): the bytes go to a new
// temporary ".<name>.<12 hex>.tmp" beside the target (mode 0600 outside Windows, as mkstemp
// makes it), are flushed, and the temporary replaces the target (MoveFileExW with
// REPLACE_EXISTING and WRITE_THROUGH on Windows, rename elsewhere). On any failure the temporary
// is removed, the target is left as it was, and std::filesystem::filesystem_error is thrown. The
// target's folder must exist.
void write_file_atomic(const std::filesystem::path& target, std::string_view bytes,
                       AtomicWriteOptions = {});

} // namespace fv
