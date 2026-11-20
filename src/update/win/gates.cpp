#include "update/gates.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <thread>

#include "base/identity.h"
#include "base/utf8.h"
#include "pyport/text.h"
#include "update/apply_error.h"

namespace fv::update {

namespace {

std::wstring wide(std::string_view text) {
    return path_from_utf8(text).wstring();
}

class WindowsMutexApi final : public MutexApi {
public:
    std::optional<MutexHandle> create(std::string_view name) override {
        const HANDLE handle = CreateMutexW(nullptr, TRUE, wide(name).c_str());
        if (handle == nullptr) {
            return std::nullopt;
        }
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(handle);
            return std::nullopt;
        }
        return reinterpret_cast<MutexHandle>(handle);
    }

    bool exists(std::string_view name) override {
        const HANDLE handle = OpenMutexW(SYNCHRONIZE, FALSE, wide(name).c_str());
        if (handle == nullptr) {
            return false;
        }
        CloseHandle(handle);
        return true;
    }

    void release(MutexHandle handle) override {
        if (handle == 0) {
            return;
        }
        const auto native = reinterpret_cast<HANDLE>(handle);
        ReleaseMutex(native);
        CloseHandle(native);
    }

    void wait(std::chrono::milliseconds delay) override { std::this_thread::sleep_for(delay); }
};

} // namespace

MutexApi& system_mutex_api() {
    static WindowsMutexApi api;
    return api;
}

void native_message_box(std::string_view message) {
    const auto text = py::slice(message, std::nullopt, static_cast<std::int64_t>(kApplyErrorLimit));
    MessageBoxW(nullptr, wide(text).c_str(), wide(identity().product).c_str(), MB_ICONERROR);
}

} // namespace fv::update
