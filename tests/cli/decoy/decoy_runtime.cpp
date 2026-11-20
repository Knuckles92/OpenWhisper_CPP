// A stand-in for the NeMo-Speech.cpp library that does nothing but leave a file behind when a
// process loads it: the path in FV_CLI_DECOY_MARKER. A test puts it where find_runtime_library
// looks and learns from the marker whether the process under test loaded a runtime. It exports
// nothing, so a real load attempt fails afterwards, on the first missing function.

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        wchar_t path[2048];
        const DWORD length = GetEnvironmentVariableW(L"FV_CLI_DECOY_MARKER", path, 2048);
        if (length > 0 && length < 2048) {
            const HANDLE marker = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                              FILE_ATTRIBUTE_NORMAL, nullptr);
            if (marker != INVALID_HANDLE_VALUE) {
                CloseHandle(marker);
            }
        }
    }
    return TRUE;
}
#else
#include <cstdio>
#include <cstdlib>

__attribute__((constructor)) static void leave_marker() {
    if (const char* path = std::getenv("FV_CLI_DECOY_MARKER")) {
        if (std::FILE* marker = std::fopen(path, "w")) {
            std::fclose(marker);
        }
    }
}
#endif
