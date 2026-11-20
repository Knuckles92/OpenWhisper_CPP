#pragma once

#include <functional>
#include <string_view>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"
#include "ui_automation/action_fields.h"

namespace fv {

class SettingsDialog;

// What an Automate page asks of the Settings window around it: the calls OpenWhisper's
// settings_actions module makes on its `dialog` (_settings_snapshot, _persist, _loading,
// notify_changed, select_destination, set_hotkey_capture_suspended, set_standard_hotkey,
// current_hotkeys and the `finished` signal). A page codes against this interface, so a test can
// hand it a stand-in.
// Affinity: GUI thread.
class SettingsPageHost : public StandardShortcutHost {
public:
    // _persist: saves one key. Refused (false) while the window loads; a failure is shown in the
    // window's message line.
    virtual bool persist(std::string_view key, const Json& value) = 0;
    // Whether the window is loading saved values into its pages: nothing is saved meanwhile.
    virtual bool isLoading() const = 0;
    // Redraws the rail, then says what changed ("actions").
    virtual void notifyChanged(const QString& kind) = 0;
    virtual void selectDestination(const QString& key) = 0;
    // Runs `fn` each time the Settings window closes, for as long as `receiver` exists.
    virtual void onClosed(QObject* receiver, std::function<void()> fn) = 0;

protected:
    SettingsPageHost() = default;
    SettingsPageHost(const SettingsPageHost&) = default;
    SettingsPageHost& operator=(const SettingsPageHost&) = default;
};

// The Settings window as a page's host. Every call is forwarded to the dialog; one made after the
// dialog is gone does nothing and answers false, "", an empty value or nothing.
// Affinity: GUI thread.
class DialogPageHost : public QObject, public SettingsPageHost {
    Q_OBJECT
public:
    explicit DialogPageHost(SettingsDialog& dialog, QObject* parent = nullptr);

    SettingsDialog* dialog() const;

    Json settings() override;
    Json currentHotkeys() override;
    QString setStandardHotkey(const QString& action, const QString& hotkey) override;
    void setHotkeyCaptureSuspended(bool suspended) override;
    bool persist(std::string_view key, const Json& value) override;
    bool isLoading() const override;
    void notifyChanged(const QString& kind) override;
    void selectDestination(const QString& key) override;
    void onClosed(QObject* receiver, std::function<void()> fn) override;

private:
    QPointer<SettingsDialog> dialog_;
};

} // namespace fv
