#pragma once

#include <functional>
#include <optional>

#include <QMetaMethod>
#include <QString>
#include <QSystemTrayIcon>

class QAction;
class QMenu;

namespace fv {

class MainWindow;

// Tray menu item ids, registered by ShellFeature in appkit/order.h's tray order.
namespace tray_id {
inline constexpr char show[] = "tray.show";
inline constexpr char hide[] = "tray.hide";
inline constexpr char toggleRecording[] = "tray.toggle_recording";
inline constexpr char copyOriginal[] = "tray.copy_original";
inline constexpr char walkthrough[] = "tray.walkthrough";
inline constexpr char screenshot[] = "tray.take_screenshot";
inline constexpr char language[] = "tray.language";
inline constexpr char meeting[] = "tray.meeting";
inline constexpr char meetingDashboard[] = "tray.open_meeting_dashboard";
inline constexpr char scratchpad[] = "tray.scratchpad";
inline constexpr char stats[] = "tray.stats";
inline constexpr char settings[] = "tray.settings";
inline constexpr char update[] = "tray.update";
inline constexpr char exit[] = "tray.exit";
} // namespace tray_id

// What the tray asks of the desktop. The defaults ask Qt; tests replace both, so no icon ever
// reaches the notification area of the session that runs them.
struct TrayEnvironment {
    // QSystemTrayIcon::isSystemTrayAvailable(), asked once at construction.
    std::function<bool()> available = [] {
        return QSystemTrayIcon::isSystemTrayAvailable();
    };
    // Puts the icon in the notification area.
    std::function<void(QSystemTrayIcon&)> showIcon = [](QSystemTrayIcon& icon) {
        icon.show();
    };
};

// The system tray icon and its menu (ui_qt/system_tray.py SystemTrayManager). The menu's items
// come from the window's MenuRegistry (Menu::Tray); this class gives them their behaviour:
// Show restores the window, Hide hides it, Settings opens Settings, and the rest are signals for
// the features that own them. A double-click restores the window; a single click does nothing
// (the menu stays attached for the right click). Without a tray the icon is never shown and a
// warning is logged.
// Affinity: UI actor.
class SystemTray : public QSystemTrayIcon {
    Q_OBJECT
public:
    explicit SystemTray(MainWindow& window, QObject* parent = nullptr);
    SystemTray(MainWindow& window, TrayEnvironment environment, QObject* parent = nullptr);
    ~SystemTray() override;

    bool available() const { return available_; }
    QMenu* menu() const { return menu_; }
    QMenu* languageMenu() const;

    void setRecording(bool recording);
    void setWalkthroughActive(bool active);
    // Offers the newer release in the menu and the tooltip; "" withdraws it.
    void setUpdateAvailable(const QString& version);
    // dashboard: whether Open Meeting Dashboard is enabled; by default while a meeting runs.
    void setMeetingActive(bool active, std::optional<bool> dashboard = {});

    // The activation handler (QSystemTrayIcon::activated), public so a test can drive it.
    void onActivated(QSystemTrayIcon::ActivationReason reason);

    // Whether a signal of this tray has a receiver (architecture 8.5's wiring check).
    bool hasReceivers(const QMetaMethod& signal) const { return isSignalConnected(signal); }

Q_SIGNALS:
    void exitRequested();
    void toggleRecording();
    void meetingToggleRequested();
    void meetingDashboardRequested();
    void copyLastOriginalRequested();
    void walkthroughRequested();
    void screenshotRequested();
    void scratchpadRequested();
    void statsRequested();
    void updateRequested();
    // Emitted as the Language submenu opens, so a connected slot can refill it first.
    void languageMenuRequested(QMenu* menu);

private:
    QAction* action(const char* id) const;
    void connectAction(const char* id, std::function<void()> slot);

    MainWindow& window_;
    QMenu* menu_;
    bool available_ = false;
    bool meetingActive_ = false;
};

} // namespace fv
