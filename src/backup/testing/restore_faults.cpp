#include "backup/testing/restore_faults.h"

#include <utility>

namespace fv::backup::testing {

RestoreFaults& restore_faults() {
    static RestoreFaults faults;
    return faults;
}

ScopedRestoreFaults::ScopedRestoreFaults(RestoreFaults faults) {
    restore_faults() = std::move(faults);
}

ScopedRestoreFaults::~ScopedRestoreFaults() {
    restore_faults() = {};
}

} // namespace fv::backup::testing
