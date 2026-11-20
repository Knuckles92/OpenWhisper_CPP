#pragma once

#include <string_view>

#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"

namespace fv {

class SettingTile;
class SettingsDialog;
class SettingsPage;
class SettingsPagesSignals;
class SettingsSwitch;

// Settings → App → General (settings_dialog.py _build_general_page): the Output tiles (paste,
// restore the clipboard, copy), the Window tile (minimize to the tray, disabled with "This
// desktop session does not provide a system tray." and loaded off where there is none) and the
// Updates tiles, whose notify switch works only while automatic checks are on. The macOS
// accessibility row is not built (other platform).
// Affinity: GUI thread.
class GeneralPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // trayAvailable: QSystemTrayIcon.isSystemTrayAvailable; nativeWayland: a native Wayland
    // session, where pasting needs the desktop's help.
    GeneralPage(SettingsPage& page, SettingsDialog& dialog, SettingsPagesSignals& signals,
                bool trayAvailable, bool nativeWayland);

    SettingTile* autoPasteTile() const { return autoPaste_; }
    SettingTile* restoreClipboardTile() const { return restoreClipboard_; }
    SettingTile* copyClipboardTile() const { return copyClipboard_; }
    SettingTile* minimizeTrayTile() const { return minimizeTray_; }
    SettingTile* updateCheckTile() const { return updateCheck_; }
    SettingTile* updateNotifyTile() const { return updateNotify_; }
    SettingTile* updateRestartPromptTile() const { return updateRestartPrompt_; }
    SettingTile* updateBetaTile() const { return updateBeta_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

private:
    void onUpdateCheckToggled(bool checked);
    void onUpdateNotifyToggled();
    void onUpdateBetaToggled();

    SettingsDialog& dialog_;
    SettingsPagesSignals& signals_;
    SettingTile* autoPaste_ = nullptr;
    SettingTile* restoreClipboard_ = nullptr;
    SettingTile* copyClipboard_ = nullptr;
    SettingTile* minimizeTray_ = nullptr;
    SettingTile* updateCheck_ = nullptr;
    SettingTile* updateNotify_ = nullptr;
    SettingTile* updateRestartPrompt_ = nullptr;
    SettingTile* updateBeta_ = nullptr;
};

} // namespace fv
