#pragma once

#include <string>
#include <string_view>
#include <vector>

#include <QWidget>

#include "appkit/settings_page.h"
#include "base/json.h"

class QComboBox;
class QSpinBox;

namespace fv {

class FieldTile;
class SettingsDialog;
class SettingsPage;
class WrappedLabel;

namespace dashboard {

// _meeting_dashboard_rail_value: "Localhost", or "LAN · auto" / "LAN · {port}" when shared on the
// local network.
std::string rail_value(const Json& settings);

} // namespace dashboard

// Settings → Meeting Mode → Dashboard (settings_dialog.py _build_meeting_dashboard_page): who can
// open the live dashboard (this computer, or the local network with a warning about what that
// serves) and its port (0 shows "Automatic"). The port saves only its key through the dialog's
// meeting binder, once a value is finished (no keyboard tracking); the access choice saves and
// shows the warning only for "lan".
// Affinity: GUI thread.
class DashboardPage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    DashboardPage(SettingsPage& page, SettingsDialog& dialog);

    QComboBox* bindCombo() const { return bindCombo_; }
    FieldTile* bindTile() const { return bindTile_; }
    WrappedLabel* bindWarning() const { return bindWarning_; }
    QSpinBox* portSpin() const { return portSpin_; }
    FieldTile* portTile() const { return portTile_; }

    // SettingsPageModule.
    void loadSettings(const Json& settings) override;
    QWidget* namedControl(std::string_view name) override;

    // The dialog's CONTROL_DESTINATIONS for meeting_dashboard.
    static const std::vector<std::string>& controlNames();

private:
    void onBindChanged();
    void updateBindUi();

    SettingsDialog& dialog_;
    QComboBox* bindCombo_ = nullptr;
    FieldTile* bindTile_ = nullptr;
    WrappedLabel* bindWarning_ = nullptr;
    QSpinBox* portSpin_ = nullptr;
    FieldTile* portTile_ = nullptr;
};

} // namespace fv
