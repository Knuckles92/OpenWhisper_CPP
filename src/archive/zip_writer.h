#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string_view>

#include "archive/zip_reader.h"

namespace fv::archive {

// Writes a zip as Python's zipfile writes one with ZipInfo(name), compress_type and
// external_attr set and force_zip64=True: every entry carries Zip64 extra fields, the date is
// 1980-01-01 00:00 (ZipInfo's default), deflate uses zlib's default level, and a non-ASCII name
// sets the UTF-8 flag. Names are written as given, unchecked (tests build hostile archives with
// it). Options{.zip64 = false} writes entries as zipfile does without force_zip64, the way
// python-docx writes a .docx: no Zip64 extra fields, version 2.0.
//
// Streaming: an entry's bytes go to the file as they come. An entry that fails part way (a
// callback throws, a read or write fails) leaves the writer unusable; destroying a writer that
// was never closed removes its file, so a half-written archive is never left behind.
// Affinity: one thread at a time.
class ZipWriter {
public:
    struct Options {
        // false: no Zip64 extra fields, so an entry of 4 GiB or more fails as it closes.
        bool zip64 = true;
    };

    // Creates (or truncates) the file. std::filesystem::filesystem_error when it cannot, and
    // (no_such_file_or_directory) when its folder does not exist: no folder is ever made.
    static ZipWriter create(const std::filesystem::path& path);
    static ZipWriter create(const std::filesystem::path& path, Options options);

    ZipWriter(ZipWriter&&) noexcept;
    ZipWriter& operator=(ZipWriter&&) noexcept;
    ZipWriter(const ZipWriter&) = delete;
    ZipWriter& operator=(const ZipWriter&) = delete;
    ~ZipWriter();

    // The file at `source` as entry `name`. `chunk` sees each block of at most 1 MiB before it
    // is written (hashing, counting, cancel by throwing).
    void add_file(std::string_view name, const std::filesystem::path& source, Method method,
                  std::uint32_t external_attr,
                  const std::function<void(std::span<const std::byte>)>& chunk);

    // An entry whose bytes come from `read`, called with a buffer of 1 MiB until it returns 0.
    void add_stream(std::string_view name, Method method, std::uint32_t external_attr,
                    const std::function<std::size_t(std::span<std::byte>)>& read);

    void add_bytes(std::string_view name, std::span<const std::byte> bytes, Method method,
                   std::uint32_t external_attr);

    // Writes the central directory and closes the file; throws std::runtime_error on any write
    // error. Idempotent.
    void close();

    const std::filesystem::path& path() const;

private:
    struct Impl;
    explicit ZipWriter(std::unique_ptr<Impl>);
    std::unique_ptr<Impl> impl_;
};

} // namespace fv::archive
