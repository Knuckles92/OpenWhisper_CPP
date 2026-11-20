// Compiled into every test executable by fv_test(). A failed assertion or an abort() in a Debug
// build must end the test with its message and call stack in the log. Left alone, the Debug C
// runtime shows a dialog instead ("abort() has been called") and the test hangs until someone
// clicks it, on the screen of an owner who is working on this machine.

namespace fv::testkit {
// So that this file is not an empty translation unit off Windows.
extern const int no_dialogs_guard;
const int no_dialogs_guard = 1;
} // namespace fv::testkit

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <crtdbg.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <stacktrace>
#include <string>

namespace {

// Not async-signal-safe, and it need not be: abort() raises SIGABRT on the thread that failed,
// and the process ends here.
void on_abort(int) {
    const std::string trace = std::to_string(std::stacktrace::current());
    std::fputs("fv_test: abort() was called. Call stack:\n", stderr);
    std::fputs(trace.c_str(), stderr);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    std::_Exit(3);
}

struct NoDialogs {
    NoDialogs() {
        SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX | SEM_NOOPENFILEERRORBOX);
        // Without this the Debug runtime reports abort() through a dialog before it raises
        // SIGABRT.
        _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
        _set_error_mode(_OUT_TO_STDERR);
#ifdef _DEBUG
        for (const int type : {_CRT_WARN, _CRT_ERROR, _CRT_ASSERT}) {
            _CrtSetReportMode(type, _CRTDBG_MODE_FILE | _CRTDBG_MODE_DEBUG);
            _CrtSetReportFile(type, _CRTDBG_FILE_STDERR);
        }
#endif
        std::signal(SIGABRT, on_abort);
    }
};

const NoDialogs no_dialogs;

} // namespace

#endif // _WIN32
