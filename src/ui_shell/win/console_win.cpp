#include "ui_shell/early_cli.h"

#include <windows.h>

#include <cstdio>

namespace fv {
namespace {

// A handle the process was started with (a pipe or a file): writing there is what the caller
// redirected to, so the parent's console must not take its place.
bool redirected(DWORD which) {
    const HANDLE handle = GetStdHandle(which);
    return handle != nullptr && handle != INVALID_HANDLE_VALUE &&
           GetFileType(handle) != FILE_TYPE_UNKNOWN;
}

} // namespace

void attachParentConsole() {
    const bool out = redirected(STD_OUTPUT_HANDLE);
    const bool err = redirected(STD_ERROR_HANDLE);
    if (out && err) {
        return;
    }
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) {
        return;
    }
    // The CRT bound stdout and stderr to nothing at start; point the unredirected ones at the
    // console now attached. A failed reopen leaves the stream writing nowhere, as before.
    FILE* stream = nullptr;
    if (!out) {
        (void)freopen_s(&stream, "CONOUT$", "w", stdout);
    }
    if (!err) {
        (void)freopen_s(&stream, "CONOUT$", "w", stderr);
    }
}

bool hasConsoleOutput() {
    return redirected(STD_OUTPUT_HANDLE) || GetConsoleWindow() != nullptr;
}

} // namespace fv
