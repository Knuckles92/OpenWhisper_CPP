#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"
#include "ui_settings/settings_dialog.h"

class QWidget;

namespace fv {

// What a Personalize page asks of the Settings window around it: the calls OpenWhisper's page
// modules make on their `dialog` (_settings_snapshot, _persist, _persist_many, _loading,
// notify_changed, select_destination, _reveal, set_standard_hotkey,
// set_hotkey_capture_suspended, current_hotkeys, the message line and the control attributes).
// A page codes against this interface, so a test hands it a FakePersonalizeHost.
// Affinity: GUI thread.
class PersonalizeHost {
public:
    virtual ~PersonalizeHost() = default;

    // The settings the page shows: the snapshot the current refresh reads, else the store's
    // (_settings_snapshot).
    virtual Json settings() = 0;
    // _persist: saves one key. Does nothing while loading (false), and does not touch the file
    // when the stored value already equals `value`; a failure is shown in the message line.
    virtual bool persist(std::string_view key, const Json& value) = 0;
    // _persist_many: merges `updates` and drops the keys in `drops`.
    virtual bool persistMany(const Json& updates, std::vector<std::string> drops = {}) = 0;
    // settings_manager.mutate_settings: `change` edits the saved mapping and the result is
    // written at once. False when nothing was written: when `change` threw a ValueError
    // (py::ValueError, for example a dictionary or snippet rule refused), `error` receives its
    // text for the page to show; any other failure is logged and leaves `error` empty, so the
    // page shows its own "couldn't save" line.
    virtual bool mutate(const std::function<void(Json&)>& change, QString* error = nullptr) = 0;
    // Whether the window is loading saved values into its pages: nothing is saved meanwhile.
    virtual bool isLoading() const = 0;
    // Redraws the rail, then says what changed ("transforms", "hotkeys", "dictionary",
    // "snippets", "styles", "languages", "microphones", "updates", "actions").
    virtual void notifyChanged(const QString& kind) = 0;
    virtual void selectDestination(const QString& key) = 0;
    // Scrolls a control into view, tints its card and focuses it.
    virtual void reveal(QWidget* target) = 0;
    // The shared line in the window's footer; "" clears it.
    virtual void showMessage(const QString& text) = 0;
    // Saves one standard shortcut through the Hotkeys page's path: "" once saved, else why not.
    virtual QString setStandardHotkey(const QString& action, const QString& hotkey) = 0;
    // Pauses global hotkeys while a field captures a shortcut, and resumes them.
    virtual void setHotkeyCaptureSuspended(bool suspended) = 0;
    // The standard shortcuts: what the Hotkeys page holds, else the defaults with the saved ones
    // over them (current_hotkeys).
    virtual Json currentHotkeys() = 0;
    // A control another page exposes by name (building that page first); null when none does.
    virtual QWidget* control(std::string_view name) = 0;

protected:
    PersonalizeHost() = default;
    PersonalizeHost(const PersonalizeHost&) = default;
    PersonalizeHost& operator=(const PersonalizeHost&) = default;
};

// The Settings window as a page's host. Every call is forwarded to the dialog; one made after the
// dialog is gone does nothing and answers false, "", an empty value or null.
// registerPersonalizePage makes one per page and hands it to the dialog, which deletes its pages
// before it, so nothing torn down with a page reaches a dead host.
// Affinity: GUI thread.
class DialogPersonalizeHost : public QObject, public PersonalizeHost {
    Q_OBJECT
public:
    DialogPersonalizeHost(SettingsDialog& dialog, QObject* parent = nullptr);

    SettingsDialog* dialog() const { return dialog_; }

    Json settings() override;
    bool persist(std::string_view key, const Json& value) override;
    bool persistMany(const Json& updates, std::vector<std::string> drops = {}) override;
    bool mutate(const std::function<void(Json&)>& change, QString* error = nullptr) override;
    bool isLoading() const override;
    void notifyChanged(const QString& kind) override;
    void selectDestination(const QString& key) override;
    void reveal(QWidget* target) override;
    void showMessage(const QString& text) override;
    QString setStandardHotkey(const QString& action, const QString& hotkey) override;
    void setHotkeyCaptureSuspended(bool suspended) override;
    Json currentHotkeys() override;
    QWidget* control(std::string_view name) override;

private:
    QPointer<SettingsDialog> dialog_;
};

} // namespace fv
