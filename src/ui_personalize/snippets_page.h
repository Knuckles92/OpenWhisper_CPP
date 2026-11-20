#pragma once

#include "base/json.h"
#include "ui_personalize/personalize_page.h"

namespace fv {

class AppContext;
class HomePage;
class InfoTile;
class SettingTile;
class SettingsStore;
class SettingsSwitch;
class SnippetsPanel;

// Settings > Personalize > Snippets (ui_qt/dialogs/settings_snippets.py): the "Expand snippets
// as you dictate" switch over the library and editor (SnippetsPanel). The switch saves at once
// and, when the save fails, snaps back to what is still saved; nothing is saved while the window
// loads. Leaving the page saves an unsaved draft (saveDrafts).
// Affinity: GUI thread.
class SnippetsPage : public PersonalizePage {
    Q_OBJECT
public:
    // `store` is the settings file the panel edits snippets in (OpenWhisper's settings manager).
    SnippetsPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store);
    ~SnippetsPage() override;

    SettingTile* enabledTile() const { return enabledTile_; }
    SettingsSwitch* enabledCheck() const;
    InfoTile* libraryTile() const { return libraryTile_; }
    SnippetsPanel* panel() const { return panel_; }

    // SettingsPageModule.
    void refreshPage() override;
    bool saveDrafts(bool ask) override;

protected:
    void load(const Json& settings) override;
    // OpenWhisper's page is read only when the window loads or refreshes it: a re-read on every
    // show would move a new snippet's untouched draft back to the first snippet.
    void sync(const Json& settings) override { (void)settings; }

private:
    void enabledToggled(bool checked);

    SettingTile* enabledTile_ = nullptr;
    InfoTile* libraryTile_ = nullptr;
    SnippetsPanel* panel_ = nullptr;
};

// Registers Snippets in the context's Settings pages: title, subtitle, search fields, controls
// and the rail's "Off", count or "None yet". Its panel edits the context's settings file.
// Affinity: UI actor.
void registerSnippetsPage(AppContext& context);

// Home's Personalize row for snippets (home_rows): the count, or "Off", and "Add", which opens a
// new snippet on the page.
// Affinity: GUI thread.
void addSnippetsHomeRows(HomePage& home);

} // namespace fv
