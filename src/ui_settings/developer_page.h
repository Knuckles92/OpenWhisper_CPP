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

// Settings → App → Developer options (settings_dialog.py _build_advanced_page): the meeting
// re-transcription guard (meeting_redecode_coverage_guard, bound through the dialog's meeting
// binder, so it only saves) above the Developer mode tile.
// Affinity: GUI thread.
class DeveloperPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    DeveloperPage(SettingsPage& page, SettingsDialog& dialog, SettingsPagesSignals& signals);

    SettingTile* coverageGuardTile() const { return coverageGuard_; }
    SettingTile* developerModeTile() const { return developerMode_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

private:
    SettingsDialog& dialog_;
    SettingsPagesSignals& signals_;
    SettingTile* coverageGuard_ = nullptr;
    SettingTile* developerMode_ = nullptr;
};

} // namespace fv
