#include "ui_settings/settings_feature.h"

#include <exception>
#include <memory>

#include <QObject>
#include <QString>

#include "appkit/app_context.h"
#include "base/log.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/fast_judgments_page.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_backup.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_window.h"
#include "ui_settings/walkthroughs_page.h"
#include "ui_shell/main_window.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.ui_controller"};

// A request's slot: nothing may leave it into Qt's event loop (architecture 4 rule 10), so what
// opening throws (a page's create, say) is logged and the request dropped.
template <class Fn> void opening(Fn&& fn) {
    try {
        fn();
    } catch (const std::exception& error) {
        kLog.error_with(error, "Couldn't open Settings");
    } catch (...) {
        kLog.error("Couldn't open Settings");
    }
}

} // namespace

void SettingsFeature::provide(AppContext& context) {
    // The context releases its services newest first: the window, whose pages hold the hooks
    // and the signals AppPages owns, goes before them.
    auto pages = std::make_shared<AppPages>(context);
    context.provide<SettingsPagesSignals>(pages->sharedSignals());
    pages->registerPages();
    registerBackupPage(context);
    registerFastJudgmentsPage(context);
    registerWalkthroughsPage(context);
    context.provide<AppPages>(std::move(pages));
    registerHomePage(context);
    context.provide<SettingsWindow>(std::make_shared<SettingsWindow>(context));
}

void SettingsFeature::install(AppContext& context) {
    SettingsWindow& settings = context.service<SettingsWindow>();
    MainWindow& window = context.service<MainWindow>();
    settings.setParentWindow(&window);
    SettingsWindow* target = &settings;
    SettingsPagesSignals* pageSignals = &context.service<SettingsPagesSignals>();
    AppContext* app = &context;
    QObject::connect(&settings, &SettingsWindow::dialogCreated, pageSignals,
                     [pageSignals, app](SettingsDialog* dialog) {
                         followBackupBusy(*app, *dialog);
                         QObject::connect(dialog, &SettingsDialog::hotkeyCaptureSuspended,
                                          pageSignals,
                                          &SettingsPagesSignals::hotkeyCaptureSuspended);
                     });
    QObject::connect(&window, &MainWindow::settingsRequested, target,
                     [target] { opening([target] { target->open(); }); });
    QObject::connect(
        &window, &MainWindow::settingsDestinationRequested, target,
        [target](const QString& destination) { opening([&] { target->open(destination); }); });
    QObject::connect(&window, &MainWindow::hotkeysRequested, target,
                     [target] { opening([target] { target->openHotkeys(); }); });
    QObject::connect(
        &window, &MainWindow::engineHelpRequested, target,
        [target](const QString& topic) { opening([&] { target->openEngineHelp(topic); }); });
}

} // namespace fv
