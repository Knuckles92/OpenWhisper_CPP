#pragma once

// Archives made for the tests: tar.gz through zlib with ustar, GNU long-name and pax headers as
// Python's tarfile writes them, and zip through minizip-ng's writer.

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <zlib.h>

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "base/utf8.h"

namespace fv::testing {

class TarBuilder {
public:
    static constexpr char kRegular = '0';
    static constexpr char kHardLink = '1';
    static constexpr char kSymlink = '2';
    static constexpr char kDirectory = '5';
    static constexpr char kFifo = '6';

    struct Entry {
        std::string name;
        char type = kRegular;
        std::string data;
        std::string linkname;
        int mode = 0644;
        long long mtime = 1'700'000'000;
    };

    void add(Entry entry) {
        header(entry.name, entry.type, entry.data, entry.linkname, entry.mode, entry.mtime);
    }

    void add(std::string name, std::string data, int mode = 0644) {
        add(Entry{std::move(name), kRegular, std::move(data), "", mode});
    }

    // A GNU "././@LongLink" header carrying the name, then the member under a cut name.
    void add_long_name(const Entry& entry) {
        header("././@LongLink", 'L', entry.name + '\0', "", 0644, 0);
        header(entry.name.substr(0, 99), entry.type, entry.data, entry.linkname, entry.mode,
               entry.mtime);
    }

    // A pax header ('x' for the next member, 'g' for all that follow), then (for 'x') the member.
    void add_pax(char kind, const std::map<std::string, std::string>& records,
                 const Entry* entry = nullptr) {
        std::string body;
        for (const auto& [key, value] : records) {
            const std::string record = " " + key + "=" + value + "\n";
            std::size_t length = record.size() + 1;
            while (std::to_string(length).size() + record.size() != length) {
                ++length;
            }
            body += std::to_string(length) + record;
        }
        header("pax_header", kind, body, "", 0644, 0);
        if (entry != nullptr) {
            header(entry->name, entry->type, entry->data, entry->linkname, entry->mode,
                   entry->mtime);
        }
    }

    // Raw bytes appended as they are (for damaged archives).
    void append_raw(const std::string& bytes) { tar_ += bytes; }

    const std::string& tar() const { return tar_; }

    std::string finished_tar() const { return tar_ + std::string(1024, '\0'); }

    void save_gz(const std::filesystem::path& path, bool finish = true) const {
        write(path, gzip(finish ? finished_tar() : tar_));
    }

    static std::string gzip(const std::string& data) {
        z_stream zs{};
        deflateInit2(&zs, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 16 + MAX_WBITS, 8, Z_DEFAULT_STRATEGY);
        std::string out(deflateBound(&zs, static_cast<uLong>(data.size())) + 64, '\0');
        zs.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
        zs.avail_in = static_cast<uInt>(data.size());
        zs.next_out = reinterpret_cast<Bytef*>(out.data());
        zs.avail_out = static_cast<uInt>(out.size());
        deflate(&zs, Z_FINISH);
        out.resize(zs.total_out);
        deflateEnd(&zs);
        return out;
    }

    static void write(const std::filesystem::path& path, const std::string& bytes) {
        std::ofstream(path, std::ios::binary) << bytes;
    }

private:
    static void put(std::string& block, std::size_t offset, std::size_t width,
                    const std::string& value) {
        block.replace(offset, std::min(width, value.size()), value.substr(0, width));
    }

    static std::string octal(long long value, std::size_t width) {
        char buffer[32];
        std::snprintf(buffer, sizeof(buffer), "%0*llo", static_cast<int>(width - 1), value);
        return std::string(buffer) + '\0';
    }

    void header(const std::string& name, char type, const std::string& data,
                const std::string& linkname, int mode, long long mtime) {
        std::string block(512, '\0');
        put(block, 0, 100, name);
        put(block, 100, 8, octal(mode, 8));
        put(block, 108, 8, octal(0, 8));
        put(block, 116, 8, octal(0, 8));
        put(block, 124, 12, octal(static_cast<long long>(data.size()), 12));
        put(block, 136, 12, octal(mtime, 12));
        put(block, 148, 8, std::string(8, ' '));
        block[156] = type;
        put(block, 157, 100, linkname);
        put(block, 257, 6, std::string("ustar\0", 6));
        put(block, 263, 2, "00");
        unsigned sum = 0;
        for (const char c : block) {
            sum += static_cast<unsigned char>(c);
        }
        char checksum[8];
        std::snprintf(checksum, sizeof(checksum), "%06o", sum);
        put(block, 148, 8, std::string(checksum, 6) + std::string("\0 ", 2));
        tar_ += block;
        tar_ += data;
        tar_ += std::string((512 - data.size() % 512) % 512, '\0');
    }

    std::string tar_;
};

struct ZipMember {
    std::string name; // the raw bytes stored as the name
    std::string data;
    bool utf8 = true;
    bool store = false;
};

inline void make_zip(const std::filesystem::path& path, const std::vector<ZipMember>& members) {
    void* writer = mz_zip_writer_create();
    if (mz_zip_writer_open_file(writer, path_to_utf8(path).c_str(), 0, 0) != MZ_OK) {
        mz_zip_writer_delete(&writer);
        throw std::runtime_error("cannot create the zip");
    }
    for (const auto& member : members) {
        mz_zip_file info{};
        info.filename = member.name.c_str();
        info.modified_date = std::time(nullptr);
        info.version_madeby = (MZ_HOST_SYSTEM_MSDOS << 8) | 45;
        info.compression_method =
            member.store ? MZ_COMPRESS_METHOD_STORE : MZ_COMPRESS_METHOD_DEFLATE;
        info.flag = member.utf8 ? MZ_ZIP_FLAG_UTF8 : 0;
        mz_zip_writer_add_buffer(writer, const_cast<char*>(member.data.data()),
                                 static_cast<std::int32_t>(member.data.size()), &info);
    }
    mz_zip_writer_close(writer);
    mz_zip_writer_delete(&writer);
}

} // namespace fv::testing
