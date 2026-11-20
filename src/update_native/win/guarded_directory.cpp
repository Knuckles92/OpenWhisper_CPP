#include "update_native/guarded_directory.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

#include "base/hash.h"
#include "base/ids.h"

namespace fv::update_native {
namespace fs = std::filesystem;
namespace {
class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) {
            if (value_ != INVALID_HANDLE_VALUE)
                CloseHandle(value_);
            value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE);
        }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }

private:
    HANDLE value_;
};

[[noreturn]] void fail(const char* operation, DWORD error = GetLastError()) {
    throw std::system_error(static_cast<int>(error), std::system_category(), operation);
}

void check_directory(HANDLE handle) {
    FILE_ATTRIBUTE_TAG_INFO info{};
    if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &info, sizeof(info)))
        fail("Inspecting update directory");
    if (!(info.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
        (info.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("An update directory is a reparse point or is not a directory.");
}

void check_file(HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (!GetFileInformationByHandle(handle, &info))
        fail("Inspecting update file");
    if ((info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
        info.nNumberOfLinks != 1)
        throw std::runtime_error("An update file is not an ordinary unlinked file.");
}

Handle open_directory(const fs::path& path, DWORD access) {
    Handle handle(CreateFileW(path.c_str(), access, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
        fail("Opening update directory");
    check_directory(handle.get());
    return handle;
}

void check_leaf(const fs::path& name) {
    const auto value = name.native();
    if (value.empty() || value == L"." || value == L".." || value.back() == L'.' ||
        value.back() == L' ' || value.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos ||
        value.find(L'\0') != std::wstring::npos || name.has_root_path() || name.filename() != name)
        throw std::invalid_argument("Expected a single update directory name.");
}
} // namespace

struct GuardedDirectory::Impl {
    fs::path path;
    std::vector<Handle> components;
};
struct GuardedDirectory::FilePin::Impl {
    explicit Impl(Handle value) : handle(std::move(value)) {}
    Handle handle;
};
GuardedDirectory::FilePin::FilePin(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
GuardedDirectory::FilePin::~FilePin() = default;
GuardedDirectory::FilePin::FilePin(FilePin&&) noexcept = default;
GuardedDirectory::FilePin& GuardedDirectory::FilePin::operator=(FilePin&&) noexcept = default;

GuardedDirectory::GuardedDirectory(const fs::path& directory) : impl_(std::make_unique<Impl>()) {
    if (!directory.is_absolute() || directory != directory.lexically_normal())
        throw std::invalid_argument("The update directory must be an absolute normalized path.");
    impl_->path = directory.root_path();
    impl_->components.push_back(open_directory(impl_->path, FILE_READ_ATTRIBUTES));
    for (const auto& component : directory.relative_path()) {
        check_leaf(component);
        impl_->path /= component;
        impl_->components.push_back(open_directory(impl_->path, FILE_READ_ATTRIBUTES));
    }
}
GuardedDirectory::~GuardedDirectory() = default;
GuardedDirectory::GuardedDirectory(GuardedDirectory&&) noexcept = default;
GuardedDirectory& GuardedDirectory::operator=(GuardedDirectory&&) noexcept = default;

void GuardedDirectory::rename_child_exclusive(const fs::path& source,
                                              const fs::path& destination) const {
    check_leaf(source);
    check_leaf(destination);
    auto handle = open_directory(impl_->path / source, DELETE | FILE_READ_ATTRIBUTES);
    const auto name = (impl_->path / destination).native();
    const auto bytes = name.size() * sizeof(wchar_t);
    if (bytes > MAXDWORD)
        throw std::invalid_argument("The update directory name is too long.");
    std::vector<std::byte> storage(std::max(
        sizeof(FILE_RENAME_INFO), offsetof(FILE_RENAME_INFO, FileName) + bytes + sizeof(wchar_t)));
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(bytes);
    std::memcpy(info->FileName, name.data(), bytes);
    if (!SetFileInformationByHandle(handle.get(), FileRenameInfo, info,
                                    static_cast<DWORD>(storage.size())))
        fail("Renaming update directory");
}

std::string GuardedDirectory::read_file(const fs::path& name, std::size_t limit) const {
    check_leaf(name);
    Handle handle(CreateFileW((impl_->path / name).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE)
        fail("Opening update file");
    check_file(handle.get());
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle.get(), &size))
        fail("Reading update file size");
    if (size.QuadPart < 0 || static_cast<unsigned long long>(size.QuadPart) > limit ||
        static_cast<unsigned long long>(size.QuadPart) > MAXDWORD)
        throw std::runtime_error("The update file is too large.");
    std::string result(static_cast<std::size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    if (!ReadFile(handle.get(), result.data(), static_cast<DWORD>(result.size()), &read, nullptr))
        fail("Reading update file");
    if (read != result.size())
        throw std::runtime_error("The update file changed while being read.");
    return result;
}

std::unique_ptr<GuardedDirectory::FilePin> GuardedDirectory::pin_file(const fs::path& name,
                                                                      bool allow_writes) const {
    check_leaf(name);
    Handle handle(CreateFileW((impl_->path / name).c_str(), GENERIC_READ,
                              FILE_SHARE_READ | (allow_writes ? FILE_SHARE_WRITE : 0), nullptr,
                              OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (handle.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return {};
        fail("Pinning update file", error);
    }
    check_file(handle.get());
    return std::unique_ptr<FilePin>(
        new FilePin(std::make_unique<FilePin::Impl>(std::move(handle))));
}

void GuardedDirectory::write_file_atomic(const fs::path& name, std::string_view bytes) const {
    // Validate an existing entry before replacing it. The subsequent handle rename never
    // follows the destination, even if its directory entry changes after this check.
    (void)pin_file(name);
    if (bytes.size() > MAXDWORD)
        throw std::invalid_argument("The update file is too large.");
    const fs::path temporary = fs::path(L".update-") += fs::path(uuid4_hex());
    Handle file(CreateFileW((impl_->path / temporary).c_str(),
                            GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        fail("Creating update temporary file");
    try {
        check_file(file.get());
        DWORD written = 0;
        if (!WriteFile(file.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &written,
                       nullptr) ||
            written != bytes.size())
            fail("Writing update temporary file");
        if (!FlushFileBuffers(file.get()))
            fail("Flushing update temporary file");
        const auto value = (impl_->path / name).native();
        const auto length = value.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(
            std::max(sizeof(FILE_RENAME_INFO),
                     offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t)));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = TRUE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(info->FileName, value.data(), length);
        if (!SetFileInformationByHandle(file.get(), FileRenameInfo, info,
                                        static_cast<DWORD>(storage.size())))
            fail("Publishing update file");
    } catch (...) {
        FILE_DISPOSITION_INFO dispose{TRUE};
        SetFileInformationByHandle(file.get(), FileDispositionInfo, &dispose, sizeof(dispose));
        throw;
    }
}

void GuardedDirectory::copy_file_atomic_from(const FilePin& source,
                                             const fs::path& destination) const {
    check_leaf(destination);
    const fs::path temporary = fs::path(L".update-") += fs::path(uuid4_hex());
    Handle file(CreateFileW((impl_->path / temporary).c_str(),
                            GENERIC_WRITE | DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, nullptr,
                            CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                            nullptr));
    if (file.get() == INVALID_HANDLE_VALUE)
        fail("Creating update temporary file");
    try {
        check_file(file.get());
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(source.impl_->handle.get(), zero, nullptr, FILE_BEGIN))
            fail("Seeking pinned update file");
        std::array<char, 65536> buffer{};
        for (;;) {
            DWORD read = 0;
            if (!ReadFile(source.impl_->handle.get(), buffer.data(),
                          static_cast<DWORD>(buffer.size()), &read, nullptr))
                fail("Reading pinned update file");
            if (!read)
                break;
            DWORD written = 0;
            if (!WriteFile(file.get(), buffer.data(), read, &written, nullptr) || written != read)
                fail("Writing update temporary file");
        }
        if (!FlushFileBuffers(file.get()))
            fail("Flushing update temporary file");
        const auto value = (impl_->path / destination).native();
        const auto length = value.size() * sizeof(wchar_t);
        std::vector<std::byte> storage(
            std::max(sizeof(FILE_RENAME_INFO),
                     offsetof(FILE_RENAME_INFO, FileName) + length + sizeof(wchar_t)));
        auto* info = reinterpret_cast<FILE_RENAME_INFO*>(storage.data());
        info->ReplaceIfExists = TRUE;
        info->RootDirectory = nullptr;
        info->FileNameLength = static_cast<DWORD>(length);
        std::memcpy(info->FileName, value.data(), length);
        if (!SetFileInformationByHandle(file.get(), FileRenameInfo, info,
                                        static_cast<DWORD>(storage.size())))
            fail("Publishing update file");
    } catch (...) {
        FILE_DISPOSITION_INFO dispose{TRUE};
        SetFileInformationByHandle(file.get(), FileDispositionInfo, &dispose, sizeof(dispose));
        throw;
    }
}

void GuardedDirectory::remove_file(const fs::path& name, std::string_view expected_sha256) const {
    check_leaf(name);
    const DWORD access =
        DELETE | FILE_READ_ATTRIBUTES | (expected_sha256.empty() ? 0 : GENERIC_READ);
    const DWORD share = FILE_SHARE_READ | (expected_sha256.empty() ? FILE_SHARE_WRITE : 0);
    Handle file(CreateFileW((impl_->path / name).c_str(), access, share, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (file.get() == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        if (error == ERROR_FILE_NOT_FOUND)
            return;
        fail("Opening update file for removal", error);
    }
    check_file(file.get());
    if (!expected_sha256.empty()) {
        Sha256 digest;
        std::array<char, 65536> buffer{};
        DWORD count = 0;
        do {
            if (!ReadFile(file.get(), buffer.data(), static_cast<DWORD>(buffer.size()), &count,
                          nullptr))
                fail("Checking update file before removal");
            digest.update(std::string_view(buffer.data(), count));
        } while (count != 0);
        if (digest.hexdigest() != expected_sha256)
            throw std::runtime_error("An update file changed before cleanup.");
    }
    FILE_DISPOSITION_INFO dispose{TRUE};
    if (!SetFileInformationByHandle(file.get(), FileDispositionInfo, &dispose, sizeof(dispose)))
        fail("Removing update file");
}

void GuardedDirectory::remove_empty_directory(const fs::path& name) const {
    check_leaf(name);
    Handle directory = open_directory(impl_->path / name, DELETE | FILE_READ_ATTRIBUTES);
    FILE_DISPOSITION_INFO dispose{TRUE};
    if (!SetFileInformationByHandle(directory.get(), FileDispositionInfo, &dispose,
                                    sizeof(dispose)))
        fail("Removing empty update directory");
}
} // namespace fv::update_native
#endif
