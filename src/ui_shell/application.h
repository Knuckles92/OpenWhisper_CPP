#pragma once

#include <memory>

#include <QObject>
#include <QString>

class QApplication;
class QMainWindow;

namespace fv {

class SettingsStore;

namespace ui {
class ThemeManager;
}

// The Qt application as OpenWhisper_CPP sets it up (ui_qt/app.py QtApplication): names, the app
// icon, the windows11 style wrapped for snappy, rounded tooltips, the palette of the saved ui_theme
// (built before the splash paints, so it reads the right colours), the saved ui_font_scale,
// and, while the preference is "system", the platform colour scheme followed live. Uses the
// QApplication that exists, or creates one.
// Affinity: GUI thread; one per process.
class Application : public QObject {
    Q_OBJECT
public:
    // argc and argv must outlive the application, as QApplication requires.
    Application(SettingsStore& settings, int& argc, char** argv);
    ~Application() override;

    QApplication& app() const { return *app_; }
    ui::ThemeManager& themeManager() const { return *themeManager_; }

    // Applies a UiTheme preference; true when the palette changed.
    bool setTheme(const QString& preference);
    void applyFontScale(int percent);

    // Shows the window (when given) and runs the event loop; returns its exit code.
    int run(QMainWindow* window = nullptr);
    void quit();
    void exit(int code = 0);

private:
    std::unique_ptr<QApplication> owned_;
    QApplication* app_;
    std::unique_ptr<ui::ThemeManager> themeManager_;
};

} // namespace fv
