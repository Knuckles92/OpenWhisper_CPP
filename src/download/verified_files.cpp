#include "download/verified_files.h"

#include <fstream>
#include <vector>

#include "base/hash.h"
#include "base/utf8.h"
#include "download/detail/format_size.h"
#include "download/detail/platform.h"
#include "download/detail/raw_file.h"

namespace fv::download {

namespace {

// Python's repr() of a path string, enough for the file names in error texts.
std::string quoted(const std::filesystem::path& path) {
    const std::string text = path_to_utf8(path);
    std::string out = "'";
    for (const char c : text) {
        if (c == '\\' || c == '\'') {
            out += '\\';
        }
        out += c;
    }
    out += '\'';
    return out;
}

} // namespace

std::string sha256_file(const std::filesystem::path& path, bool regular_only) {
    Sha256 digest;
    std::vector<char> buffer(kChunkBytes);
    if (!regular_only) {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            throw std::filesystem::filesystem_error(
                "open", path, std::make_error_code(std::errc::no_such_file_or_directory));
        }
        while (in) {
            in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
            const auto got = static_cast<std::size_t>(in.gcount());
            if (got == 0) {
                break;
            }
            digest.update(std::string_view(buffer.data(), got));
        }
        if (in.bad()) {
            throw std::filesystem::filesystem_error("read", path,
                                                    std::make_error_code(std::errc::io_error));
        }
        return digest.hexdigest();
    }
    detail::RawFile file;
    try {
        file = detail::RawFile::open_read_nofollow(path);
    } catch (const std::system_error&) {
        // A final link that O_NOFOLLOW (or the reparse-point check) refused is the case this
        // flag exists for; a missing or unreadable file stays an OS error.
        std::error_code ignored;
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(path, ignored))) {
            throw NotARegularFile(path_to_utf8(path));
        }
        throw;
    }
    const auto info = file.info();
    if (!info.regular || info.links != 1) {
        throw NotARegularFile(path_to_utf8(path));
    }
    while (true) {
        const auto got = file.read(buffer);
        if (got == 0) {
            break;
        }
        digest.update(std::string_view(buffer.data(), got));
    }
    return digest.hexdigest();
}

std::uint64_t disk_free_bytes(const std::filesystem::path& path) {
    const auto space = std::filesystem::space(path);
    // shutil.disk_usage: GetDiskFreeSpaceExW's total free bytes on Windows, f_bavail elsewhere.
    return detail::kWindows ? space.free : space.available;
}

std::optional<std::pair<std::int64_t, std::int64_t>>
free_space_shortfall(const std::filesystem::path& path, std::int64_t required_bytes, double margin,
                     const std::function<std::uint64_t(const std::filesystem::path&)>& free_bytes) {
    if (required_bytes <= 0) {
        return std::nullopt;
    }
    std::filesystem::create_directories(path);
    const auto free =
        static_cast<std::int64_t>(free_bytes ? free_bytes(path) : disk_free_bytes(path));
    // int(required_bytes * margin): a double product truncated toward zero.
    const auto needed = static_cast<std::int64_t>(static_cast<double>(required_bytes) * margin);
    if (free < needed) {
        return std::pair{needed, free};
    }
    return std::nullopt;
}

std::string describe_free_space_shortfall(std::int64_t needed, std::int64_t free) {
    return "Not enough disk space: " + detail::format_size_bytes(needed) + " needed, " +
           detail::format_size_bytes(free) + " free on this drive.";
}

bool is_windows_lock_error(const std::error_code& code) {
    if (!detail::kWindows || code.category() != std::system_category()) {
        return false;
    }
    const int value = code.value();
    return value == 5 || value == 32 || value == 33;
}

std::string os_error_text(const std::error_code& code) {
    if (code.category() == std::system_category()) {
        const auto message = detail::system_message(code.value());
        return (detail::kWindows ? "[WinError " : "[Errno ") + std::to_string(code.value()) + "] " +
               message;
    }
    if (code.category() == std::generic_category()) {
        return "[Errno " + std::to_string(code.value()) + "] " + code.message();
    }
    return code.message();
}

std::string os_error_text(const std::system_error& error) {
    std::string text = os_error_text(error.code());
    if (const auto* fs = dynamic_cast<const std::filesystem::filesystem_error*>(&error)) {
        if (!fs->path1().empty()) {
            text += ": " + quoted(fs->path1());
            if (!fs->path2().empty()) {
                text += " -> " + quoted(fs->path2());
            }
        }
    }
    return text;
}

} // namespace fv::download
