#pragma once

// What the Downloads widget tests share: the application styled as OpenWhisper styles it, the
// catalog's own copy for a Windows host, and small finders for the buttons of a dialog.

#include <string>
#include <vector>

#include <QApplication>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QStringList>

#include "models/catalog.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/style_props.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// The application styled as OpenWhisper's QtApplication styles it, for one test case.
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

private:
    fv::ui::ThemeManager manager_;
};

// A Windows x64 host with nothing installed and no GPU: the platform the sizes come from.
inline fv::models::Host windowsHost() {
    fv::models::Host host;
    host.platform = std::string(fv::models::platform::kWinAmd64);
    return host;
}

// The button with this object name or buttonId property (the tests' _button helper: primary
// and danger buttons keep their class object name for the theme and carry their identifier in
// buttonId).
template <class Dialog> QPushButton* buttonNamed(Dialog& dialog, const QString& name) {
    const QList<QPushButton*> buttons = dialog.template findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->objectName() == name ||
            button->property(fv::ui::prop::buttonId).toString() == name) {
            return button;
        }
    }
    return nullptr;
}

// The buttons of a dialog by their text.
template <class Dialog> QPushButton* buttonText(Dialog& dialog, const QString& text) {
    const QList<QPushButton*> buttons = dialog.template findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->text() == text) {
            return button;
        }
    }
    return nullptr;
}

// The texts of the buttons that are the dialog's default.
template <class Dialog> QStringList defaultButtons(Dialog& dialog) {
    QStringList names;
    const QList<QPushButton*> buttons = dialog.template findChildren<QPushButton*>();
    for (QPushButton* button : buttons) {
        if (button->isDefault()) {
            names.append(button->text());
        }
    }
    return names;
}

} // namespace fvtest
