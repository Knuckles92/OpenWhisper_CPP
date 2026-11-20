#include <windows.h>

#include <string>

#include "meeting_web/dashboard_root.h"

namespace fv::meeting_web {

std::filesystem::path executable_dir() {
    // Long paths: grow the buffer until GetModuleFileNameW stops truncating.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) {
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace fv::meeting_web
