#pragma once

#include <string>
#include <vector>

#include <QApplication>
#include <QString>

#include "automation/action_model.h"
#include "base/json.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {

// The application styled the way OpenWhisper's QtApplication styles it (ThemeManager, theme,
// font scale) for one test case; back to 100 % and no sheet afterwards. Every test here that
// changes the font scale goes through it, so the scale never outlives its manager.
class AppTheme {
public:
    explicit AppTheme(const QString& theme = QStringLiteral("dark"), int scalePercent = 100)
        : manager_(theme) {
        fv::ui::applyUiTheme(theme, *qApp, manager_);
        fv::ui::applyUiFontScale(scalePercent, *qApp, manager_);
    }
    ~AppTheme() {
        fv::ui::applyUiFontScale(100, *qApp, manager_);
        qApp->setStyleSheet(QString());
    }
    AppTheme(const AppTheme&) = delete;
    AppTheme& operator=(const AppTheme&) = delete;

    void setScale(int percent) { fv::ui::applyUiFontScale(percent, *qApp, manager_); }

private:
    fv::ui::ThemeManager manager_;
};

// The fixed actions of the reference builder (tools/parity/builders/a21.py).
inline fv::automation::Action askClaude() {
    fv::automation::Action action;
    action.id = "ask-claude";
    action.name = "Ask Claude";
    action.kind = "open_link";
    action.params = fv::Json{{"template", "claude://claude.ai/new?q={text}"}};
    action.hotkey = "ctrl+alt+c";
    action.spoken_names = {"Claude"};
    return action;
}

inline fv::automation::Action homeAutomation() {
    fv::automation::Action action;
    action.id = "home";
    action.name = "Home automation";
    action.kind = "webhook";
    action.params = fv::Json{{"url", "https://example.com/hooks/home"}, {"output", "none"}};
    action.enabled = false;
    return action;
}

inline fv::automation::Action logIt() {
    fv::automation::Action action;
    action.id = "log-it";
    action.name = "Log it";
    action.kind = "program";
    action.params = fv::Json{{"program", "C:\\Tools\\log.exe"}};
    return action;
}

inline std::vector<fv::automation::Action> fixedActions() {
    return {askClaude(), homeAutomation(), logIt()};
}

} // namespace fvtest
