#include "history/detail/export_file.h"

#include <span>

#include "history/detail/exclusive_file.h"

namespace fv::history_detail {

// On Windows a CRT mode only decides the read-only attribute, which 0600 and Python's 0666 both
// leave clear, so the recordings' exclusive file serves.
bool create_export_file(const std::filesystem::path& path, std::string_view bytes) {
    const auto file = ExclusiveFile::create(path);
    if (!file) {
        return false;
    }
    file->write(std::as_bytes(std::span(bytes.data(), bytes.size())));
    file->close();
    return true;
}

} // namespace fv::history_detail
