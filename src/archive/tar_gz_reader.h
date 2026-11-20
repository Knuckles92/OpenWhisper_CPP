#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace fv::archive {

// One member of a tar archive as Python's tarfile.TarInfo describes it, after GNU long names
// and pax headers have been applied.
struct TarMember {
    // Header type flags.
    static constexpr char kRegular = '0';
    static constexpr char kOldRegular = '\0';
    static constexpr char kHardLink = '1';
    static constexpr char kSymlink = '2';
    static constexpr char kCharDevice = '3';
    static constexpr char kBlockDevice = '4';
    static constexpr char kDirectory = '5';
    static constexpr char kFifo = '6';
    static constexpr char kContiguous = '7';

    std::string name; // a directory's without its trailing "/"
    std::string linkname;
    char type = kRegular;
    std::int64_t mode = 0;
    std::int64_t size = 0;
    double mtime = 0; // seconds since 1970; pax may give fractions
    std::size_t index = 0;

    bool is_regular() const {
        return type == kRegular || type == kOldRegular || type == kContiguous;
    }
    bool is_directory() const { return type == kDirectory; }
    bool is_symlink() const { return type == kSymlink; }
    bool is_hardlink() const { return type == kHardLink; }
};

// Reads a .tar.gz front to back (gzip members concatenated as gzip allows), as tarfile's "r:gz"
// mode does: ustar, GNU long names and links, pax and global pax headers. A bad header after the
// first ends the archive quietly, as tarfile does; sparse members are refused. Throws BadArchive
// for a file that is not gzip or tar ("not a gzip file", "empty file", "bad checksum", "unexpected
// end of data", "zlib error: ...") and std::system_error when the file cannot be opened.
// Affinity: one thread at a time.
class TarGzReader {
public:
    explicit TarGzReader(const std::filesystem::path& archive);
    ~TarGzReader();
    TarGzReader(const TarGzReader&) = delete;
    TarGzReader& operator=(const TarGzReader&) = delete;

    // The next member, skipping what is left of the current one's data; nullopt at the end.
    std::optional<TarMember> next();
    // The current member's data; fewer bytes than asked only at its end.
    std::size_t read(std::span<char> buffer);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Every member's header, in order (TarFile.getmembers()).
std::vector<TarMember> list_tar_gz(const std::filesystem::path& archive);

} // namespace fv::archive
