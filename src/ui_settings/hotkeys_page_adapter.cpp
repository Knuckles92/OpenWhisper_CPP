#include "ui_settings/hotkeys_page_adapter.h"

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_settings/app_pages.h"
#include "ui_settings/home_page.h"
#include "ui_settings/settings_dialog.h"

namespace fv {

HotkeysPageAdapter::HotkeysPageAdapter(SettingsDialog& dialog, SettingsStore& store,
                                       SettingsPagesSignals& signals)
    : QObject(&dialog), dialog_(&dialog), store_(store), signals_(&signals) {}

HotkeysPage* HotkeysPageAdapter::createPage(QWidget* parent) {
    page_ = new HotkeysPage(store_, *this, parent);
    QPointer<HotkeysPage> page = page_;
    dialog_->setStandardHotkeyHandler([page](const QString& action, const QString& hotkey) {
        if (page == nullptr) {
            return QStringLiteral("Couldn't save hotkeys: no Hotkeys page is open.");
        }
        return page->setStandardHotkey(qt::toUtf8(action), qt::toUtf8(hotkey));
    });
    return page_;
}

void HotkeysPageAdapter::showMessage(const QString& message) {
    if (dialog_ == nullptr) {
        return;
    }
    dialog_->showMessage(message);
    // _save_hotkeys: the message, then the rail, once the page holds what was saved.
    if (railPending_) {
        railPending_ = false;
        refreshHomeShortcut();
        dialog_->refreshRailValues();
    }
}

void HotkeysPageAdapter::refreshHomeShortcut() {
    // _update_hotkey_displays and _on_hands_free_latch_toggled: Home's shortcut row follows at
    // once, not only when Home is shown next.
    if (dialog_ == nullptr || !dialog_->isBuilt(destinations::overview)) {
        return;
    }
    if (auto* home = dynamic_cast<HomePage*>(dialog_->pageContent(destinations::overview))) {
        home->refreshShortcut();
    }
}

void HotkeysPageAdapter::setCaptureSuspended(bool suspended) {
    if (dialog_ == nullptr) {
        return;
    }
    dialog_->setHotkeyCaptureSuspended(suspended);
    // keyPressEvent: Esc while a standard shortcut is being captured stops the capture and keeps
    // Settings open; otherwise it closes Settings as always.
    if (!suspended) {
        dialog_->setEscapeOverride({});
        return;
    }
    QPointer<HotkeysPage> page = page_;
    QPointer<SettingsDialog> dialog = dialog_;
    dialog_->setEscapeOverride([page, dialog] {
        if (page != nullptr && page->capturingKey().has_value()) {
            page->cancelCapture();
        } else if (dialog != nullptr) {
            dialog->reject();
        }
    });
}

void HotkeysPageAdapter::hotkeysChanged(const Json& hotkeys) {
    if (signals_ != nullptr && signals_->hasHotkeysReceiver()) {
        Q_EMIT signals_->hotkeysChanged(hotkeys);
    } else {
        save_hotkey_settings(store_, hotkeys);
    }
    // Reached only when the shortcuts were saved: the page shows its message next.
    railPending_ = true;
}

void HotkeysPageAdapter::triggerModeChanged(const QString& mode) {
    if (signals_ != nullptr) {
        Q_EMIT signals_->recordingTriggerModeChanged(mode);
    }
}

void HotkeysPageAdapter::openDestination(const QString& destination) {
    if (dialog_ != nullptr) {
        dialog_->selectDestination(destination);
    }
}

bool HotkeysPageAdapter::persist(std::string_view key, const Json& value) {
    const bool saved = dialog_ != nullptr && dialog_->persist(key, value);
    if (saved && key == keys::recording_hands_free_latch.key()) {
        refreshHomeShortcut();
    }
    return saved;
}

} // namespace fv
