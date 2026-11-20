#include "ui_settings/general_page.h"

#include <utility>

#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "settings/keys.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/settings_dialog.h"
#include "ui_settings/settings_page.h"
#include "ui_theme/icons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"

namespace fv {
namespace {

QString product() {
    return qt::toQString(identity().product);
}

SettingTile* tile(const QString& title, const QString& description, const char* icon) {
    return new SettingTile(title, description, ui::designIcon(QString::fromLatin1(icon)));
}

} // namespace

GeneralPage::GeneralPage(SettingsPage& page, SettingsDialog& dialog, SettingsPagesSignals& signals,
                         bool trayAvailable, bool nativeWayland)
    : QWidget(&page), dialog_(dialog), signals_(signals) {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    QString autoPasteDescription = QStringLiteral(
        "Types a dictated transcript into whichever window has focus, pasting it through the "
        "clipboard. Upload File results stay in the window with their own Copy buttons.");
    if (nativeWayland) {
        // Hyprland's dispatcher (the other half of this sentence) is not ported: Linux only.
        autoPasteDescription +=
            QStringLiteral(" This Wayland desktop requires manual clipboard paste.");
    }
    autoPaste_ = tile(QStringLiteral("Paste into the active window"), autoPasteDescription,
                      "bolt-green.svg");
    restoreClipboard_ =
        tile(QStringLiteral("Restore the clipboard after pasting"),
             QStringLiteral("Puts back what you had copied once a paste lands, including Command "
                            "Mode rewrites. Turn off to keep the pasted text on the clipboard."),
             "arrow-back-up-gray.svg");
    copyClipboard_ =
        tile(QStringLiteral("Copy to the clipboard"),
             QStringLiteral("Keeps the transcript on the clipboard when auto-paste is off or "
                            "unavailable."),
             "stack-slate.svg");
    minimizeTray_ = tile(QStringLiteral("Minimize to the system tray on close"),
                         QStringLiteral("Closing the window keeps %1 running in the tray, so "
                                        "hotkeys and recording stay available.")
                             .arg(product()),
                         "box-blue.svg");
    if (!trayAvailable) {
        minimizeTray_->setEnabled(false);
        minimizeTray_->setDescription(
            QStringLiteral("This desktop session does not provide a system tray."));
    }
    updateCheck_ = tile(QStringLiteral("Check for updates automatically"),
                        QStringLiteral("When %1 starts and every 4 hours while it runs, asks "
                                       "GitHub (api.github.com) for the latest release. The "
                                       "request includes the app version, no account or device "
                                       "ID. Nothing is installed without your approval.")
                            .arg(product()),
                        "refresh-blue.svg");
    updateCheck_->checkbox()->setObjectName(QStringLiteral("updateCheckEnabledCheck"));
    updateNotify_ = tile(QStringLiteral("Notify me about updates"),
                         QStringLiteral("Shows an Update button in the title bar and the tray "
                                        "menu when a newer release is found. Requires automatic "
                                        "checks."),
                         "bell-amber.svg");
    updateNotify_->checkbox()->setObjectName(QStringLiteral("updateNotifyEnabledCheck"));
    updateRestartPrompt_ =
        tile(QStringLiteral("Ask me to restart after an update"),
             QStringLiteral("When %1 is updated while it is running, offers to restart so the "
                            "update finishes. Turn off if you chose \"Don't show this again\" "
                            "and changed your mind.")
                 .arg(product()),
             "download-blue.svg");
    updateRestartPrompt_->checkbox()->setObjectName(QStringLiteral("updateRestartPromptCheck"));
    updateBeta_ = tile(QStringLiteral("Get beta updates"),
                       QStringLiteral("Also offers test releases a few days before everyone else "
                                      "gets them. They are verified like any release but may "
                                      "have rough edges. Turning this off never downgrades: you "
                                      "keep the beta until a newer regular release is out."),
                       "stack-purple.svg");
    updateBeta_->checkbox()->setObjectName(QStringLiteral("updateBetaTrackCheck"));

    page.addTileGroup(layout, QStringLiteral("Output"),
                      {autoPaste_, restoreClipboard_, copyClipboard_});
    page.addTileGroup(layout, QStringLiteral("Window"), {minimizeTray_});
    page.addTileGroup(layout, QStringLiteral("Updates"),
                      {updateCheck_, updateNotify_, updateRestartPrompt_, updateBeta_});

    SettingsBinder& bindings = dialog_.bindings();
    const auto read = [](const BoolSetting& setting) {
        return [&setting](const Json& settings) {
            return Json(setting.read(settings));
        };
    };
    bindings.checkbox(autoPaste_->checkbox(), std::string(keys::auto_paste.key()),
                      read(keys::auto_paste));
    bindings.checkbox(restoreClipboard_->checkbox(), std::string(keys::restore_clipboard.key()),
                      read(keys::restore_clipboard));
    bindings.checkbox(copyClipboard_->checkbox(), std::string(keys::copy_clipboard.key()),
                      read(keys::copy_clipboard));
    bindings.checkbox(minimizeTray_->checkbox(), std::string(keys::minimize_tray.key()),
                      [trayAvailable](const Json& settings) {
                          return Json(trayAvailable && keys::minimize_tray.read(settings));
                      });
    connect(updateCheck_->checkbox(), &QAbstractButton::toggled, this,
            [this](bool checked) { onUpdateCheckToggled(checked); });
    bindings.checkbox(updateNotify_->checkbox(), std::string(keys::update_notify_enabled.key()),
                      read(keys::update_notify_enabled));
    connect(updateNotify_->checkbox(), &QAbstractButton::toggled, this,
            [this] { onUpdateNotifyToggled(); });
    bindings.checkbox(updateRestartPrompt_->checkbox(),
                      std::string(keys::update_restart_prompt_enabled.key()),
                      read(keys::update_restart_prompt_enabled));
    bindings.checkbox(updateBeta_->checkbox(), std::string(keys::update_beta_track_enabled.key()),
                      read(keys::update_beta_track_enabled));
    connect(updateBeta_->checkbox(), &QAbstractButton::toggled, this,
            [this] { onUpdateBetaToggled(); });
}

void GeneralPage::loadSettings(const Json& settings) {
    updateCheck_->checkbox()->setChecked(keys::update_check_enabled.read(settings));
    updateNotify_->setEnabled(updateCheck_->checkbox()->isChecked());
}

QWidget* GeneralPage::namedControl(std::string_view name) {
    const std::pair<std::string_view, SettingTile*> tiles[] = {
        {"auto_paste", autoPaste_},
        {"restore_clipboard", restoreClipboard_},
        {"copy_clipboard", copyClipboard_},
        {"minimize_tray", minimizeTray_},
        {"update_check", updateCheck_},
        {"update_notify", updateNotify_},
        {"update_restart_prompt", updateRestartPrompt_},
        {"update_beta", updateBeta_},
    };
    for (const auto& [stem, setting] : tiles) {
        if (name.starts_with(stem)) {
            const std::string_view rest = name.substr(stem.size());
            if (rest == "_tile") {
                return setting;
            }
            if (rest == "_check") {
                return setting->checkbox();
            }
        }
    }
    return nullptr;
}

void GeneralPage::onUpdateCheckToggled(bool checked) {
    updateNotify_->setEnabled(checked);
    if (dialog_.persist(keys::update_check_enabled.key(), checked)) {
        dialog_.notifyChanged(QStringLiteral("updates"));
        Q_EMIT signals_.updatesChanged();
    }
}

void GeneralPage::onUpdateNotifyToggled() {
    // The binding connected first, so the choice is already saved.
    if (!dialog_.isLoading()) {
        dialog_.notifyChanged(QStringLiteral("updates"));
        Q_EMIT signals_.updatesChanged();
    }
}

void GeneralPage::onUpdateBetaToggled() {
    // The binding connected first, so the choice is already saved. Asking the updater to check
    // again soon (release_track_changed) is the updater's own reaction to this setting (M3).
    if (!dialog_.isLoading()) {
        dialog_.notifyChanged(QStringLiteral("updates"));
        Q_EMIT signals_.updatesChanged();
    }
}

} // namespace fv
