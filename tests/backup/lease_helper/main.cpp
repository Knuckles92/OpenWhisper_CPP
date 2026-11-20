// Holds a shared data lease on argv[1] until standard input closes, as the reader process of
// OpenWhisper's tests/test_backup_startup.py does. Prints READY once the lease is held. With
// "legacy" as argv[2] the lease is OpenWhisper's (legacy_identity()), as a running OpenWhisper
// holds it. On Windows the arguments are read as UTF-16 (wmain): the narrow argv holds the ANSI
// code page, which cannot spell every folder name.

#include <exception>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

#include "base/data_lease.h"
#include "base/identity.h"
#include "base/utf8.h"

namespace {

int run(const std::filesystem::path& root, bool legacy) {
    try {
        auto lease = fv::DataLease::acquire(root, legacy ? fv::legacy_identity() : fv::identity(),
                                            fv::LeaseMode::Shared);
        std::cout << "READY" << std::endl;
        std::string line;
        while (std::getline(std::cin, line)) {
        }
        lease.release();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

constexpr const char* kUsage = "usage: fv_backup_lease_helper <data root> [legacy]\n";

} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        std::cerr << kUsage;
        return 2;
    }
    return run(std::filesystem::path(argv[1]), argc > 2 && std::wstring_view(argv[2]) == L"legacy");
}
#else
int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << kUsage;
        return 2;
    }
    return run(fv::path_from_utf8(argv[1]), argc > 2 && std::string_view(argv[2]) == "legacy");
}
#endif
