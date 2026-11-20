#include "update_native/detail/health_child.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <stdexcept>
#include <system_error>
#include <windows.h>

#include "base/utf8.h"

namespace fv::update_native::detail {
namespace {
class WindowsHealthChild final : public HealthChild {
public:
    explicit WindowsHealthChild(HANDLE process) : process_(process) {}
    ~WindowsHealthChild() override { CloseHandle(process_); }
    WindowsHealthChild(const WindowsHealthChild&) = delete;
    WindowsHealthChild& operator=(const WindowsHealthChild&) = delete;

    bool wait(std::chrono::milliseconds timeout) override {
        const DWORD result = WaitForSingleObject(process_, static_cast<DWORD>(timeout.count()));
        if (result == WAIT_OBJECT_0)
            return true;
        if (result != WAIT_TIMEOUT)
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Waiting for updated application");
        return false;
    }

    void terminate() override {
        if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT && !TerminateProcess(process_, 1))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Stopping failed updated application");
        if (WaitForSingleObject(process_, 15000) != WAIT_OBJECT_0)
            throw std::runtime_error("The failed updated application did not exit.");
    }

private:
    HANDLE process_;
};
} // namespace

std::unique_ptr<HealthChild> HealthChild::launch(const std::filesystem::path& executable,
                                                 const std::vector<std::string>& arguments,
                                                 const std::filesystem::path& directory) {
    std::wstring command = L"\"" + executable.native() + L"\"";
    for (const auto& argument : arguments)
        command += L" " + path_from_utf8(argument).native();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        directory.c_str(), &startup, &process))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Launching updated application");
    CloseHandle(process.hThread);
    return std::make_unique<WindowsHealthChild>(process.hProcess);
}
} // namespace fv::update_native::detail
