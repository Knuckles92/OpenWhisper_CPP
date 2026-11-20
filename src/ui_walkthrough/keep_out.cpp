#include "ui_walkthrough/keep_out.h"

#include "ui_snap/affinity.h"

namespace fv {

bool setKeepOutOfScreenshots(QWidget* window, bool enabled) {
    return ui_snap::keepOutOfScreenshots(window, enabled);
}

} // namespace fv
