#pragma once

#include <cstddef>
#include <filesystem>
#include <memory>
#include <span>
#include <utility>

// A file created exclusively (os.open with O_CREAT | O_EXCL, mode 0600) and written with the
// C runtime: the recordings copy of history_manager._save_recording.
namespace fv::history_detail {

class ExclusiveFile {
public:
    ~ExclusiveFile();
    ExclusiveFile(const ExclusiveFile&) = delete;
    ExclusiveFile& operator=(const ExclusiveFile&) = delete;

    // nullptr when the path exists (FileExistsError); std::filesystem::filesystem_error for any
    // other failure.
    static std::unique_ptr<ExclusiveFile> create(const std::filesystem::path& path);

    // Each throws std::filesystem::filesystem_error on failure.
    void write(std::span<const std::byte> bytes);
    // flush, then fsync (_commit on Windows).
    void sync();
    void close();

private:
    ExclusiveFile(std::filesystem::path path, int fd) : path_(std::move(path)), fd_(fd) {}

    std::filesystem::path path_;
    int fd_ = -1;
};

// Whether two paths name the same file as os.path.normcase(os.path.abspath(...)) compares
// them: case-insensitively on Windows.
bool same_normalized_path(const std::filesystem::path& a, const std::filesystem::path& b);

} // namespace fv::history_detail
