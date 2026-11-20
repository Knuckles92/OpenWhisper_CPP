#include "ui_settings/developer_page.h"

#include <QVBoxLayout>

#include "settings/keys.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace fv {

DeveloperPage::DeveloperPage(SettingsPage& page, SettingsDialog& dialog,
                             SettingsPagesSignals& signals)
    : QWidget(&page), dialog_(dialog), signals_(signals) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    coverageGuard_ = new SettingTile(
        QStringLiteral("Keep the live transcript if re-transcription is much shorter"),
        QStringLiteral("Reject a re-transcription with fewer than 80% of the live transcript's "
                       "words. Word count does not measure accuracy. Applies after End and when "
                       "retrying saved meetings. Off by default."),
        ui::designIcon(QStringLiteral("microphone-blue.svg")));
    dialog_.meetingBindings().checkbox(
        coverageGuard_->checkbox(), std::string(keys::meeting_redecode_coverage_guard.key()),
        [](const Json& settings) {
            return Json(keys::meeting_redecode_coverage_guard.read(settings));
        });
    page.addTileGroup(layout, QStringLiteral("Meeting re-transcription"), {coverageGuard_});

    developerMode_ = new SettingTile(
        QStringLiteral("Developer mode"),
        QStringLiteral("Unlocks a Load demo meeting control on the Meeting Mode tab. The demo "
                       "opens the dashboard with a fake transcript so you can test end-of-meeting "
                       "cleanup and the final report without recording a real meeting."),
        ui::designIcon(QStringLiteral("bolt-green.svg")));
    developerMode_->checkbox()->setObjectName(QStringLiteral("developerModeCheck"));
    connect(developerMode_->checkbox(), &QAbstractButton::toggled, this, [this](bool checked) {
        if (!dialog_.persist(keys::developer_mode.key(), checked)) {
            return;
        }
        Q_EMIT signals_.developerModeChanged(checked);
    });
    page.addTileGroup(layout, QStringLiteral("Developer"), {developerMode_});
}

void DeveloperPage::loadSettings(const Json& settings) {
    developerMode_->checkbox()->setChecked(keys::developer_mode.read(settings));
}

QWidget* DeveloperPage::namedControl(std::string_view name) {
    if (name == "meeting_redecode_coverage_guard_tile") {
        return coverageGuard_;
    }
    if (name == "meeting_redecode_coverage_guard_check") {
        return coverageGuard_->checkbox();
    }
    if (name == "developer_mode_tile") {
        return developerMode_;
    }
    if (name == "developer_mode_check") {
        return developerMode_->checkbox();
    }
    return nullptr;
}

} // namespace fv
