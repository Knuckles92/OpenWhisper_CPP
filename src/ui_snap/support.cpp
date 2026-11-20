#include "ui_snap/support.h"

#include <QStandardPaths>

#include "base/desktop_session.h"
#include "base/env.h"
#include "base/identity.h"
#include "ui_snap/desktop.h"

namespace fv::ui_snap {

QString macReason() {
    const QString product = QString::fromUtf8(identity().product);
    return QStringLiteral(u"Snap needs Screen Recording permission. Allow %1 in System Settings "
                          u"› Privacy & Security › Screen & System Audio Recording. "
                          u"Already enabled? Fully quit and reopen %1. If this started after an "
                          u"update and persists, remove %1 from that list, add the updated app "
                          u"from Applications, and reopen it.")
        .arg(product);
}

Platform currentPlatform() {
    Platform platform;
    platform.system = systemPlatform();
    if (platform.system == "win32") {
        return platform;
    }
    const auto signature = system_env().get("HYPRLAND_INSTANCE_SIGNATURE");
    platform.hyprland = signature && !signature->empty() &&
                        !QStandardPaths::findExecutable(QStringLiteral("hyprctl")).isEmpty();
    platform.grim = !QStandardPaths::findExecutable(QStringLiteral("grim")).isEmpty();
    // XWayland makes Qt look like X11, but grabbing its root window shows only other X
    // clients: the session decides, not Qt's platform.
    platform.wayland = qtPlatformName().toLower().startsWith(QStringLiteral("wayland")) ||
                       is_wayland_session(platform.system, system_env());
    return platform;
}

bool usesGrim(const Platform& platform) {
    return platform.system.starts_with("linux") && platform.hyprland;
}

bool usesGrim() {
    return usesGrim(currentPlatform());
}

std::pair<bool, QString> snapSupport(const Platform& platform) {
    if (platform.system == "win32") {
        return {true, QString()};
    }
    if (platform.system == "darwin") {
        if (platform.screenRecording) {
            return {true, QString()};
        }
        return {false, macReason()};
    }
    if (!platform.system.starts_with("linux")) {
        return {false, QString::fromUtf16(kOtherReason)};
    }
    if (platform.hyprland) {
        if (platform.grim) {
            return {true, QString()};
        }
        return {false, QString::fromUtf16(kGrimReason)};
    }
    if (platform.wayland) {
        return {false, QString::fromUtf16(kWaylandReason)};
    }
    return {true, QString()};
}

std::pair<bool, QString> snapSupport() {
    return snapSupport(currentPlatform());
}

void requestAccess() {
    // macOS's Screen Recording request (support.py:70-83) is not ported; elsewhere there is
    // nothing to ask.
}

} // namespace fv::ui_snap
