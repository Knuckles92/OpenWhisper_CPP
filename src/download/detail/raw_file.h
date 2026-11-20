#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>

// A file handle that is opened without following a final link, for `.part` files and for
// sha256_file(regular_only). Every failure throws std::system_error with the platform's code.
namespace fv::download::detail {

struct FileInfo {
    bool regular = false; // an ordinary file (not a directory, link, device or pipe)
    std::uint64_t links = 0;
    std::uint64_t size = 0;
};

// Affinity: one thread at a time.
class RawFile {
public:
    // Read and write, created when missing (owner-only on POSIX). A final link is not followed:
    // POSIX refuses it (O_NOFOLLOW) and so does Windows (a reparse point is refused once open).
    static RawFile open_partial(const std::filesystem::path&);
    // Read only, a final link refused as above.
    static RawFile open_read_nofollow(const std::filesystem::path&);

    RawFile() = default;
    ~RawFile();
    RawFile(RawFile&&) noexcept;
    RawFile& operator=(RawFile&&) noexcept;
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    FileInfo info() const;
    // Fewer bytes than asked only at the end of the file; 0 there.
    std::size_t read(std::span<char>);
    void write(std::span<const char>);
    void seek(std::uint64_t offset);
    void truncate(std::uint64_t size); // at the current position's file, not moving it
    void sync();                       // the data on stable storage (fsync, FlushFileBuffers)
    void make_private();               // chmod 0600 on POSIX; nothing on Windows (as Python 3.12)
    void close();

private:
#ifdef _WIN32
    void* handle_ = nullptr;
#else
    int fd_ = -1;
#endif
};

} // namespace fv::download::detail
