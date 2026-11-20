#include "ui_shell/shell_feature.h"

#include <memory>

#include <QAction>
#include <QKeySequence>
#include <QMenu>
#include <QString>
#include <QWidget>

#include "appkit/app_context.h"
#include "appkit/order.h"
#include "appkit/registries.h"
#include "base/log.h"
#include "ui_shell/main_window.h"
#include "ui_shell/system_tray.h"

namespace fv {
namespace {

constexpr log::Logger kControllerLog{"ui_qt.ui_controller"};

// Qt's default role, under which an item named like "Settings" or "Exit" moves or is renamed;
// the items OpenWhisper left at the default keep it.
constexpr QAction::MenuRole kDefaultRole = QAction::TextHeuristicRole;

QAction* addItem(MenuRegistry& menus, const char* id, Menu menu, int order, const char* text,
                 QAction::MenuRole role, const char* shortcut = nullptr) {
    MenuItem item;
    item.id = id;
    item.menu = menu;
    item.order = order;
    item.text = QString::fromUtf8(text);
    item.role = role;
    if (shortcut != nullptr) {
        item.shortcut = QKeySequence(QString::fromLatin1(shortcut));
    }
    return menus.add(item);
}

void registerTitleBarMenus(MenuRegistry& menus) {
    using namespace order;
    addItem(menus, menu_id::settings, Menu::File, file_menu::settings, "Settings", QAction::NoRole);
    addItem(menus, menu_id::guidedSetup, Menu::File, file_menu::guided_setup, "Guided setup…",
            QAction::NoRole);
    addItem(menus, menu_id::downloads, Menu::File, file_menu::downloads, "Downloads...",
            QAction::NoRole);
    addItem(menus, menu_id::hotkeys, Menu::File, file_menu::hotkeys, "Hotkeys", kDefaultRole);
    menus.addSeparator(Menu::File, file_menu::separator_before_tray);
    addItem(menus, menu_id::minimizeToTray, Menu::File, file_menu::minimize_to_tray,
            "Minimize to Tray", kDefaultRole);
    addItem(menus, menu_id::exit, Menu::File, file_menu::exit, "Exit", QAction::NoRole,
            MainWindow::kQuitShortcut);

    addItem(menus, menu_id::sidePanel, Menu::View, view_menu::side_panel, "History", kDefaultRole,
            MainWindow::kHistoryShortcut);
    addItem(menus, menu_id::historyCalendar, Menu::View, view_menu::history_calendar,
            "History Calendar", kDefaultRole, MainWindow::kHistoryCalendarShortcut);
    addItem(menus, menu_id::compactMode, Menu::View, view_menu::compact_mode, "Compact Mode",
            kDefaultRole, MainWindow::kCompactShortcut);
    QAction* host = addItem(menus, menu_id::hostMode, Menu::View, view_menu::host_mode, "Host Mode",
                            kDefaultRole, MainWindow::kHostShortcut);
    host->setCheckable(true);
    host->setStatusTip(QStringLiteral("Show what this computer serves to paired computers "
                                      "instead of the recording tabs"));
    menus.addSeparator(Menu::View, view_menu::separator_before_windows);
    addItem(menus, menu_id::scratchpad, Menu::View, view_menu::scratchpad, "Scratchpad",
            kDefaultRole);
    addItem(menus, menu_id::stats, Menu::View, view_menu::stats, "Stats", kDefaultRole);
    menus.addSeparator(Menu::View, view_menu::separator_before_dashboard);
    addItem(menus, menu_id::meetingDashboard, Menu::View, view_menu::open_meeting_dashboard,
            "Open Meeting Dashboard", kDefaultRole);

    // NoRole: macOS's application-menu merge is not a platform OpenWhisper_CPP builds for.
    addItem(menus, menu_id::checkForUpdates, Menu::Help, help_menu::check_for_updates,
            "Check for Updates...", QAction::NoRole);
    addItem(menus, menu_id::about, Menu::Help, help_menu::about, "About", QAction::NoRole);
}

void registerTrayMenu(MenuRegistry& menus) {
    using namespace order;
    addItem(menus, tray_id::show, Menu::Tray, tray::show, "Show", kDefaultRole);
    addItem(menus, tray_id::hide, Menu::Tray, tray::hide, "Hide", kDefaultRole);
    menus.addSeparator(Menu::Tray, tray::separator_before_recording);
    addItem(menus, tray_id::toggleRecording, Menu::Tray, tray::toggle_recording, "Start Recording",
            kDefaultRole);
    addItem(menus, tray_id::copyOriginal, Menu::Tray, tray::copy_original,
            "Copy original of last dictation", kDefaultRole);
    addItem(menus, tray_id::walkthrough, Menu::Tray, tray::walkthrough, "Start Walkthrough",
            kDefaultRole);
    addItem(menus, tray_id::screenshot, Menu::Tray, tray::take_screenshot, "Take Screenshot",
            kDefaultRole);
    // A submenu, built with the tray menu that holds it; the language feature refills it as it
    // opens (SystemTray::languageMenuRequested).
    menus.addItem(MenuItemSpec{Menu::Tray, tray::language,
                               [](QObject* parent) -> QAction* {
                                   auto* language = new QMenu(QStringLiteral("Language"),
                                                              qobject_cast<QWidget*>(parent));
                                   return language->menuAction();
                               },
                               tray_id::language});
    menus.addSeparator(Menu::Tray, tray::separator_before_meeting);
    addItem(menus, tray_id::meeting, Menu::Tray, tray::meeting, "Start Meeting", kDefaultRole);
    QAction* dashboard =
        addItem(menus, tray_id::meetingDashboard, Menu::Tray, tray::open_meeting_dashboard,
                "Open Meeting Dashboard", kDefaultRole);
    dashboard->setEnabled(false);
    menus.addSeparator(Menu::Tray, tray::separator_before_windows);
    addItem(menus, tray_id::scratchpad, Menu::Tray, tray::scratchpad, "Scratchpad", kDefaultRole);
    addItem(menus, tray_id::stats, Menu::Tray, tray::stats, "Stats", kDefaultRole);
    addItem(menus, tray_id::settings, Menu::Tray, tray::settings, "Settings", QAction::NoRole);
    menus.addSeparator(Menu::Tray, tray::separator_before_exit);
    // Shown only while a newer release is waiting (SystemTray::setUpdateAvailable).
    QAction* update =
        addItem(menus, tray_id::update, Menu::Tray, tray::update, "", QAction::NoRole);
    update->setVisible(false);
    addItem(menus, tray_id::exit, Menu::Tray, tray::exit, "Exit", QAction::NoRole);
}

} // namespace

void ShellFeature::provide(AppContext& context) {
    context.provide<MainWindow>(std::make_shared<MainWindow>(context, MainWindow::DeferSetup{}));
}

void ShellFeature::install(AppContext& context) {
    registerTitleBarMenus(context.menus());
    registerTrayMenu(context.menus());
    // An empty page stands in for the Upload File tab when the record feature, which installs
    // before the shell (by id), did not register it.
    if (!context.screens().isDefined(kUploadScreen)) {
        context.screens().define(kUploadScreen, order::tab::upload_file,
                                 QStringLiteral("Upload File"),
                                 [](QWidget* parent) -> QWidget* { return new QWidget(parent); });
    }
}

void connectShell(MainWindow& window, SystemTray& tray) {
    QObject::connect(&tray, &SystemTray::exitRequested, &window, [&window] {
        kControllerLog.info("Exit requested from tray");
        // Through the window: a bare QApplication::quit() would be vetoed by the close-to-tray
        // rule, which only hid the window again.
        window.requestQuit();
    });
}

} // namespace fv
