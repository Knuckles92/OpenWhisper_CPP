#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string_view>

// Python's buffered open(), os.fsync and os.replace for the spool's files. win/files.cpp and
// posix/files.cpp hold the platform halves. Failures throw std::system_error.
namespace fv::meeting::detail {

// Affinity: one thread at a time; blocks on disk I/O.
class BinaryFile {
public:
    BinaryFile() = default;
    ~BinaryFile();
    BinaryFile(BinaryFile&& other) noexcept;
    BinaryFile& operator=(BinaryFile&& other) noexcept;
    BinaryFile(const BinaryFile&) = delete;
    BinaryFile& operator=(const BinaryFile&) = delete;

    // `mode` is open()'s: "rb", "wb", "ab" or "r+b".
    static BinaryFile open(const std::filesystem::path& path, const char* mode);

    bool is_open() const { return file_ != nullptr; }
    void write(std::span<const std::byte> bytes);
    void write(std::string_view text);
    // Up to out.size() bytes; fewer only at the end of the file.
    std::size_t read(std::span<std::byte> out);
    void seek(std::uint64_t offset);
    void flush();
    // os.fsync(handle.fileno()): flushes the buffer, then the OS's.
    void sync();
    void close();

private:
    explicit BinaryFile(std::FILE* file) : file_(file) {}
    void sync_descriptor(); // platform half

    std::FILE* file_ = nullptr;
};

std::FILE* open_file(const std::filesystem::path& path, const char* mode); // platform half
// os.replace: moves `from` over `to`, replacing it.
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to);
// Names the calling thread for debuggers and crash dumps.
void set_thread_name(std::string_view name);

} // namespace fv::meeting::detail
