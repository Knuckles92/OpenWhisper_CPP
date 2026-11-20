#include "ui_shell/system_tray.h"

#include <utility>

#include <QAction>
#include <QMenu>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "appkit/registries.h"
#include "base/identity.h"
#include "base/log.h"
#include "ui_shell/main_window.h"
#include "ui_theme/app_icon.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.system_tray"};

QString productName() {
    return qt::toQString(identity().product);
}

} // namespace

SystemTray::SystemTray(MainWindow& window, QObject* parent)
    : SystemTray(window, TrayEnvironment{}, parent) {}

SystemTray::SystemTray(MainWindow& window, TrayEnvironment environment, QObject* parent)
    : QSystemTrayIcon(parent), window_(window), menu_(new QMenu) {
    available_ = environment.available && environment.available();

    setIcon(ui::appIcon());
    setToolTip(productName());

    window_.context().menus().populate(Menu::Tray, menu_);
    connectAction(tray_id::show, [this] { window_.restoreFromTray(); });
    connectAction(tray_id::hide, [this] { window_.hide(); });
    connectAction(tray_id::toggleRecording, [this] { Q_EMIT toggleRecording(); });
    connectAction(tray_id::copyOriginal, [this] { Q_EMIT copyLastOriginalRequested(); });
    connectAction(tray_id::walkthrough, [this] { Q_EMIT walkthroughRequested(); });
    connectAction(tray_id::screenshot, [this] { Q_EMIT screenshotRequested(); });
    connectAction(tray_id::meeting, [this] { Q_EMIT meetingToggleRequested(); });
    connectAction(tray_id::meetingDashboard, [this] { Q_EMIT meetingDashboardRequested(); });
    connectAction(tray_id::scratchpad, [this] { Q_EMIT scratchpadRequested(); });
    connectAction(tray_id::stats, [this] { Q_EMIT statsRequested(); });
    connectAction(tray_id::settings, [this] { window_.openSettings(); });
    connectAction(tray_id::update, [this] { Q_EMIT updateRequested(); });
    connectAction(tray_id::exit, [this] { Q_EMIT exitRequested(); });
    if (QMenu* language = languageMenu()) {
        connect(language, &QMenu::aboutToShow, this,
                [this, language] { Q_EMIT languageMenuRequested(language); });
    }
    // Attached on every platform FioriVoce builds for. macOS, where an attached menu swallows
    // the left click that should restore the window, is not one of them.
    setContextMenu(menu_);

    connect(this, &QSystemTrayIcon::activated, this, &SystemTray::onActivated);

    if (available_) {
        if (environment.showIcon) {
            environment.showIcon(*this);
        }
        kLog.info("System tray initialized");
    } else {
        kLog.warning("No system tray is available; close-to-tray and tray controls are disabled");
    }
}

SystemTray::~SystemTray() {
    setContextMenu(nullptr);
    delete menu_;
}

QAction* SystemTray::action(const char* id) const {
    return window_.context().menus().action(id);
}

void SystemTray::connectAction(const char* id, std::function<void()> slot) {
    if (QAction* found = action(id)) {
        connect(found, &QAction::triggered, this, [slot = std::move(slot)] { slot(); });
    }
}

QMenu* SystemTray::languageMenu() const {
    QAction* language = action(tray_id::language);
    return language != nullptr ? language->menu<QMenu*>() : nullptr;
}

void SystemTray::onActivated(QSystemTrayIcon::ActivationReason reason) {
    if (reason == QSystemTrayIcon::DoubleClick) {
        window_.restoreFromTray();
    }
}

void SystemTray::setRecording(bool recording) {
    if (QAction* toggle = action(tray_id::toggleRecording)) {
        toggle->setText(recording ? QStringLiteral("Stop Recording")
                                  : QStringLiteral("Start Recording"));
    }
}

void SystemTray::setWalkthroughActive(bool active) {
    if (QAction* walkthrough = action(tray_id::walkthrough)) {
        walkthrough->setText(active ? QStringLiteral("Finish Walkthrough")
                                    : QStringLiteral("Start Walkthrough"));
    }
}

void SystemTray::setUpdateAvailable(const QString& version) {
    if (QAction* update = action(tray_id::update)) {
        update->setText(QStringLiteral("Update to %1…").arg(version));
        update->setVisible(!version.isEmpty());
    }
    setToolTip(version.isEmpty()
                   ? productName()
                   : QStringLiteral("%1: update %2 available").arg(productName(), version));
}

void SystemTray::setMeetingActive(bool active, std::optional<bool> dashboard) {
    meetingActive_ = active;
    if (QAction* meeting = action(tray_id::meeting)) {
        meeting->setText(meetingActive_ ? QStringLiteral("End Meeting")
                                        : QStringLiteral("Start Meeting"));
    }
    if (QAction* open = action(tray_id::meetingDashboard)) {
        open->setEnabled(dashboard.value_or(meetingActive_));
    }
}

} // namespace fv
