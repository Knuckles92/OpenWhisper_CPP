#pragma once

#include <QString>

#include "base/json.h"
#include "ui_personalize/personalize_page.h"

namespace fv {

class AppContext;
class CleanupProfilesPanel;
class SettingsStore;

// Settings > Personalize > Profiles (settings_dialog.py _build_cleanup_profiles_page): the
// cleanup profile library and editor (CleanupProfilesPanel). A saved or deleted profile redraws
// the rail and says "cleanup_profiles" changed (on_cleanup_profiles_changed: Quick Record and the
// profile shortcuts follow); capturing a shortcut pauses the app's hotkeys; "Choose cleanup
// model…" opens AI cleanup at its model. Leaving the page saves an unsaved draft (saveDrafts) and
// stops a capture (cancelCapture); the window's refresh re-reads the library, keeping a draft.
// Affinity: GUI thread.
class ProfilesPage : public PersonalizePage {
    Q_OBJECT
public:
    // `store` is the settings file the panel edits profiles in (OpenWhisper's settings manager).
    ProfilesPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store);
    ~ProfilesPage() override;

    // "<n> profile(s)".
    static QString railValue(const Json& settings);

    CleanupProfilesPanel* panel() const { return panel_; }

    // SettingsPageModule.
    void refreshPage() override;
    bool saveDrafts(bool ask) override;
    void cancelCapture() override;

protected:
    // OpenWhisper's panel is read only when it is built and when the window refreshes it: a
    // re-read on every show would move a new profile's untouched draft back to the first one.
    void sync(const Json& settings) override { (void)settings; }

private:
    CleanupProfilesPanel* panel_ = nullptr;
};

// Registers Profiles in the context's Settings pages: title, subtitle, its one control
// ("cleanup_profiles_panel") and the rail's profile count. Its panel edits the context's settings
// file.
// Affinity: UI actor.
void registerProfilesPage(AppContext& context);

} // namespace fv
