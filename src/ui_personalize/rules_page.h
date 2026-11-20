#pragma once

#include <string_view>

#include <QString>

#include "base/json.h"
#include "ui_personalize/personalize_page.h"

namespace fv {

class AppContext;
class Button;
class CleanupRulesSection;
class CleanupSettings;
class InfoTile;

// Settings > Personalize > Learned rules (settings_dialog.py _build_cleanup_rules_page and
// _rule_transcribe_detail): the "AI cleanup is off" gate, then the composer and library
// (CleanupRulesSection). Teaching and editing rules waits while AI cleanup is off; the gate's
// button turns it on at its saved level. The rules are re-read whenever the page is shown,
// because Dictionary moves spelling rules out of them while it is hidden. Closing Settings
// throws away a take still recording.
// Affinity: GUI thread.
class RulesPage : public PersonalizePage {
    Q_OBJECT
public:
    RulesPage(PersonalizeHost& host, SettingsPage& container, AppContext& context,
              CleanupSettings& cleanup);
    ~RulesPage() override;

    // "No rules", "1 rule", "<n> rules".
    static QString railValue(std::size_t ruleCount);

    CleanupRulesSection* section() const { return section_; }
    InfoTile* gateTile() const { return gateTile_; }
    Button* turnOnButton() const { return turnOnButton_; }

    // Whether AI cleanup is on: the AI cleanup page's switch when it is built in this window,
    // else the saved setting (the page is not built to answer).
    bool cleanupEnabled() const;
    // Where the dictation engine runs, named as the rail names it: "Sent to <server · model>"
    // for a cloud server, the remote host's line for a paired computer, else "On this computer ·
    // <model>".
    QString transcribeDetail() const;
    // _update_cleanup_prompt_ui's part on this page.
    void updateCleanupUi();

protected:
    void load(const Json& settings) override;

private:
    AppContext& context_;
    CleanupSettings& cleanup_;
    InfoTile* gateTile_ = nullptr;
    Button* turnOnButton_ = nullptr;
    CleanupRulesSection* section_ = nullptr;
};

// Registers Learned rules in the context's Settings pages: title, subtitle, search fields,
// controls and the rail's rule count.
// Affinity: UI actor.
void registerRulesPage(AppContext& context);

} // namespace fv
