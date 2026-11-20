#pragma once

#include <functional>

#include <QObject>
#include <QPointer>
#include <QString>

class QWidget;

namespace fv {

class AppContext;
class SettingsDialog;

// The app's one Settings window (UIController's open_settings_dialog, open_settings_destination,
// open_engine_help_destination, open_hotkey_settings, open_downloads and _raise_dialog), provided
// as a feature service (ctx.service<SettingsWindow>()). The dialog is created on first use, as a
// child of the main window, and reused: every opening refreshes it, picks the destination, then
// shows, raises and activates it. A page that cannot be built (its create throws) is logged and
// left unbuilt; the window opens all the same.
// Affinity: GUI thread.
class SettingsWindow : public QObject {
    Q_OBJECT
public:
    explicit SettingsWindow(AppContext& context, QObject* parent = nullptr);
    ~SettingsWindow() override;

    // The window the dialog is a child of (the main window); set before the first opening.
    void setParentWindow(QWidget* window) { parentWindow_ = window; }

    // Without a destination, open_settings_dialog: Home, scrolled to the top. Otherwise
    // open_settings_destination: "engine_downloads" opens Downloads on the missing runtime,
    // "text" reveals the AI cleanup model, anything else (Home included, which keeps its
    // scroll) is selected by key or alias.
    void open(const QString& destination = {});
    // open_settings_dialog(focus_hf_policy=True): Downloads with its policy control focused.
    void openHfPolicy();
    // open_hotkey_settings.
    void openHotkeys();
    // open_downloads: Downloads, then the component focused. A component the guided setup
    // already shows the install of opens nothing.
    void openDownloads(const QString& componentId = {});
    // open_engine_help_destination: "api_keys" opens API keys on the selected speech
    // endpoint's credential (the page alone when that server needs no key); "ondemand",
    // "downloads" and "remote_engine" open that destination. "remote_models" asks the
    // paired host's opener first and falls back to the Remote engine page if unpaired.
    void openEngineHelp(const QString& destination);
    // Set by the remote feature. Returns true after opening a paired host's model manager.
    void setRemoteModelsOpener(std::function<bool(QWidget*)> open);

    // The dialog, null until the first opening.
    SettingsDialog* dialog() const;
    // Creates the dialog when it does not exist yet, without showing it.
    SettingsDialog& ensureDialog();
    bool isVisible() const;

    // get_missing_local_runtime: the component id of the selected engine's missing runtime, or
    // "" when none has been reported.
    void setMissingRuntimeProvider(std::function<QString()> provider);
    // The guided setup's shows_runtime_install(component_id).
    void setRuntimeInstallShownElsewhere(std::function<bool(const QString&)> shown);
    // Shows, raises and activates the dialog (_raise_dialog), through a seam a test replaces.
    std::function<void(SettingsDialog&)> raiseDialog;

Q_SIGNALS:
    // The dialog was just created: the controller connects its callbacks here, once.
    void dialogCreated(fv::SettingsDialog* dialog);

private:
    // _prepare_settings_dialog then refresh().
    SettingsDialog& prepared();
    void raise(SettingsDialog& dialog);

    AppContext& context_;
    QPointer<QWidget> parentWindow_;
    QPointer<SettingsDialog> dialog_;
    std::function<QString()> missingRuntime_;
    std::function<bool(const QString&)> installShownElsewhere_;
    std::function<bool(QWidget*)> openRemoteModels_;
};

} // namespace fv
