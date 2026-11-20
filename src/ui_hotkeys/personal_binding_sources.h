#pragma once

#include <functional>
#include <string>
#include <vector>

#include "base/json.h"
#include "ui_hotkeys/hotkey_bindings.h"

namespace fv {

class HotkeysPage;

// Runs after a row's shortcut was saved: what the kind's own page and the app need to hear
// (OpenWhisper's on_cleanup_profiles_changed, and notify_changed("transforms")).
using BindingSavedHook = std::function<void()>;

// The Hotkeys page's profile rows over fv_text (ui_qt/widgets/hotkey_bindings.py bindings() and
// _STORE): every profile in saved order, the starters while none were saved. A row saves through
// save_cleanup_profile, so a shortcut is refused with the reason the Profiles editor gives.
// `all` is the page's registered sources: the saved actions, which fv_text cannot load, count in
// the validation; without it they do not.
// Affinity: GUI thread; save() runs inside SettingsStore::mutate.
class ProfileBindingSource final : public BindingSource {
public:
    explicit ProfileBindingSource(const BindingSources* all = nullptr, BindingSavedHook saved = {});

    std::vector<hotkey_bindings::Binding> list(const Json& settings) const override;
    bool save(Json& settings, const std::string& id, const std::string& hotkey) override;
    void saved() override;

private:
    const BindingSources* all_;
    BindingSavedHook saved_;
};

// The same for transforms, over save_transform and the Commands editor's reasons.
// Affinity: GUI thread; save() runs inside SettingsStore::mutate.
class TransformBindingSource final : public BindingSource {
public:
    explicit TransformBindingSource(const BindingSources* all = nullptr,
                                    BindingSavedHook saved = {});

    std::vector<hotkey_bindings::Binding> list(const Json& settings) const override;
    bool save(Json& settings, const std::string& id, const std::string& hotkey) override;
    void saved() override;

private:
    const BindingSources* all_;
    BindingSavedHook saved_;
};

struct PersonalBindingHooks {
    BindingSavedHook profilesSaved;
    BindingSavedHook transformsSaved;
};

// Registers both sources on `page` (HotkeysPage::registerBindingSource); their rows show on the
// page's next load. They read the page's other registered sources when they validate a shortcut,
// so register the action source on the same page.
void registerPersonalBindingSources(HotkeysPage& page, PersonalBindingHooks hooks = {});

} // namespace fv
