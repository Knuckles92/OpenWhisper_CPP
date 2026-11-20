#pragma once

#include <functional>
#include <string>
#include <vector>

#include <QObject>
#include <QPointer>

#include "base/json.h"
#include "ui_hotkeys/hotkey_bindings.h"
#include "ui_hotkeys/hotkeys_page.h"

namespace fv {

class SettingsDialog;

// The Hotkeys page's Actions group (ui_qt/widgets/hotkey_bindings.py bindings() and _STORE for
// ACTION): every saved action in saved order, the agent action included, with its one-line
// summary and whether it is on. A row saves through automation::save_action, so a shortcut is
// refused with the reason the action editor gives.
// Affinity: GUI thread; save() runs inside SettingsStore::mutate.
class ActionBindingSource final : public BindingSource {
public:
    // `saved` runs after a row saved a shortcut (Settings' _on_binding_shortcut_saved for
    // actions: the Actions page reads again, then notify_changed("actions")).
    explicit ActionBindingSource(std::function<void()> saved = {});

    std::vector<hotkey_bindings::Binding> list(const Json& settings) const override;
    bool save(Json& settings, const std::string& id, const std::string& hotkey) override;
    void saved() override;

private:
    std::function<void()> saved_;
};

// Registers the action source on `page` (HotkeysPage::registerBindingSource("action", ...)),
// with `saved` as its hook; its rows show on the page's next load.
void registerActionBindingSource(HotkeysPage& page, std::function<void()> saved = {});

// Registers the action source on `dialog`'s Hotkeys page once the page is built (it is built on
// first use), then loads the page again, unless the window is loading it anyway. A shortcut saved
// through a row refreshes the Actions page and says "actions" changed.
// Affinity: GUI thread.
class ActionBindingsWiring : public QObject {
    Q_OBJECT
public:
    explicit ActionBindingsWiring(SettingsDialog& dialog);

    // The page the source was registered on, null before.
    HotkeysPage* registeredPage() const { return registered_; }

private:
    void wire();

    QPointer<SettingsDialog> dialog_;
    QPointer<HotkeysPage> registered_;
};

} // namespace fv
