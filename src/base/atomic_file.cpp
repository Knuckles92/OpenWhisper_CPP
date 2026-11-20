#include "base/atomic_file.h"

#include <string>
#include <system_error>

#include "base/detail/atomic_replace.h"
#include "base/ids.h"
#include "base/utf8.h"

namespace fv {

namespace {

// tempfile.TMP_MAX: mkstemp gives up after this many names that already exist.
constexpr int kNameAttempts = 10000;

} // namespace

void write_file_atomic(const std::filesystem::path& target, std::string_view bytes,
                       AtomicWriteOptions options) {
    const auto absolute = std::filesystem::absolute(target);
    const auto directory = absolute.parent_path();
    const std::string prefix = "." + path_to_utf8(absolute.filename()) + ".";
    std::filesystem::path temp;
    for (int attempt = 0;; ++attempt) {
        if (attempt == kNameAttempts) {
            throw std::filesystem::filesystem_error("No unused temporary file name", directory,
                                                    std::make_error_code(std::errc::file_exists));
        }
        temp = directory / path_from_utf8(prefix + random_hex(12) + ".tmp");
        try {
            if (detail::create_temp_file(temp, bytes, options.fsync)) {
                break;
            }
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temp, ignored);
            throw;
        }
    }
    try {
        detail::replace_file(temp, absolute);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temp, ignored);
        throw;
    }
}

} // namespace fv
