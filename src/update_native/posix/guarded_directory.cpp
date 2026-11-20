#include "update_native/guarded_directory.h"

#include <array>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>
#ifdef __linux__
#include <linux/fs.h>
#include <sys/syscall.h>
#endif

#include "base/hash.h"
#include "base/ids.h"

// The updater ships for Windows only. This implementation keeps the transaction code, which is
// shared, building and testable elsewhere: every component is opened without following a link
// and every operation is relative to the leaf descriptor.
namespace fv::update_native {
namespace fs = std::filesystem;
namespace {
class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() {
        if (value_ >= 0)
            ::close(value_);
    }
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0)
                ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }

private:
    int value_;
};

[[noreturn]] void fail(const char* operation, int error = errno) {
    throw std::system_error(error, std::system_category(), operation);
}

void check_file(int fd) {
    struct stat info {};
    if (::fstat(fd, &info) != 0)
        fail("Inspecting update file");
    if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
        throw std::runtime_error("An update file is not an ordinary unlinked file.");
}

Fd open_directory(int parent, const char* name) {
    Fd handle(::openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (handle.get() < 0)
        fail("Opening update directory");
    return handle;
}

// A component of the guarded path itself: any single name the file system accepts.
void check_component(const fs::path& name) {
    const auto& value = name.native();
    if (value.empty() || value == "." || value == ".." || value.find('/') != std::string::npos ||
        value.find('\0') != std::string::npos)
        throw std::invalid_argument("Expected a single update directory name.");
}

// A name the transaction passes in: the release layout is Windows', so its rules hold here too.
void check_leaf(const fs::path& name) {
    const auto& value = name.native();
    if (value.empty() || value == "." || value == ".." || value.back() == '.' ||
        value.back() == ' ' || value.find_first_of("\\/:*?\"<>|") != std::string::npos ||
        value.find('\0') != std::string::npos || name.has_root_path() || name.filename() != name)
        throw std::invalid_argument("Expected a single update directory name.");
}

// O_NONBLOCK keeps a planted FIFO from blocking the open; it does nothing to a regular file.
Fd open_leaf(int directory, const fs::path& name) {
    return Fd(::openat(directory, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
}

void write_all(int fd, const char* data, std::size_t size) {
    while (size != 0) {
        const auto written = ::write(fd, data, size);
        if (written < 0) {
            if (errno == EINTR)
                continue;
            fail("Writing update temporary file");
        }
        data += written;
        size -= static_cast<std::size_t>(written);
    }
}

void flush(int fd, const char* operation) {
    if (::fsync(fd) != 0)
        fail(operation);
}

// Creates a private temporary in `directory`, lets `fill` write it, and renames it over `name`.
template <class Fill> void publish(int directory, const fs::path& name, mode_t mode, Fill&& fill) {
    const std::string temporary = ".update-" + uuid4_hex();
    Fd file(::openat(directory, temporary.c_str(),
                     O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, mode));
    if (file.get() < 0)
        fail("Creating update temporary file");
    try {
        fill(file.get());
        flush(file.get(), "Flushing update temporary file");
        if (::renameat(directory, temporary.c_str(), directory, name.c_str()) != 0)
            fail("Publishing update file");
    } catch (...) {
        ::unlinkat(directory, temporary.c_str(), 0);
        throw;
    }
    flush(directory, "Flushing update directory");
}
} // namespace

struct GuardedDirectory::Impl {
    fs::path path;
    std::vector<Fd> components;
    int leaf() const { return components.back().get(); }
};
struct GuardedDirectory::FilePin::Impl {
    explicit Impl(Fd value) : handle(std::move(value)) {}
    Fd handle;
};
GuardedDirectory::FilePin::FilePin(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GuardedDirectory::FilePin::~FilePin() = default;
GuardedDirectory::FilePin::FilePin(FilePin&&) noexcept = default;
GuardedDirectory::FilePin& GuardedDirectory::FilePin::operator=(FilePin&&) noexcept = default;

GuardedDirectory::GuardedDirectory(const fs::path& directory) : impl_(std::make_unique<Impl>()) {
    if (!directory.is_absolute() || directory != directory.lexically_normal())
        throw std::invalid_argument("The update directory must be an absolute normalized path.");
    impl_->path = directory.root_path();
    impl_->components.push_back(open_directory(AT_FDCWD, impl_->path.c_str()));
    for (const auto& component : directory.relative_path()) {
        check_component(component);
        auto next = open_directory(impl_->leaf(), component.c_str());
        impl_->path /= component;
        impl_->components.push_back(std::move(next));
    }
}
GuardedDirectory::~GuardedDirectory() = default;
GuardedDirectory::GuardedDirectory(GuardedDirectory&&) noexcept = default;
GuardedDirectory& GuardedDirectory::operator=(GuardedDirectory&&) noexcept = default;

void GuardedDirectory::rename_child_exclusive(const fs::path& source,
                                              const fs::path& destination) const {
    check_leaf(source);
    check_leaf(destination);
    (void)open_directory(impl_->leaf(), source.c_str());
#if defined(__linux__) && defined(SYS_renameat2)
    if (::syscall(SYS_renameat2, impl_->leaf(), source.c_str(), impl_->leaf(), destination.c_str(),
                  RENAME_NOREPLACE) != 0)
        fail("Renaming update directory");
#elif defined(__APPLE__)
    if (::renameatx_np(impl_->leaf(), source.c_str(), impl_->leaf(), destination.c_str(),
                       RENAME_EXCL) != 0)
        fail("Renaming update directory");
#else
    fail("Renaming update directory", ENOTSUP);
#endif
    flush(impl_->leaf(), "Flushing update directory");
}

std::string GuardedDirectory::read_file(const fs::path& name, std::size_t limit) const {
    check_leaf(name);
    Fd handle = open_leaf(impl_->leaf(), name);
    if (handle.get() < 0)
        fail("Opening update file");
    check_file(handle.get());
    struct stat info {};
    if (::fstat(handle.get(), &info) != 0)
        fail("Reading update file size");
    if (info.st_size < 0 || static_cast<unsigned long long>(info.st_size) > limit)
        throw std::runtime_error("The update file is too large.");
    std::string result(static_cast<std::size_t>(info.st_size), '\0');
    std::size_t done = 0;
    while (done < result.size()) {
        const auto count = ::read(handle.get(), result.data() + done, result.size() - done);
        if (count < 0) {
            if (errno == EINTR)
                continue;
            fail("Reading update file");
        }
        if (count == 0)
            break;
        done += static_cast<std::size_t>(count);
    }
    char extra = 0;
    if (done != result.size() || ::read(handle.get(), &extra, 1) != 0)
        throw std::runtime_error("The update file changed while being read.");
    return result;
}

std::unique_ptr<GuardedDirectory::FilePin> GuardedDirectory::pin_file(const fs::path& name,
                                                                      bool) const {
    check_leaf(name);
    Fd handle = open_leaf(impl_->leaf(), name);
    if (handle.get() < 0) {
        if (errno == ENOENT)
            return {};
        fail("Pinning update file");
    }
    check_file(handle.get());
    return std::unique_ptr<FilePin>(
        new FilePin(std::make_unique<FilePin::Impl>(std::move(handle))));
}

void GuardedDirectory::write_file_atomic(const fs::path& name, std::string_view bytes) const {
    // Validate an existing entry before replacing it; the rename never follows the destination.
    (void)pin_file(name);
    publish(impl_->leaf(), name, 0644,
            [&](int file) { write_all(file, bytes.data(), bytes.size()); });
}

void GuardedDirectory::copy_file_atomic_from(const FilePin& source,
                                             const fs::path& destination) const {
    check_leaf(destination);
    struct stat info {};
    if (::fstat(source.impl_->handle.get(), &info) != 0)
        fail("Inspecting pinned update file");
    publish(impl_->leaf(), destination, info.st_mode & 0777, [&](int file) {
        std::array<char, 65536> buffer{};
        off_t offset = 0;
        for (;;) {
            const auto count =
                ::pread(source.impl_->handle.get(), buffer.data(), buffer.size(), offset);
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                fail("Reading pinned update file");
            }
            if (count == 0)
                break;
            write_all(file, buffer.data(), static_cast<std::size_t>(count));
            offset += count;
        }
    });
}

void GuardedDirectory::remove_file(const fs::path& name, std::string_view expected_sha256) const {
    check_leaf(name);
    Fd file = open_leaf(impl_->leaf(), name);
    if (file.get() < 0) {
        if (errno == ENOENT)
            return;
        fail("Opening update file for removal");
    }
    check_file(file.get());
    if (!expected_sha256.empty()) {
        Sha256 digest;
        std::array<char, 65536> buffer{};
        for (;;) {
            const auto count = ::read(file.get(), buffer.data(), buffer.size());
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                fail("Checking update file before removal");
            }
            if (count == 0)
                break;
            digest.update(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
        }
        if (digest.hexdigest() != expected_sha256)
            throw std::runtime_error("An update file changed before cleanup.");
    }
    if (::unlinkat(impl_->leaf(), name.c_str(), 0) != 0)
        fail("Removing update file");
}

void GuardedDirectory::remove_empty_directory(const fs::path& name) const {
    check_leaf(name);
    (void)open_directory(impl_->leaf(), name.c_str());
    if (::unlinkat(impl_->leaf(), name.c_str(), AT_REMOVEDIR) != 0)
        fail("Removing empty update directory");
}
} // namespace fv::update_native
