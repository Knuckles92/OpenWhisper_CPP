#include "process/detached.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cwchar>
#include <optional>

#include "base/utf8.h"

namespace fv {

namespace {

// Clears the DLL search path set with SetDllDirectoryW for one launch and puts it back after.
class IndependentDllSearch {
public:
    IndependentDllSearch() {
        const DWORD size = GetDllDirectoryW(0, nullptr);
        if (size > 0) {
            std::wstring buffer(size + 1, L'\0');
            const DWORD length = GetDllDirectoryW(static_cast<DWORD>(buffer.size()), buffer.data());
            buffer.resize(length);
            if (!buffer.empty()) {
                previous_ = std::move(buffer);
            }
        }
        ok_ = SetDllDirectoryW(nullptr) != FALSE;
    }
    ~IndependentDllSearch() {
        if (ok_) {
            SetDllDirectoryW(previous_ ? previous_->c_str() : nullptr);
        }
    }
    IndependentDllSearch(const IndependentDllSearch&) = delete;
    IndependentDllSearch& operator=(const IndependentDllSearch&) = delete;

    bool ok() const { return ok_; }

private:
    std::optional<std::wstring> previous_;
    bool ok_ = false;
};

} // namespace

std::expected<std::uint32_t, std::string> start_detached(const std::filesystem::path& program,
                                                         std::span<const std::string> args,
                                                         const std::filesystem::path& cwd) {
    std::vector<std::string> argv;
    argv.reserve(args.size() + 1);
    argv.push_back(path_to_utf8(program));
    argv.insert(argv.end(), args.begin(), args.end());
    std::wstring command = path_from_utf8(list2cmdline(argv)).wstring();
    const std::wstring directory = (cwd.empty() ? program.parent_path() : cwd).wstring();
    const std::string failure = "Could not start " + path_to_utf8(program.filename()) + ".";

    IndependentDllSearch dll_search;
    if (!dll_search.ok()) {
        return std::unexpected(std::string("Could not prepare an independent update process."));
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const DWORD flags = DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW;
    const std::wstring application = program.wstring();
    if (!CreateProcessW(application.c_str(), command.data(), nullptr, nullptr, FALSE, flags,
                        nullptr, directory.empty() ? nullptr : directory.c_str(), &startup,
                        &process)) {
        return std::unexpected(failure);
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<std::uint32_t>(process.dwProcessId);
}

} // namespace fv
