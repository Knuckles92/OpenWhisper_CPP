// Constructs a QtUiExecutor on its main thread, then calls assert_not_gui_thread() on the thread
// argv[1] names ("gui" or "worker"). Prints RETURNED when the call returns; in a debug build the
// call on the GUI thread aborts instead.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

#include <QObject>

#include "appkit/qt_bridge.h"

#ifdef _WIN32
#include <crtdbg.h>
#include <windows.h>
#endif

int main(int argc, char** argv) {
#ifdef _WIN32
    // abort() must end the process quietly: no "abort() has been called" box, no crash dialog.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _CrtSetReportMode(_CRT_ERROR, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ERROR, _CRTDBG_FILE_STDERR);
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc < 2 || (std::strcmp(argv[1], "gui") != 0 && std::strcmp(argv[1], "worker") != 0)) {
        std::fprintf(stderr, "usage: fv_appkit_gui_thread_helper gui|worker\n");
        return 2;
    }
    QObject context;
    fv::qt::QtUiExecutor executor(&context);
    if (std::strcmp(argv[1], "gui") == 0) {
        fv::assert_not_gui_thread();
    } else {
        std::thread worker([] { fv::assert_not_gui_thread(); });
        worker.join();
    }
    std::printf("RETURNED\n");
    std::fflush(stdout);
    return 0;
}
