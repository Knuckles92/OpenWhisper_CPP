#include "ui_personalize/personalize_host.h"

#include <exception>
#include <typeinfo>
#include <utility>

#include "appkit/app_context.h"
#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "pyport/errors.h"
#include "settings/hotkeys.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_settings/settings_dialog.h"

namespace fv {
namespace {

// The only page that saves through mutate is the Dictionary (settings_dictionary._mutate).
constexpr log::Logger kDictionaryLog{"ui_qt.dialogs.settings_dictionary"};

} // namespace

DialogPersonalizeHost::DialogPersonalizeHost(SettingsDialog& dialog, QObject* parent)
    : QObject(parent), dialog_(&dialog) {}

Json DialogPersonalizeHost::settings() {
    if (dialog_ == nullptr) {
        return Json::object();
    }
    return *dialog_->settingsSnapshot();
}

bool DialogPersonalizeHost::persist(std::string_view key, const Json& value) {
    return dialog_ != nullptr && dialog_->persist(key, value);
}

bool DialogPersonalizeHost::persistMany(const Json& updates, std::vector<std::string> drops) {
    return dialog_ != nullptr && dialog_->persistMany(updates, std::move(drops));
}

bool DialogPersonalizeHost::mutate(const std::function<void(Json&)>& change, QString* error) {
    if (error != nullptr) {
        error->clear();
    }
    if (dialog_ == nullptr) {
        return false;
    }
    try {
        dialog_->context().settings().mutate(change);
        return true;
    } catch (const py::ValueError& refused) {
        // A rule the user can fix (a dictionary entry that is too long, a duplicate trigger).
        if (error != nullptr) {
            *error = qt::toQString(refused.what());
        }
    } catch (const std::exception& failure) {
        // The type only: what() may hold a dictionary entry.
        kDictionaryLog.warning("Couldn't save the dictionary ({})",
                               type_display_name(typeid(failure)));
    }
    return false;
}

bool DialogPersonalizeHost::isLoading() const {
    return dialog_ != nullptr && dialog_->isLoading();
}

void DialogPersonalizeHost::notifyChanged(const QString& kind) {
    if (dialog_ != nullptr) {
        dialog_->notifyChanged(kind);
    }
}

void DialogPersonalizeHost::selectDestination(const QString& key) {
    if (dialog_ != nullptr) {
        dialog_->selectDestination(key);
    }
}

void DialogPersonalizeHost::reveal(QWidget* target) {
    if (dialog_ != nullptr) {
        dialog_->reveal(target);
    }
}

void DialogPersonalizeHost::showMessage(const QString& text) {
    if (dialog_ != nullptr) {
        dialog_->showMessage(text);
    }
}

QString DialogPersonalizeHost::setStandardHotkey(const QString& action, const QString& hotkey) {
    if (dialog_ == nullptr) {
        return QStringLiteral("That shortcut could not be saved.");
    }
    return dialog_->setStandardHotkey(action, hotkey);
}

void DialogPersonalizeHost::setHotkeyCaptureSuspended(bool suspended) {
    if (dialog_ != nullptr) {
        dialog_->setHotkeyCaptureSuspended(suspended);
    }
}

Json DialogPersonalizeHost::currentHotkeys() {
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

QWidget* DialogPersonalizeHost::control(std::string_view name) {
    return dialog_ != nullptr ? dialog_->control(name) : nullptr;
}

} // namespace fv
