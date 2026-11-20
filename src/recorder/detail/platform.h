#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

// Unbuffered file I/O on the OS descriptor (a Win32 HANDLE in win/, a POSIX fd in posix/) for the
// recording journal, its OS lease and recovery: Python's open(..., buffering=0), os.fsync,
// msvcrt.locking / fcntl.flock and os.replace. Every failure throws std::system_error whose code
// compares equal to the std::errc Python's OSError subclass stands for (no_such_file_or_directory
// for FileNotFoundError, permission_denied for PermissionError).
namespace fv::recorder_detail {

// Affinity: one thread at a time; blocks on disk I/O.
class OsFile {
public:
    OsFile() = default;
    ~OsFile();
    OsFile(OsFile&& other) noexcept;
    OsFile& operator=(OsFile&& other) noexcept;
    OsFile(const OsFile&) = delete;
    OsFile& operator=(const OsFile&) = delete;

    // open(path, "x+b"): a new file, refused when it exists (errc::file_exists).
    static OsFile create_new(const std::filesystem::path& path);
    // open(path, "a+b"): created when missing, for reading and appending.
    static OsFile open_append(const std::filesystem::path& path);
    // open(path, "rb").
    static OsFile open_read(const std::filesystem::path& path);
    // open(path, "wb"): created or truncated.
    static OsFile create_truncate(const std::filesystem::path& path);

    bool is_open() const;
    // One write call; returns the bytes the OS took (possibly fewer than given).
    std::size_t write(std::span<const std::byte> data);
    // Every byte, or std::system_error (0 bytes taken reads as no space left).
    void write_all(std::span<const std::byte> data);
    // Up to `count` bytes from the current position; fewer only at the end of the file.
    std::size_t read(std::span<std::byte> out);
    void seek(std::uint64_t offset);
    std::uint64_t size() const;
    void sync(); // FlushFileBuffers / fsync
    void close();

    // A non-blocking exclusive lock on the first byte (msvcrt.locking(LK_NBLCK, 1) /
    // flock(LOCK_EX | LOCK_NB)); false when another holder has it. The OS drops it when the
    // file closes or the process dies.
    bool try_lock();

private:
    explicit OsFile(std::intptr_t handle) : handle_(handle) {}

    std::intptr_t handle_ = -1; // HANDLE or fd; -1 when closed
};

// os.replace: moves `from` over `to`, replacing it.
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to);

// Names the calling thread for debuggers and crash dumps.
void set_thread_name(std::string_view name);

} // namespace fv::recorder_detail
