#include "cli/detail/console_interrupt.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <condition_variable>
#include <mutex>

namespace fv::cli::detail {
namespace {

std::mutex g_mutex;
std::condition_variable g_signalled;
bool g_interrupted = false;

// Runs on a thread the system creates for the event. Returning TRUE keeps the default handler
// (ExitProcess) from running; for a closing console the process ends once this returns anyway,
// so it gives the server a moment to stop first.
BOOL WINAPI on_console_event(DWORD event) {
    switch (event) {
    case CTRL_C_EVENT:
    case CTRL_BREAK_EVENT:
    case CTRL_CLOSE_EVENT: {
        {
            std::lock_guard lock(g_mutex);
            g_interrupted = true;
        }
        g_signalled.notify_all();
        if (event == CTRL_CLOSE_EVENT) {
            Sleep(2000);
        }
        return TRUE;
    }
    default:
        return FALSE;
    }
}

} // namespace

void prepare_interrupt() {
    SetConsoleCtrlHandler(on_console_event, TRUE);
}

void wait_for_interrupt() {
    std::unique_lock lock(g_mutex);
    g_signalled.wait(lock, [] { return g_interrupted; });
}

} // namespace fv::cli::detail
