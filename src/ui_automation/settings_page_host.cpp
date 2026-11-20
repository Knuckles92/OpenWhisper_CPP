#include "ui_automation/settings_page_host.h"

#include <utility>

#include "appkit/destinations.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_settings/settings_dialog.h"

namespace fv {

DialogPageHost::DialogPageHost(SettingsDialog& dialog, QObject* parent)
    : QObject(parent), dialog_(&dialog) {}

SettingsDialog* DialogPageHost::dialog() const {
    return dialog_;
}

Json DialogPageHost::settings() {
    if (dialog_ == nullptr) {
        return Json::object();
    }
    return *dialog_->settingsSnapshot();
}

Json DialogPageHost::currentHotkeys() {
    if (dialog_ == nullptr) {
        return Json::object();
    }
    // What the Hotkeys page holds once it has read them, else the defaults with the saved ones
    // over them.
    if (auto* page = dynamic_cast<HotkeysPage*>(dialog_->pageContent(destinations::hotkeys));
        page != nullptr && !page->currentHotkeys().empty()) {
        return page->currentHotkeys();
    }
    Json hotkeys = default_hotkeys();
    const Json settings = *dialog_->settingsSnapshot();
    if (settings.is_object()) {
        const auto saved = settings.find(keys::hotkeys.key());
        if (saved != settings.end() && saved->is_object()) {
            for (const auto& [action, value] : saved->items()) {
                hotkeys[action] = value;
            }
        }
    }
    return hotkeys;
}

QString DialogPageHost::setStandardHotkey(const QString& action, const QString& hotkey) {
    if (dialog_ == nullptr) {
        return QStringLiteral("That shortcut could not be saved.");
    }
    return dialog_->setStandardHotkey(action, hotkey);
}

void DialogPageHost::setHotkeyCaptureSuspended(bool suspended) {
    if (dialog_ != nullptr) {
        dialog_->setHotkeyCaptureSuspended(suspended);
    }
}

bool DialogPageHost::persist(std::string_view key, const Json& value) {
    return dialog_ != nullptr && dialog_->persist(key, value);
}

bool DialogPageHost::isLoading() const {
    return dialog_ != nullptr && dialog_->isLoading();
}

void DialogPageHost::notifyChanged(const QString& kind) {
    if (dialog_ != nullptr) {
        dialog_->notifyChanged(kind);
    }
}

void DialogPageHost::selectDestination(const QString& key) {
    if (dialog_ != nullptr) {
        dialog_->selectDestination(key);
    }
}

void DialogPageHost::onClosed(QObject* receiver, std::function<void()> fn) {
    if (dialog_ != nullptr) {
        connect(dialog_.data(), &SettingsDialog::closed, receiver, std::move(fn));
    }
}

} // namespace fv
