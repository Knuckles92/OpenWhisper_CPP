#include "agentapi/detail/host_name.h"

#include <string>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fv::agentapi::detail {

std::string this_host_name() {
    DWORD size = 0;
    GetComputerNameExW(ComputerNamePhysicalDnsHostname, nullptr, &size);
    if (size == 0) {
        return {};
    }
    std::wstring name(size, L'\0');
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, name.data(), &size)) {
        return {};
    }
    name.resize(size);
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()),
                                          nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0) {
        WideCharToMultiByte(CP_UTF8, 0, name.data(), static_cast<int>(name.size()), out.data(),
                            bytes, nullptr, nullptr);
    }
    return out;
}

} // namespace fv::agentapi::detail
