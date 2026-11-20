#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "audio/wav_metadata.h"

// Unbuffered file I/O on the OS descriptor (Win32 HANDLE in win/, a POSIX fd in posix/), for
// WAV writing and tagging. Every failure throws std::system_error.
namespace fv::audio::detail {

class RawFile final : public RiffFile {
public:
    RawFile() = default;
    ~RawFile() override;
    RawFile(RawFile&& other) noexcept;
    RawFile& operator=(RawFile&& other) noexcept;
    RawFile(const RawFile&) = delete;
    RawFile& operator=(const RawFile&) = delete;

    // An existing file, for reading and writing.
    static RawFile open_existing(const std::filesystem::path& path);
    // A new file "tmp<8 random characters><suffix>" in `directory`, created exclusively as
    // Python's tempfile.mkstemp does; its path is put in `created`.
    static RawFile create_temp(const std::filesystem::path& directory, std::string_view suffix,
                               std::filesystem::path& created);

    bool is_open() const;
    std::uint64_t seek_end() override;
    void seek(std::uint64_t offset) override;
    std::size_t write(std::span<const std::byte> data) override;
    void truncate(std::uint64_t size) override;
    // Every byte, or std::system_error; 0 bytes accepted is "Recording storage accepted no
    // more data".
    void write_all(std::span<const std::byte> data);
    void sync(); // FlushFileBuffers / fsync
    void close();

private:
    std::intptr_t handle_ = -1; // HANDLE or fd; -1 when closed
};

// "tmp" + 8 characters of tempfile's alphabet + suffix.
std::string random_temp_name(std::string_view suffix);

// Moves `from` over `to`, replacing it (os.replace). Throws std::system_error.
void replace_file(const std::filesystem::path& from, const std::filesystem::path& to);

} // namespace fv::audio::detail
