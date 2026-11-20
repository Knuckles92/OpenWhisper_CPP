#pragma once

#include <filesystem>
#include <string_view>

// The file write of history_export.write_per_entry_files: open(path, "x", encoding="utf-8").
namespace fv::history_detail {

// Creates `path` exclusively and writes `bytes` to it: false, writing nothing, when the path
// exists (FileExistsError). The file gets the permissions Python's open() gives a new file
// (0666 less the umask on POSIX), not the private 0600 of a recording. Throws
// std::filesystem::filesystem_error for any other failure; a file created before a failed
// write stays, as in Python.
bool create_export_file(const std::filesystem::path& path, std::string_view bytes);

} // namespace fv::history_detail
