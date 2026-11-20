#include "ui_automation/action_bindings.h"

#include <memory>
#include <optional>
#include <utility>

#include <QStackedWidget>

#include "appkit/destinations.h"
#include "appkit/qt_bridge.h"
#include "automation/action_copy.h"
#include "automation/action_model.h"
#include "hotkeys/conflicts.h"
#include "ui_hotkeys/hotkeys_page.h"
#include "ui_settings/settings_dialog.h"

namespace fv {

ActionBindingSource::ActionBindingSource(std::function<void()> saved) : saved_(std::move(saved)) {}

std::vector<hotkey_bindings::Binding> ActionBindingSource::list(const Json& settings) const {
    std::vector<hotkey_bindings::Binding> bindings;
    for (const automation::Action& action : automation::load_actions(settings)) {
        bindings.push_back({std::string(hotkeys::kAction), action.id, action.name, action.hotkey,
                            automation::action_copy::summary(action) + ".", action.enabled});
    }
    return bindings;
}

bool ActionBindingSource::save(Json& settings, const std::string& id, const std::string& hotkey) {
    std::optional<automation::Action> action = automation::find_action(settings, id);
    if (!action) {
        return false;
    }
    action->hotkey = hotkey;
    automation::save_action(settings, *action);
    return true;
}

void ActionBindingSource::saved() {
    if (saved_) {
        saved_();
    }
}

void registerActionBindingSource(HotkeysPage& page, std::function<void()> saved) {
    page.registerBindingSource(std::string(hotkeys::kAction),
                               std::make_shared<ActionBindingSource>(std::move(saved)));
}

ActionBindingsWiring::ActionBindingsWiring(SettingsDialog& dialog)
    : QObject(&dialog), dialog_(&dialog) {
    connect(&dialog, &SettingsDialog::destinationChanged, this, [this] { wire(); });
    connect(&dialog, &SettingsDialog::windowShown, this, [this] { wire(); });
    if (QStackedWidget* stack = dialog.stack()) {
        connect(stack, &QStackedWidget::widgetAdded, this, [this] { wire(); });
    }
    wire();
}

void ActionBindingsWiring::wire() {
    if (dialog_ == nullptr) {
        return;
    }
    const std::string key(destinations::hotkeys);
    if (!dialog_->isRegistered(key) || !dialog_->isBuilt(key)) {
        return;
    }
    auto* page = qobject_cast<HotkeysPage*>(dialog_->pageContent(key));
    if (page == nullptr || page == registered_.data()) {
        return;
    }
    registered_ = page;
    // _on_binding_shortcut_saved (settings_dialog.py:2818-2830) for an action: the Actions page
    // reads again, then notify_changed("actions").
    QPointer<SettingsDialog> dialog = dialog_;
    registerActionBindingSource(*page, [dialog] {
        if (dialog == nullptr) {
            return;
        }
        dialog->refreshPage(destinations::actions);
        dialog->notifyChanged(QStringLiteral("actions"));
    });
    if (!dialog_->isLoading()) {
        page->loadSettings(*dialog_->settingsSnapshot());
    }
}

} // namespace fv
