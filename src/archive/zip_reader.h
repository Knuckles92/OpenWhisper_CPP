#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "base/cancel.h"

namespace fv::archive {

// The compression of an entry: ZIP_STORED, ZIP_DEFLATED, or any other method (which reading
// refuses as Python's zipfile does without the optional codecs).
enum class Method { Stored, Deflated, Other };

// One central-directory record, as Python's ZipInfo exposes it.
struct EntryInfo {
    std::string name;                  // as ZipEntry::name
    std::uint64_t size = 0;            // file_size
    std::uint64_t compressed_size = 0; // compress_size
    std::uint32_t crc32 = 0;
    // external_attr: on a Unix-made member the st_mode in the high 16 bits.
    std::uint32_t external_attr = 0;
    Method method = Method::Stored;
    bool directory = false; // is_dir(): the name ends with "/"
};

struct ZipEntry {
    // As Python's ZipInfo.filename: UTF-8 when the entry says so, else decoded from code page
    // 437; cut at a NUL; "\" turned into "/" on Windows.
    std::string name;
    bool is_dir = false; // the name ends with "/"
    std::int64_t size = 0;
};

// Reads a zip (Zip64 included) entry by entry through minizip-ng. Throws BadArchive for a file
// that is not a zip, a damaged entry or a CRC mismatch, and std::system_error when the file
// cannot be opened.
// Affinity: one thread at a time.
class ZipReader {
public:
    explicit ZipReader(const std::filesystem::path& archive);
    // Release archives have a bounded directory and require literal forward-slash names.
    struct Limits {
        std::size_t members;
        std::size_t name_bytes;
        bool literal_names;
    };
    ZipReader(const std::filesystem::path& archive, Limits limits);
    ~ZipReader();
    ZipReader(const ZipReader&) = delete;
    ZipReader& operator=(const ZipReader&) = delete;

    // The central directory, in its order (Python's infolist()).
    const std::vector<ZipEntry>& entries() const;

    // Streams entry `index`'s uncompressed bytes to `sink`, then checks its CRC. Reading entries
    // in increasing order is cheapest.
    void read(std::size_t index, const std::function<void(std::span<const char>)>& sink);

    // Every central-directory record, in order, duplicates included (Python's infolist()).
    const std::vector<EntryInfo>& infolist() const;

    // zf.open(name).read(), streamed to `sink` in blocks: the last entry with that name (Python's
    // NameToInfo keeps the last), its CRC checked at the end. `cancel` is checked before each
    // block (ExtractCanceled). Throws std::out_of_range for a name no entry has, and
    // BadArchive when the local header names a different file than the central directory.
    void read_entry(std::string_view name,
                    const std::function<void(std::span<const std::byte>)>& sink,
                    const CancelToken& cancel = {});

private:
    void read_index(std::size_t index, bool check_local_name,
                    const std::function<void(std::span<const std::byte>)>& sink,
                    const CancelToken& cancel);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::archive
