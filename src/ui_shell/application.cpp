#include "ui_shell/application.h"

#include <QApplication>
#include <QMainWindow>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_theme/app_icon.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"
#include "ui_theme/tooltip_style.h"

namespace fv {
namespace {

constexpr log::Logger kRoot{"root"};

} // namespace

Application::Application(SettingsStore& settings, int& argc, char** argv) {
    // Before the QApplication exists, so every platform reads the right name from the start.
    const QString product = qt::toQString(identity().product);
    QApplication::setApplicationName(product);
    QApplication::setApplicationDisplayName(product);
    QApplication::setOrganizationName(product);

    app_ = qobject_cast<QApplication*>(QCoreApplication::instance());
    if (app_ == nullptr) {
        owned_ = std::make_unique<QApplication>(argc, argv);
        app_ = owned_.get();
    }

    // Inherited by every window and dialog, and used by the taskbar and alt-tab.
    app_->setWindowIcon(ui::appIcon());

    const auto snapshot = settings.snapshot();
    const QString preference = ui::resolveUiTheme(*snapshot);
    // On the resolved palette, so the loading screen, which paints before any stylesheet is
    // applied, already reads the right colours.
    themeManager_ =
        std::make_unique<ui::ThemeManager>(ui::resolveThemePreference(preference, app_));
    ui::installTooltipStyle(*app_);
    setTheme(preference);
    applyFontScale(static_cast<int>(settings.get(keys::ui_font_scale)));
    ui::followPlatformColorScheme(*app_, *themeManager_);
}

Application::~Application() = default;

bool Application::setTheme(const QString& preference) {
    return ui::applyUiTheme(preference, *app_, *themeManager_);
}

void Application::applyFontScale(int percent) {
    ui::applyUiFontScale(percent, *app_, *themeManager_);
}

int Application::run(QMainWindow* window) {
    if (window != nullptr) {
        window->show();
    }
    kRoot.info("Starting Qt event loop");
    return QApplication::exec();
}

void Application::quit() {
    QApplication::quit();
}

void Application::exit(int code) {
    QApplication::exit(code);
}

} // namespace fv
