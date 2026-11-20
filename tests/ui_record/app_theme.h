#pragma once

#include <QApplication>
#include <QString>

#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// The application styled the way OpenWhisper's QtApplication styles it (ThemeManager, theme,
// font scale), for the duration of one test case.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() { qApp->setStyleSheet(QString()); }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

    fv::ui::ThemeManager& manager() { return manager_; }

private:
    fv::ui::ThemeManager manager_;
};

} // namespace fvtest
