#include "ui_shell/single_instance.h"

#include <unistd.h>

#include <string>

namespace fv {

// services/ui_instance.runtime_paths names both the lock folder and the socket by os.getuid().
std::string instanceUserPart() {
    return std::to_string(getuid());
}

} // namespace fv
