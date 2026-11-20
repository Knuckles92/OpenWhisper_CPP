#include "ui_hotkeys/personal_binding_sources.h"

#include <memory>
#include <utility>

#include "hotkeys/conflicts.h"
#include "text/cleanup_profiles.h"
#include "text/transforms.h"
#include "ui_hotkeys/hotkeys_page.h"

namespace fv {

namespace {

// What the shortcut checks of fv_text cannot load themselves: the saved actions.
text::ShortcutContext shortcutContext(const BindingSources* all, const Json& settings) {
    text::ShortcutContext context;
    if (all != nullptr) {
        context.actions = conflictBindings(*all, settings).actions;
    }
    return context;
}

} // namespace

// --- profiles ---

ProfileBindingSource::ProfileBindingSource(const BindingSources* all, BindingSavedHook saved)
    : all_(all), saved_(std::move(saved)) {}

std::vector<hotkey_bindings::Binding> ProfileBindingSource::list(const Json& settings) const {
    std::vector<hotkey_bindings::Binding> bindings;
    for (const text::CleanupProfile& profile : text::load_cleanup_profiles(settings)) {
        bindings.push_back({std::string(hotkeys::kProfile), profile.id, profile.name,
                            profile.hotkey, hotkey_bindings::clip(profile.instructions), true});
    }
    return bindings;
}

bool ProfileBindingSource::save(Json& settings, const std::string& id, const std::string& hotkey) {
    std::optional<text::CleanupProfile> profile = text::find_cleanup_profile(settings, id);
    if (!profile) {
        return false;
    }
    profile->hotkey = hotkey;
    text::save_cleanup_profile(settings, *profile, shortcutContext(all_, settings));
    return true;
}

void ProfileBindingSource::saved() {
    if (saved_) {
        saved_();
    }
}

// --- transforms ---

TransformBindingSource::TransformBindingSource(const BindingSources* all, BindingSavedHook saved)
    : all_(all), saved_(std::move(saved)) {}

std::vector<hotkey_bindings::Binding> TransformBindingSource::list(const Json& settings) const {
    std::vector<hotkey_bindings::Binding> bindings;
    for (const text::Transform& transform : text::load_transforms(settings)) {
        bindings.push_back({std::string(hotkeys::kTransform), transform.id, transform.name,
                            transform.hotkey, hotkey_bindings::clip(transform.instruction), true});
    }
    return bindings;
}

bool TransformBindingSource::save(Json& settings, const std::string& id,
                                  const std::string& hotkey) {
    std::optional<text::Transform> transform = text::find_transform(settings, id);
    if (!transform) {
        return false;
    }
    transform->hotkey = hotkey;
    text::save_transform(settings, *transform, shortcutContext(all_, settings));
    return true;
}

void TransformBindingSource::saved() {
    if (saved_) {
        saved_();
    }
}

void registerPersonalBindingSources(HotkeysPage& page, PersonalBindingHooks hooks) {
    const BindingSources* all = &page.bindingShortcuts()->sources();
    page.registerBindingSource(
        std::string(hotkeys::kProfile),
        std::make_shared<ProfileBindingSource>(all, std::move(hooks.profilesSaved)));
    page.registerBindingSource(
        std::string(hotkeys::kTransform),
        std::make_shared<TransformBindingSource>(all, std::move(hooks.transformsSaved)));
}

} // namespace fv
