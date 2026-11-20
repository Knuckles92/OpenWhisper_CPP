#pragma once

#include <string_view>

#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"
#include "ui_hotkeys/hotkeys_page.h"

namespace fv {

class SettingsDialog;
class SettingsPagesSignals;
class SettingsStore;

// The Settings window as the Hotkeys page's host (HotkeysPageHost over SettingsDialog): the
// footer line, the dialog's capture suspension, its persist and destinations, and the saved
// shortcuts, which go to SettingsPagesSignals::hotkeysChanged while that has a receiver (the
// controller saves and registers them) and are saved here otherwise
// (settings_manager.save_hotkey_settings). A save redraws the rail once the page holds the new
// shortcuts, after its message. Parented to the dialog, so it outlives the page, which the dialog
// destroys first.
// Affinity: GUI thread.
class HotkeysPageAdapter : public QObject, public HotkeysPageHost {
    Q_OBJECT
public:
    HotkeysPageAdapter(SettingsDialog& dialog, SettingsStore& store, SettingsPagesSignals& signals);

    // Builds the page under parent and installs the dialog's standard-shortcut handler on it
    // (set_standard_hotkey).
    HotkeysPage* createPage(QWidget* parent);
    HotkeysPage* page() const { return page_; }

    // HotkeysPageHost.
    void showMessage(const QString& message) override;
    void setCaptureSuspended(bool suspended) override;
    void hotkeysChanged(const Json& hotkeys) override;
    void triggerModeChanged(const QString& mode) override;
    void openDestination(const QString& destination) override;
    bool persist(std::string_view key, const Json& value) override;

private:
    // Home's record shortcut and what it does, when Home is built.
    void refreshHomeShortcut();

    QPointer<SettingsDialog> dialog_;
    SettingsStore& store_;
    QPointer<SettingsPagesSignals> signals_;
    QPointer<HotkeysPage> page_;
    bool railPending_ = false;
};

} // namespace fv
