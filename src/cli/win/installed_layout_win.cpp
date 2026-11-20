#include "cli/installed_check.h"

#include <algorithm>
#include <iterator>
#include <stdexcept>
#include <string>
#include <system_error>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace fv::cli {
namespace {

// The ones fiorivoce.exe cannot start, take its look or draw its icons without, and the TLS
// backend Qt Network loads at run time, which no import table names (a34 section 6.2 and M3.11's
// traps ask for it by name); windeployqt picks the rest.
constexpr const char* kQtPlugins[] = {
    "platforms/qwindows.dll", "platforms/qoffscreen.dll", "styles/qmodernwindowsstyle.dll",
    "imageformats/qsvg.dll",  "iconengines/qsvgicon.dll", "tls/qschannelbackend.dll",
};
constexpr const char* kQtLibraries[] = {
    "Qt6Core.dll", "Qt6Gui.dll", "Qt6Widgets.dll", "Qt6Svg.dll", "Qt6Network.dll",
};
// What every x64 program built with MSVC's C++ runtime imports. The setup does not install the
// Visual C++ Redistributable, so these ship beside the programs.
constexpr const char* kCppRuntime[] = {"vcruntime140.dll", "vcruntime140_1.dll", "msvcp140.dll"};

// The libraries of Windows itself that the programs, Qt, its plugins and the vcpkg libraries
// import (dumpbin /dependents over a staged install and the build folder, November 2026), with
// a margin of others Qt's modules are known to use, and the Visual C++ runtime that its
// redistributable puts in System32 (an install holds its own copy all the same:
// local_only_prefixes). Sorted, lower case. Only these count as the system's: a library of any
// other name in System32, such as a zlib or OpenSSL DLL a third-party installer put there, must
// still be beside the programs. A Qt update that imports a library missing here makes the
// installed self-test report it, and it is added here.
constexpr std::string_view kWindowsLibraries[] = {
    "advapi32.dll",
    "authz.dll",
    "avrt.dll",
    "bcrypt.dll",
    "cfgmgr32.dll",
    "combase.dll",
    "comctl32.dll",
    "comdlg32.dll",
    "concrt140.dll",
    "credui.dll",
    "crypt32.dll",
    "d2d1.dll",
    "d3d11.dll",
    "d3d12.dll",
    "d3d9.dll",
    "dbghelp.dll",
    "dcomp.dll",
    "dnsapi.dll",
    "dwmapi.dll",
    "dwrite.dll",
    "dxcore.dll",
    "dxgi.dll",
    "gdi32.dll",
    "glu32.dll",
    "hid.dll",
    "icu.dll",
    "icuin.dll",
    "icuuc.dll",
    "imagehlp.dll",
    "imm32.dll",
    "iphlpapi.dll",
    "kernel32.dll",
    "kernelbase.dll",
    "ksuser.dll",
    "mpr.dll",
    "msimg32.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll",
    "mswsock.dll",
    "ncrypt.dll",
    "netapi32.dll",
    "normaliz.dll",
    "ntdll.dll",
    "ole32.dll",
    "oleacc.dll",
    "oleaut32.dll",
    "opengl32.dll",
    "powrprof.dll",
    "propsys.dll",
    "psapi.dll",
    "rpcrt4.dll",
    "secur32.dll",
    "setupapi.dll",
    "shcore.dll",
    "shell32.dll",
    "shlwapi.dll",
    "sspicli.dll",
    "ucrtbase.dll",
    "uiautomationcore.dll",
    "user32.dll",
    "userenv.dll",
    "uxtheme.dll",
    "vccorlib140.dll",
    "vcomp140.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "vcruntime140_threads.dll",
    "version.dll",
    "windowscodecs.dll",
    "winhttp.dll",
    "wininet.dll",
    "winmm.dll",
    "winspool.drv",
    "wintrust.dll",
    "wldap32.dll",
    "ws2_32.dll",
    "wtsapi32.dll",
};
static_assert(std::ranges::is_sorted(kWindowsLibraries), "binary_search needs the list sorted");

std::wstring system_folder() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const UINT length = GetSystemDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return buffer;
        }
        buffer.resize(length);
    }
}

} // namespace

InstalledLayout installed_layout() {
    InstalledLayout layout;
    layout.required = {"fiorivoce.exe", "fiorivoce-cli.exe", "fiorivoce-updater.exe",
                       "webui/dist/index.html"};
    for (const char* plugin : kQtPlugins) {
        layout.required.emplace_back(plugin);
    }
    for (const char* library : kQtLibraries) {
        layout.required.emplace_back(library);
    }
    for (const char* library : kCppRuntime) {
        layout.required.emplace_back(library);
    }
    layout.required.emplace_back("THIRD_PARTY_NOTICES.md");

    layout.import_roots = {"fiorivoce.exe", "fiorivoce-cli.exe", "fiorivoce-updater.exe"};
    for (const char* plugin : kQtPlugins) {
        layout.import_roots.emplace_back(plugin);
    }
    // The rest of the Visual C++ runtime folder the stage copies (msvcp140_1.dll, concrt140.dll,
    // vcruntime140_threads.dll, ...): a program that imports one must find it beside it too.
    layout.local_only_prefixes = {"vcruntime140", "msvcp140", "concrt140", "vccorlib140"};
    return layout;
}

bool is_system_library_in(std::string_view name, const std::filesystem::path& system_folder) {
    std::string folded(name);
    for (char& c : folded) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    // API sets resolve inside the loader; they are not files.
    if (folded.starts_with("api-ms-win-") || folded.starts_with("ext-ms-")) {
        return true;
    }
    if (system_folder.empty() || !std::ranges::binary_search(kWindowsLibraries, folded)) {
        return false;
    }
    const std::filesystem::path file =
        system_folder / std::filesystem::path(std::u8string(
                            reinterpret_cast<const char8_t*>(name.data()), name.size()));
    std::error_code error;
    return std::filesystem::is_regular_file(file, error);
}

bool is_system_library(std::string_view name) {
    return is_system_library_in(name, std::filesystem::path(system_folder()));
}

std::filesystem::path executable_folder() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length =
            GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::runtime_error("The executable's folder could not be found.");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
}

} // namespace fv::cli
