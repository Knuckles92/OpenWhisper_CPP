// Links in the data root and replacing an existing bundle: services/backup.py's _root,
// _safe_path and _owned_tree_files refuse symbolic links and junctions (_is_link), and
// os.replace publishes over an older file of the same name.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <cstring>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winioctl.h>
#endif

#include "backup/backup.h"
#include "backup/create.h"
#include "backup/inspect.h"
#include "backup/testing/sample_data.h"
#include "testkit/testkit.h"

namespace fs = std::filesystem;
using Catch::Matchers::Message;
using fv::backup::BackupError;
using fv::backup::create_backup;
using fv::backup::CreateOptions;
using fv::backup::inspect_backup;
using fv::backup::testing::make_sample_data;
using fv::backup::testing::read_bytes;
using fv::backup::testing::write_bytes;

namespace {

CreateOptions options_for(const fs::path& root) {
    CreateOptions options;
    options.data_root = root;
    options.app_version = "0.1.0";
    return options;
}

// A directory link at `link` to the folder `target`: a junction on Windows (which needs no
// privilege, unlike a symbolic link), a symbolic link elsewhere. False when the file system
// cannot hold one.
bool make_directory_link(const fs::path& link, const fs::path& target) {
#ifdef _WIN32
    std::error_code error;
    if (!fs::create_directory(link, error)) {
        return false;
    }
    const std::wstring print = fs::absolute(target).wstring();
    const std::wstring substitute = L"\\??\\" + print;
    const auto substitute_bytes = static_cast<USHORT>(substitute.size() * sizeof(wchar_t));
    const auto print_bytes = static_cast<USHORT>(print.size() * sizeof(wchar_t));
    // REPARSE_DATA_BUFFER's MountPointReparseBuffer: tag, data length, reserved, then the
    // substitute and print names' offsets and lengths and both names, each NUL-terminated.
    const USHORT data_length =
        static_cast<USHORT>(8 + substitute_bytes + sizeof(wchar_t) + print_bytes + sizeof(wchar_t));
    std::vector<unsigned char> buffer(8 + data_length, 0);
    auto put16 = [&](std::size_t at, USHORT value) {
        std::memcpy(&buffer[at], &value, 2);
    };
    const DWORD tag = IO_REPARSE_TAG_MOUNT_POINT;
    std::memcpy(buffer.data(), &tag, 4);
    put16(4, data_length);
    put16(8, 0);
    put16(10, substitute_bytes);
    put16(12, static_cast<USHORT>(substitute_bytes + sizeof(wchar_t)));
    put16(14, print_bytes);
    std::memcpy(&buffer[16], substitute.c_str(), substitute_bytes);
    std::memcpy(&buffer[16 + substitute_bytes + sizeof(wchar_t)], print.c_str(), print_bytes);
    HANDLE handle = CreateFileW(link.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return false;
    }
    DWORD returned = 0;
    const BOOL made =
        DeviceIoControl(handle, FSCTL_SET_REPARSE_POINT, buffer.data(),
                        static_cast<DWORD>(buffer.size()), nullptr, 0, &returned, nullptr);
    CloseHandle(handle);
    return made != FALSE;
#else
    std::error_code error;
    fs::create_directory_symlink(target, link, error);
    return !error;
#endif
}

// Removes the link itself, never what it points to.
void remove_link(const fs::path& link) {
#ifdef _WIN32
    RemoveDirectoryW(link.c_str());
#else
    std::error_code ignored;
    fs::remove(link, ignored);
#endif
}

} // namespace

TEST_CASE("a linked data root or owned folder is refused", "[backup][platform]") {
    fv::testkit::TempDataRoot temp;
    const fs::path outside = temp.path() / "outside";
    write_bytes(outside / "elsewhere.wav", "RIFFelsewhere");

    SECTION("the data root itself") {
        const fs::path real = temp.path() / "real";
        make_sample_data(real);
        const fs::path root = temp.path() / "linked-root";
        if (!make_directory_link(root, real)) {
            SKIP("the temporary folder's file system cannot hold a directory link");
        }
        CHECK_THROWS_MATCHES(create_backup(temp.path() / "a.fvbackup", options_for(root)),
                             BackupError,
                             Message("The data directory is a symbolic link or junction"));
        remove_link(root);
    }
    SECTION("an owned folder") {
        const fs::path root = temp.path() / "source";
        make_sample_data(root);
        fs::remove_all(root / "captures");
        if (!make_directory_link(root / "captures", outside)) {
            SKIP("the temporary folder's file system cannot hold a directory link");
        }
        CHECK_THROWS_MATCHES(
            create_backup(temp.path() / "b.fvbackup", options_for(root)), BackupError,
            Message("Symbolic links are not supported in backed-up data: captures"));
        remove_link(root / "captures");
    }
    SECTION("a folder below a recording folder") {
        const fs::path root = temp.path() / "source";
        make_sample_data(root);
        if (!make_directory_link(root / "recordings" / "linked", outside)) {
            SKIP("the temporary folder's file system cannot hold a directory link");
        }
        CHECK_THROWS_MATCHES(create_backup(temp.path() / "c.fvbackup", options_for(root)),
                             BackupError,
                             Message("Symbolic links are not supported in backed-up data"));
        remove_link(root / "recordings" / "linked");
    }
    // Nothing was published, and the linked folder's content is untouched.
    CHECK(read_bytes(outside / "elsewhere.wav") == "RIFFelsewhere");
    for (const char* name : {"a.fvbackup", "b.fvbackup", "c.fvbackup"}) {
        CHECK_FALSE(fs::exists(temp.path() / name));
    }
}

TEST_CASE("a new bundle replaces an older file of the same name", "[backup][platform]") {
    // os.replace(temp_archive, destination): an existing destination is overwritten once the
    // new bundle has passed its inspection.
    fv::testkit::TempDataRoot temp;
    const fs::path source = temp.path() / "source";
    make_sample_data(source);
    const fs::path archive = temp.path() / "nightly.fvbackup";
    write_bytes(archive, "an older bundle");
    const auto info = create_backup(archive, options_for(source));
    CHECK(read_bytes(archive) != "an older bundle");
    CHECK(info == inspect_backup(archive));
    // And again over a real bundle.
    const auto again = create_backup(archive, options_for(source));
    CHECK(again == inspect_backup(archive));
}
