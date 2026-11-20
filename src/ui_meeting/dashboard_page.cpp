#include "ui_meeting/dashboard_page.h"

#include <algorithm>
#include <utility>

#include <QComboBox>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "settings/keys.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

namespace dashboard {

std::string rail_value(const Json& settings) {
    if (keys::meeting_server_bind.read(settings) != "lan") {
        return "Localhost";
    }
    const std::int64_t port = keys::meeting_server_port.read(settings);
    return "LAN · " + (port == 0 ? std::string("auto") : std::to_string(port));
}

} // namespace dashboard

DashboardPage::DashboardPage(SettingsPage& page, SettingsDialog& dialog)
    : QWidget(&page), dialog_(dialog) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    bindCombo_ = new ElidingComboBox();
    bindCombo_->setObjectName(QStringLiteral("meetingBindCombo"));
    bindCombo_->addItem(QStringLiteral("Localhost only (this computer)"),
                        QStringLiteral("localhost"));
    bindCombo_->addItem(QStringLiteral("Share on local network"), QStringLiteral("lan"));
    bindCombo_->setMinimumHeight(40);
    connect(bindCombo_, &QComboBox::currentIndexChanged, this, [this] { onBindChanged(); });
    bindTile_ = new FieldTile(QStringLiteral("Who can open the dashboard"),
                              QStringLiteral("Localhost keeps the live dashboard on this "
                                             "computer. Sharing serves it to other devices on "
                                             "your local network."),
                              bindCombo_, ui::designIcon(QStringLiteral("world-blue.svg")));
    bindWarning_ = new WrappedLabel(QStringLiteral(
        "Sharing on the local network serves the live meeting — running transcript, notes, "
        "insights, and audio playback — over plain, unencrypted HTTP. Anyone holding the guest "
        "link can read and edit the meeting and play the raw meeting recording."));
    bindWarning_->setObjectName(QStringLiteral("meetingBindWarning"));
    bindTile_->addBody(bindWarning_);
    page.addTileGroup(layout, QStringLiteral("Access"), {bindTile_});

    portSpin_ = new NoWheelSpinBox();
    portSpin_->setMinimum(0);
    portSpin_->setMaximum(65535);
    portSpin_->setSpecialValueText(QStringLiteral("Automatic"));
    portSpin_->setValue(static_cast<int>(keys::meeting_server_port.default_value()));
    portSpin_->setMinimumHeight(40);
    portSpin_->setMinimumWidth(120);
    // Save the finished port, not 8, 80, and 808 on the way to 8080.
    portSpin_->setKeyboardTracking(false);
    dialog_.meetingBindings().spin(
        portSpin_, std::string(keys::meeting_server_port.key()),
        [](const Json& settings) { return Json(keys::meeting_server_port.read(settings)); });
    portTile_ = new FieldTile(QStringLiteral("Dashboard port"),
                              QStringLiteral("Automatic lets the meeting server pick a free port "
                                             "each session. Pick a fixed port only if you need a "
                                             "stable link."),
                              portSpin_, ui::designIcon(QStringLiteral("server-blue.svg")), true);
    page.addTileGroup(layout, QStringLiteral("Port"), {portTile_});
}

const std::vector<std::string>& DashboardPage::controlNames() {
    static const std::vector<std::string> names{"meeting_bind_combo", "meeting_bind_tile",
                                                "meeting_bind_warning", "meeting_port_spinbox",
                                                "meeting_port_tile"};
    return names;
}

QWidget* DashboardPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, QWidget*> controls[] = {
        {"meeting_bind_combo", bindCombo_},     {"meeting_bind_tile", bindTile_},
        {"meeting_bind_warning", bindWarning_}, {"meeting_port_spinbox", portSpin_},
        {"meeting_port_tile", portTile_},
    };
    for (const auto& [key, widget] : controls) {
        if (key == name) {
            return widget;
        }
    }
    return nullptr;
}

void DashboardPage::loadSettings(const Json& settings) {
    bindCombo_->setCurrentIndex(
        std::max(0, bindCombo_->findData(qt::toQString(keys::meeting_server_bind.read(settings)))));
    updateBindUi();
}

void DashboardPage::onBindChanged() {
    updateBindUi();
    dialog_.persist(keys::meeting_server_bind.key(),
                    qt::toUtf8(bindCombo_->currentData().toString()));
}

void DashboardPage::updateBindUi() {
    bindWarning_->setVisible(bindCombo_->currentData().toString() == QStringLiteral("lan"));
}

} // namespace fv
