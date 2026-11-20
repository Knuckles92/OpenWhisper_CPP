#include "backup/detail/json_file.h"

#include <string>

#include "backup/backup.h"
#include "backup/detail/fs.h"
#include "base/atomic_file.h"
#include "pyport/errors.h"
#include "pyport/json.h"

namespace fv::backup::detail {

void write_json_atomic(const std::filesystem::path& path, const Json& value) {
    std::filesystem::create_directories(path.parent_path());
    py::DumpOptions compact;
    compact.ensure_ascii = false;
    compact.item_sep = ",";
    compact.key_sep = ":";
    // Compact JSON holds no line break of its own (one inside a string is escaped), so the text
    // mode of open(path, "w") changes only the final newline.
#ifdef _WIN32
    const std::string text = py::dumps(value, compact) + "\r\n";
#else
    const std::string text = py::dumps(value, compact) + "\n";
#endif
    write_file_atomic(path, text);
    fsync_directory(path.parent_path());
}

Json read_json(const std::filesystem::path& path, std::uint64_t max_bytes) {
    if (!is_file(path) || is_link(path)) {
        throw BackupError("Restore metadata is missing or invalid");
    }
    std::uint64_t size = 0;
    try {
        size = stat(path).size;
    } catch (const std::filesystem::filesystem_error&) {
        throw BackupError("Restore metadata is missing or invalid");
    }
    if (size > max_bytes) {
        throw BackupError("Restore metadata is missing or invalid");
    }
    Json data;
    try {
        const std::string bytes = read_file(path);
        if (!is_valid_utf8(bytes)) {
            throw BackupError("Restore metadata is invalid");
        }
        data = py::loads(bytes);
    } catch (const std::filesystem::filesystem_error&) {
        throw BackupError("Restore metadata is invalid");
    } catch (const py::ValueError&) {
        throw BackupError("Restore metadata is invalid");
    }
    if (!data.is_object()) {
        throw BackupError("Restore metadata must be an object");
    }
    return data;
}

} // namespace fv::backup::detail
