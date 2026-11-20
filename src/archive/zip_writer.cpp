#include "archive/zip_writer.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

#include "base/utf8.h"

namespace fv::archive {

namespace {

constexpr std::size_t kBlock = std::size_t{1} << 20;

// ZipInfo.create_system: 0 (MS-DOS) when made on Windows, 3 (Unix) elsewhere. The version is 45,
// which Zip64 needs and zipfile writes for every forced Zip64 entry, or else 20 (DEFAULT_VERSION,
// deflate's).
#ifdef _WIN32
constexpr std::uint16_t kHost = MZ_HOST_SYSTEM_MSDOS << 8;
#else
constexpr std::uint16_t kHost = MZ_HOST_SYSTEM_UNIX << 8;
#endif
constexpr std::uint16_t kMadeByZip64 = kHost | 45;
constexpr std::uint16_t kMadeBy = kHost | 20;

[[noreturn]] void fail(const std::filesystem::path& path, std::string_view what) {
    throw std::filesystem::filesystem_error(std::string(what), path,
                                            std::make_error_code(std::errc::io_error));
}

// ZipInfo's default date_time, (1980, 1, 1, 0, 0, 0), as the local time minizip-ng converts
// back to a DOS date.
std::time_t zip_epoch() {
    std::tm tm{};
    tm.tm_year = 80;
    tm.tm_mon = 0;
    tm.tm_mday = 1;
    tm.tm_isdst = -1;
    return std::mktime(&tm);
}

std::uint16_t compression(Method method) {
    switch (method) {
    case Method::Stored:
        return MZ_COMPRESS_METHOD_STORE;
    case Method::Deflated:
        return MZ_COMPRESS_METHOD_DEFLATE;
    case Method::Other:
        break;
    }
    throw std::invalid_argument("ZipWriter writes stored or deflated entries only");
}

} // namespace

struct ZipWriter::Impl {
    std::filesystem::path path;
    void* writer = nullptr;
    bool closed = false;
    bool broken = false;
    bool zip64 = true;
    std::vector<std::byte> buffer;

    ~Impl() {
        if (writer != nullptr) {
            mz_zip_writer_delete(&writer);
        }
        if (!closed) {
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    }

    void require_open() const {
        if (closed || broken || writer == nullptr) {
            throw std::logic_error("ZipWriter is closed or failed part way");
        }
    }

    void open_entry(std::string_view name, Method method, std::uint32_t external_attr) {
        require_open();
        const std::string filename(name);
        mz_zip_file info{};
        info.version_madeby = zip64 ? kMadeByZip64 : kMadeBy;
        info.compression_method = compression(method);
        info.modified_date = zip_epoch();
        info.filename = filename.c_str();
        info.external_fa = external_attr;
        info.zip64 = zip64 ? MZ_ZIP64_FORCE : MZ_ZIP64_DISABLE;
        if (std::ranges::any_of(filename, [](char c) { return (c & 0x80) != 0; })) {
            info.flag |= MZ_ZIP_FLAG_UTF8;
        }
        if (mz_zip_writer_entry_open(writer, &info) != MZ_OK) {
            broken = true;
            fail(path, "Cannot add a zip entry");
        }
    }

    void write(std::span<const std::byte> block) {
        std::size_t done = 0;
        while (done < block.size()) {
            const auto part =
                static_cast<std::int32_t>(std::min<std::size_t>(block.size() - done, kBlock));
            const std::int32_t written =
                mz_zip_writer_entry_write(writer, block.data() + done, part);
            if (written != part) {
                broken = true;
                fail(path, "Cannot write a zip entry");
            }
            done += static_cast<std::size_t>(part);
        }
    }

    void close_entry() {
        if (mz_zip_writer_entry_close(writer) != MZ_OK) {
            broken = true;
            fail(path, "Cannot finish a zip entry");
        }
    }

    // Runs `body` with the entry open; anything it throws leaves the writer broken.
    template <class F>
    void entry(std::string_view name, Method method, std::uint32_t attr, F&& body) {
        open_entry(name, method, attr);
        try {
            body();
        } catch (...) {
            broken = true;
            throw;
        }
        close_entry();
    }
};

ZipWriter::ZipWriter(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
ZipWriter::ZipWriter(ZipWriter&&) noexcept = default;
ZipWriter& ZipWriter::operator=(ZipWriter&&) noexcept = default;
ZipWriter::~ZipWriter() = default;

ZipWriter ZipWriter::create(const std::filesystem::path& path) {
    return create(path, Options{});
}

ZipWriter ZipWriter::create(const std::filesystem::path& path, Options options) {
    // mz_zip_writer_open_file makes a missing parent folder, from a copy of the path cut to 319
    // bytes (a long path could leave a truncated stray folder); zipfile's open() raises
    // FileNotFoundError instead. With the parent in place it makes nothing: a cut path names
    // one of the parent's ancestors.
    if (std::error_code error;
        !path.parent_path().empty() && !std::filesystem::is_directory(path.parent_path(), error)) {
        throw std::filesystem::filesystem_error(
            "Cannot create the zip file", path,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    auto impl = std::make_unique<Impl>();
    impl->path = path;
    impl->zip64 = options.zip64;
    impl->writer = mz_zip_writer_create();
    if (impl->writer == nullptr) {
        throw std::bad_alloc();
    }
    mz_zip_writer_set_compress_level(impl->writer, MZ_COMPRESS_LEVEL_DEFAULT);
    if (mz_zip_writer_open_file(impl->writer, path_to_utf8(path).c_str(), 0, 0) != MZ_OK) {
        impl->closed = true; // nothing of ours to remove
        throw std::filesystem::filesystem_error("Cannot create the zip file", path,
                                                std::make_error_code(std::errc::permission_denied));
    }
    return ZipWriter(std::move(impl));
}

void ZipWriter::add_stream(std::string_view name, Method method, std::uint32_t external_attr,
                           const std::function<std::size_t(std::span<std::byte>)>& read) {
    impl_->buffer.resize(kBlock);
    impl_->entry(name, method, external_attr, [&] {
        while (true) {
            const std::size_t got = read(std::span<std::byte>(impl_->buffer));
            if (got == 0) {
                break;
            }
            impl_->write(std::span<const std::byte>(impl_->buffer.data(),
                                                    std::min(got, impl_->buffer.size())));
        }
    });
}

void ZipWriter::add_file(std::string_view name, const std::filesystem::path& source, Method method,
                         std::uint32_t external_attr,
                         const std::function<void(std::span<const std::byte>)>& chunk) {
    std::ifstream in(source, std::ios::binary);
    if (!in) {
        throw std::filesystem::filesystem_error(
            "Cannot open the file", source,
            std::make_error_code(std::errc::no_such_file_or_directory));
    }
    add_stream(name, method, external_attr, [&](std::span<std::byte> buffer) -> std::size_t {
        in.read(reinterpret_cast<char*>(buffer.data()),
                static_cast<std::streamsize>(buffer.size()));
        if (in.bad()) {
            fail(source, "Cannot read the file");
        }
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got > 0 && chunk) {
            chunk(std::span<const std::byte>(buffer.data(), got));
        }
        return got;
    });
}

void ZipWriter::add_bytes(std::string_view name, std::span<const std::byte> bytes, Method method,
                          std::uint32_t external_attr) {
    impl_->entry(name, method, external_attr, [&] { impl_->write(bytes); });
}

void ZipWriter::close() {
    if (impl_->closed) {
        return;
    }
    impl_->require_open();
    const std::int32_t status = mz_zip_writer_close(impl_->writer);
    mz_zip_writer_delete(&impl_->writer);
    if (status != MZ_OK) {
        impl_->broken = true;
        fail(impl_->path, "Cannot write the zip central directory");
    }
    impl_->closed = true;
}

const std::filesystem::path& ZipWriter::path() const {
    return impl_->path;
}

} // namespace fv::archive
