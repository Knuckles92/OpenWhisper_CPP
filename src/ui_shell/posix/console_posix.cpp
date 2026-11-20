#include "ui_shell/early_cli.h"

#include <fcntl.h>
#include <unistd.h>

namespace fv {

// A process started from a terminal shares it already.
void attachParentConsole() {}

bool hasConsoleOutput() {
    return fcntl(STDOUT_FILENO, F_GETFD) != -1;
}

} // namespace fv
