#include "clipboard/sequence.h"

#include <QGuiApplication>
#include <QString>

namespace fv::clipboard {

bool foreign_copies_visible() {
    // Wayland's wlr-data-control path (QT_WAYLAND_USE_DATA_CONTROL with Qt 6.10 or later) is not
    // supported, so on Wayland Qt never sees another app's copy while this app is unfocused.
    return !QGuiApplication::platformName().startsWith(QStringLiteral("wayland"));
}

} // namespace fv::clipboard
