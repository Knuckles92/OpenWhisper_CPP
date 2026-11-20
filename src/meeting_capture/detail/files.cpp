#include "meeting_capture/detail/files.h"

#include <cerrno>
#include <system_error>
#include <utility>

namespace fv::meeting::detail {

namespace {

[[noreturn]] void throw_errno(int code, const char* what) {
    throw std::system_error(code == 0 ? EIO : code, std::generic_category(), what);
}

} // namespace

BinaryFile::~BinaryFile() {
    if (file_ != nullptr) {
        std::fclose(file_);
    }
}

BinaryFile::BinaryFile(BinaryFile&& other) noexcept : file_(std::exchange(other.file_, nullptr)) {}

BinaryFile& BinaryFile::operator=(BinaryFile&& other) noexcept {
    if (this != &other) {
        if (file_ != nullptr) {
            std::fclose(file_);
        }
        file_ = std::exchange(other.file_, nullptr);
    }
    return *this;
}

BinaryFile BinaryFile::open(const std::filesystem::path& path, const char* mode) {
    errno = 0;
    std::FILE* file = open_file(path, mode);
    if (file == nullptr) {
        throw_errno(errno, "Could not open the file");
    }
    return BinaryFile(file);
}

void BinaryFile::write(std::span<const std::byte> bytes) {
    if (bytes.empty()) {
        return;
    }
    errno = 0;
    if (std::fwrite(bytes.data(), 1, bytes.size(), file_) != bytes.size()) {
        throw_errno(errno, "Could not write the file");
    }
}

void BinaryFile::write(std::string_view text) {
    write(std::as_bytes(std::span(text.data(), text.size())));
}

std::size_t BinaryFile::read(std::span<std::byte> out) {
    if (out.empty()) {
        return 0;
    }
    errno = 0;
    const std::size_t got = std::fread(out.data(), 1, out.size(), file_);
    if (got < out.size() && std::ferror(file_) != 0) {
        throw_errno(errno, "Could not read the file");
    }
    return got;
}

void BinaryFile::seek(std::uint64_t offset) {
    // fseek takes a long, which is 32 bits on Windows; step there in pieces.
    errno = 0;
    if (std::fseek(file_, 0, SEEK_SET) != 0) {
        throw_errno(errno, "Could not seek");
    }
    constexpr std::uint64_t step = 1U << 30;
    while (offset > 0) {
        const std::uint64_t piece = offset < step ? offset : step;
        if (std::fseek(file_, static_cast<long>(piece), SEEK_CUR) != 0) {
            throw_errno(errno, "Could not seek");
        }
        offset -= piece;
    }
}

void BinaryFile::flush() {
    errno = 0;
    if (std::fflush(file_) != 0) {
        throw_errno(errno, "Could not flush the file");
    }
}

void BinaryFile::sync() {
    flush();
    sync_descriptor();
}

void BinaryFile::close() {
    std::FILE* file = std::exchange(file_, nullptr);
    if (file == nullptr) {
        return;
    }
    errno = 0;
    if (std::fclose(file) != 0) {
        throw_errno(errno, "Could not close the file");
    }
}

} // namespace fv::meeting::detail
