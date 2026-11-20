#include "archive/zip_reader.h"

#include <limits>

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <array>
#include <optional>
#include <stdexcept>
#include <system_error>
#include <unordered_map>

#include "archive/detail/text.h"
#include "archive/errors.h"
#include "base/utf8.h"

namespace fv::archive {

namespace {

// Code page 437's upper half, as Python's cp437 codec decodes it.
constexpr std::array<char16_t, 128> kCp437 = {
    0x00C7, 0x00FC, 0x00E9, 0x00E2, 0x00E4, 0x00E0, 0x00E5, 0x00E7, 0x00EA, 0x00EB, 0x00E8, 0x00EF,
    0x00EE, 0x00EC, 0x00C4, 0x00C5, 0x00C9, 0x00E6, 0x00C6, 0x00F4, 0x00F6, 0x00F2, 0x00FB, 0x00F9,
    0x00FF, 0x00D6, 0x00DC, 0x00A2, 0x00A3, 0x00A5, 0x20A7, 0x0192, 0x00E1, 0x00ED, 0x00F3, 0x00FA,
    0x00F1, 0x00D1, 0x00AA, 0x00BA, 0x00BF, 0x2310, 0x00AC, 0x00BD, 0x00BC, 0x00A1, 0x00AB, 0x00BB,
    0x2591, 0x2592, 0x2593, 0x2502, 0x2524, 0x2561, 0x2562, 0x2556, 0x2555, 0x2563, 0x2551, 0x2557,
    0x255D, 0x255C, 0x255B, 0x2510, 0x2514, 0x2534, 0x252C, 0x251C, 0x2500, 0x253C, 0x255E, 0x255F,
    0x255A, 0x2554, 0x2569, 0x2566, 0x2560, 0x2550, 0x256C, 0x2567, 0x2568, 0x2564, 0x2565, 0x2559,
    0x2558, 0x2552, 0x2553, 0x256B, 0x256A, 0x2518, 0x250C, 0x2588, 0x2584, 0x258C, 0x2590, 0x2580,
    0x03B1, 0x00DF, 0x0393, 0x03C0, 0x03A3, 0x03C3, 0x00B5, 0x03C4, 0x03A6, 0x0398, 0x03A9, 0x03B4,
    0x221E, 0x03C6, 0x03B5, 0x2229, 0x2261, 0x00B1, 0x2265, 0x2264, 0x2320, 0x2321, 0x00F7, 0x2248,
    0x00B0, 0x2219, 0x00B7, 0x221A, 0x207F, 0x00B2, 0x25A0, 0x00A0,
};

void append_utf8(std::string& out, char32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

// The name as stored, decoded as UTF-8 or code page 437 by the entry's flag (ZipInfo's
// orig_filename).
std::string decode_text(const char* raw, std::uint16_t flags) {
    std::string name = raw != nullptr ? raw : "";
    if ((flags & MZ_ZIP_FLAG_UTF8) == 0) {
        std::string decoded;
        for (const char c : name) {
            const auto byte = static_cast<unsigned char>(c);
            append_utf8(decoded, byte < 0x80 ? byte : kCp437[byte - 0x80]);
        }
        name = std::move(decoded);
    }
    return name;
}

std::string decode_name(const char* raw, std::uint16_t flags) {
    std::string name = decode_text(raw, flags);
#ifdef _WIN32
    for (auto& c : name) {
        if (c == '\\') {
            c = '/';
        }
    }
#endif
    return name;
}

// Python's repr() of a bytes object: b'...', printable ASCII kept, \t \n \r and the quote and
// backslash escaped, every other byte as \xNN.
std::string bytes_repr(std::string_view bytes) {
    const bool single = bytes.find('\'') != std::string_view::npos;
    const bool double_quote = bytes.find('"') != std::string_view::npos;
    const char quote = single && !double_quote ? '"' : '\'';
    constexpr std::string_view kHex = "0123456789abcdef";
    std::string out = "b";
    out += quote;
    for (const char c : bytes) {
        const auto byte = static_cast<unsigned char>(c);
        if (c == '\\' || c == quote) {
            out += '\\';
            out += c;
        } else if (c == '\t') {
            out += "\\t";
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (byte < 0x20 || byte >= 0x7F) {
            out += "\\x";
            out += kHex[byte >> 4];
            out += kHex[byte & 0x0F];
        } else {
            out += c;
        }
    }
    out += quote;
    return out;
}

Method method_of(std::uint16_t compression_method) {
    switch (compression_method) {
    case MZ_COMPRESS_METHOD_STORE:
        return Method::Stored;
    case MZ_COMPRESS_METHOD_DEFLATE:
        return Method::Deflated;
    default:
        return Method::Other;
    }
}

} // namespace

struct ZipReader::Impl {
    void* reader = nullptr;
    std::vector<ZipEntry> entries;
    std::vector<EntryInfo> infos;
    // Each entry's orig_filename, for the local-header check.
    std::vector<std::string> stored_names;
    // Each name's last entry (NameToInfo), so that reading every member of a large bundle by
    // name stays linear.
    std::unordered_map<std::string, std::size_t> by_name;
    std::optional<std::size_t> current;
    bool literal_names = false;

    ~Impl() {
        if (reader != nullptr) {
            mz_zip_reader_close(reader);
            mz_zip_reader_delete(&reader);
        }
    }

    void go_to(std::size_t index) {
        if (!current || index < *current) {
            if (mz_zip_reader_goto_first_entry(reader) != MZ_OK) {
                throw BadArchive("Bad magic number for central directory");
            }
            current = 0;
        }
        while (*current < index) {
            if (mz_zip_reader_goto_next_entry(reader) != MZ_OK) {
                throw BadArchive("Bad magic number for central directory");
            }
            ++*current;
        }
    }
};

ZipReader::ZipReader(const std::filesystem::path& archive)
    : ZipReader(archive, {std::numeric_limits<std::size_t>::max(),
                          std::numeric_limits<std::size_t>::max(), false}) {}

ZipReader::ZipReader(const std::filesystem::path& archive, Limits limits)
    : impl_(std::make_unique<Impl>()) {
    impl_->literal_names = limits.literal_names;
    std::error_code error;
    if (!std::filesystem::is_regular_file(archive, error)) {
        throw std::filesystem::filesystem_error(
            "open", archive,
            error ? error : std::make_error_code(std::errc::no_such_file_or_directory));
    }
    impl_->reader = mz_zip_reader_create();
    if (impl_->reader == nullptr ||
        mz_zip_reader_open_file(impl_->reader, path_to_utf8(archive).c_str()) != MZ_OK) {
        throw BadArchive("File is not a zip file");
    }
    std::int32_t status = mz_zip_reader_goto_first_entry(impl_->reader);
    while (status == MZ_OK) {
        mz_zip_file* info = nullptr;
        if (mz_zip_reader_entry_get_info(impl_->reader, &info) != MZ_OK || info == nullptr) {
            throw BadArchive("Bad magic number for central directory");
        }
        if (impl_->entries.size() >= limits.members || info->filename_size > limits.name_bytes) {
            throw BadArchive("Archive directory exceeds its size limits.");
        }
        if (limits.literal_names) {
            if (!info->filename) {
                throw BadArchive("Archive contains an invalid member name.");
            }
            const auto raw = std::string_view(info->filename, info->filename_size);
            if (raw.find('\0') != raw.npos || raw.find('\\') != raw.npos) {
                throw BadArchive("Archive contains an invalid member name.");
            }
        }
        ZipEntry entry;
        entry.name = decode_name(info->filename, info->flag);
        entry.is_dir = entry.name.ends_with('/');
        entry.size = info->uncompressed_size;
        EntryInfo record;
        record.name = entry.name;
        record.size = static_cast<std::uint64_t>(info->uncompressed_size);
        record.compressed_size = static_cast<std::uint64_t>(info->compressed_size);
        record.crc32 = info->crc;
        record.external_attr = info->external_fa;
        record.method = method_of(info->compression_method);
        record.directory = entry.is_dir;
        impl_->by_name[entry.name] = impl_->entries.size();
        impl_->entries.push_back(std::move(entry));
        impl_->infos.push_back(std::move(record));
        impl_->stored_names.push_back(decode_text(info->filename, info->flag));
        status = mz_zip_reader_goto_next_entry(impl_->reader);
    }
    if (status != MZ_END_OF_LIST) {
        throw BadArchive("Bad magic number for central directory");
    }
    impl_->current.reset();
}

ZipReader::~ZipReader() = default;

const std::vector<ZipEntry>& ZipReader::entries() const {
    return impl_->entries;
}

const std::vector<EntryInfo>& ZipReader::infolist() const {
    return impl_->infos;
}

void ZipReader::read(std::size_t index, const std::function<void(std::span<const char>)>& sink) {
    read_index(
        index, false,
        [&](std::span<const std::byte> block) {
            sink(std::span<const char>(reinterpret_cast<const char*>(block.data()), block.size()));
        },
        {});
}

void ZipReader::read_entry(std::string_view name,
                           const std::function<void(std::span<const std::byte>)>& sink,
                           const CancelToken& cancel) {
    if (const auto it = impl_->by_name.find(std::string(name)); it != impl_->by_name.end()) {
        read_index(it->second, true, sink, cancel);
        return;
    }
    throw std::out_of_range("There is no item named " + detail::python_repr(name) +
                            " in the archive");
}

void ZipReader::read_index(std::size_t index, bool check_local_name,
                           const std::function<void(std::span<const std::byte>)>& sink,
                           const CancelToken& cancel) {
    const auto& name = impl_->entries.at(index).name;
    impl_->go_to(index);
    const std::int32_t opened = mz_zip_reader_entry_open(impl_->reader);
    if (opened == MZ_PASSWORD_ERROR) {
        throw BadArchive("File " + detail::python_repr(name) +
                         " is encrypted, password required for extraction");
    }
    if (opened == MZ_SUPPORT_ERROR) {
        throw BadArchive("That compression method is not supported");
    }
    if (opened != MZ_OK) {
        throw BadArchive("Bad magic number for file header");
    }
    if (check_local_name) {
        void* zip = nullptr;
        mz_zip_file* local = nullptr;
        if (mz_zip_reader_get_zip_handle(impl_->reader, &zip) != MZ_OK ||
            mz_zip_entry_get_local_info(zip, &local) != MZ_OK || local == nullptr) {
            mz_zip_reader_entry_close(impl_->reader);
            throw BadArchive("Bad magic number for file header");
        }
        if (impl_->literal_names &&
            (!local->filename ||
             std::string_view(local->filename, local->filename_size).find('\0') !=
                 std::string_view::npos)) {
            mz_zip_reader_entry_close(impl_->reader);
            throw BadArchive("Invalid local member name.");
        }
        const std::string header = decode_text(local->filename, local->flag);
        if (header != impl_->stored_names[index]) {
            mz_zip_reader_entry_close(impl_->reader);
            throw BadArchive("File name in directory " +
                             detail::python_repr(impl_->stored_names[index]) + " and header " +
                             bytes_repr(local->filename != nullptr ? local->filename : "") +
                             " differ.");
        }
    }
    std::array<std::byte, 1 << 16> buffer{};
    while (true) {
        if (cancel.cancelled()) {
            mz_zip_reader_entry_close(impl_->reader);
            throw ExtractCanceled();
        }
        const std::int32_t got = mz_zip_reader_entry_read(impl_->reader, buffer.data(),
                                                          static_cast<std::int32_t>(buffer.size()));
        if (got < 0) {
            mz_zip_reader_entry_close(impl_->reader);
            throw BadArchive("Error -3 while decompressing data");
        }
        if (got == 0) {
            break;
        }
        try {
            sink(std::span<const std::byte>(buffer.data(), static_cast<std::size_t>(got)));
        } catch (...) {
            mz_zip_reader_entry_close(impl_->reader);
            throw;
        }
    }
    if (mz_zip_reader_entry_close(impl_->reader) == MZ_CRC_ERROR) {
        throw BadArchive("Bad CRC-32 for file " + detail::python_repr(name));
    }
}

} // namespace fv::archive
