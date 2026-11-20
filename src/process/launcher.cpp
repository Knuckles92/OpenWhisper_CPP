#include "process/launcher.h"

#include "process/detached.h"

namespace fv {

std::string windows_command_line(const std::vector<std::string>& argv) {
    return list2cmdline(argv);
}

} // namespace fv
