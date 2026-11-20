#pragma once

#include <functional>
#include <memory>
#include <optional>

#include <QString>

#include "base/json.h"
#include "ui_personalize/personalize_page.h"

class QPushButton;
class QObject;

namespace fv {

class AppContext;
class CommandShortcutField;
class Executor;
class HomePage;
class InfoTile;
class SettingTile;
class SettingsStore;
class TransformsPanel;
class WrappedLabel;

// Whether Command Mode and transforms can reach a model (text_rewrite.provider_readiness): true
// when a cleanup model and its key are set up, false when not, nullopt while unknown. The check
// reads the credential store, so it runs on a worker and its answer comes back on the GUI thread.
// Provided once for the app with ctx.provide<CommandReadiness>() by the Personalize feature;
// without a check (no credential store, as in tests) every answer is nullopt.
// Affinity: ask() and setCheck() on the GUI thread; the check runs on the worker.
class CommandReadiness {
public:
    using Check = std::function<std::optional<bool>(const Json& settings)>;

    // Where answers go: `owner`'s thread, while it lives. Held as a member of the owner (or of
    // a child of it), so it is destroyed on the owner's thread before the owner; a worker posts
    // only under its lock, so an answer never reaches a destroyed owner. The latest ask through
    // it wins: an older answer still on its way is dropped.
    class Receiver {
    public:
        explicit Receiver(QObject* owner);
        ~Receiver();
        Receiver(const Receiver&) = delete;
        Receiver& operator=(const Receiver&) = delete;

    private:
        friend class CommandReadiness;
        struct State;
        std::shared_ptr<State> state_;
    };

    CommandReadiness() = default;
    CommandReadiness(Check check, Executor* worker);

    // Replaces the check and the worker it runs on; an empty check answers nullopt.
    void setCheck(Check check, Executor* worker);

    // Asks for `settings`; `done` gets the answer on the receiver's owner's thread.
    void ask(const Json& settings, Receiver& receiver,
             std::function<void(std::optional<bool>)> done) const;

private:
    // Shared with asks still on the worker, so a check replaced meanwhile stays alive.
    std::shared_ptr<const Check> check_;
    Executor* worker_ = nullptr;
};

// Settings > Personalize > Commands (ui_qt/dialogs/settings_commands.py): the Command Mode
// shortcut and how it is used, writing new text when nothing is selected, the transforms library
// (TransformsPanel) and the profiles that have their own shortcut.
//
// The "Set up AI cleanup first" notice shows only when no cleanup model is ready (not while that
// is unknown), even with cleanup off, since Command Mode and transforms always use that model.
// Shortcuts and the cleanup provider change on other pages, so the page shows the settings again
// whenever it is shown. Leaving the page saves an unsaved transform (saveDrafts) and stops every
// shortcut capture on it (cancelCapture).
// Affinity: GUI thread.
class CommandsPage : public PersonalizePage {
    Q_OBJECT
public:
    // `store` is the settings file the transforms are edited in (OpenWhisper's settings manager).
    CommandsPage(PersonalizeHost& host, SettingsPage& container, SettingsStore& store,
                 const CommandReadiness& readiness);
    ~CommandsPage() override;

    // _profile_shortcuts: "<name> · <shortcut>" per profile with a shortcut, one per line, or
    // "No profile has a shortcut yet."
    static QString profileShortcuts(const Json& settings);

    InfoTile* gateTile() const { return gateTile_; }
    InfoTile* commandsTile() const { return commandsTile_; }
    CommandShortcutField* shortcut() const { return shortcut_; }
    WrappedLabel* modeHint() const { return modeHint_; }
    SettingTile* insertTile() const { return insertTile_; }
    TransformsPanel* transformsPanel() const { return panel_; }
    InfoTile* profilesTile() const { return profilesTile_; }
    WrappedLabel* profilesList() const { return profilesList_; }
    QPushButton* profilesLink() const { return profilesLink_; }

    // SettingsPageModule.
    void refreshPage() override;
    bool saveDrafts(bool ask) override;
    void cancelCapture() override;

protected:
    void load(const Json& settings) override;
    void sync(const Json& settings) override;

private:
    // _show_state.
    void showState(const Json& settings);
    void insertToggled(bool checked);

    const CommandReadiness& readiness_;
    CommandReadiness::Receiver readinessReceiver_{this};
    InfoTile* gateTile_ = nullptr;
    InfoTile* commandsTile_ = nullptr;
    CommandShortcutField* shortcut_ = nullptr;
    WrappedLabel* modeHint_ = nullptr;
    SettingTile* insertTile_ = nullptr;
    TransformsPanel* panel_ = nullptr;
    InfoTile* profilesTile_ = nullptr;
    WrappedLabel* profilesList_ = nullptr;
    QPushButton* profilesLink_ = nullptr;
};

// focus_cleanup_model through a page's host: AI cleanup, revealing its model tile.
void focusCleanupModel(PersonalizeHost& host);

// Registers Commands in the context's Settings pages: title, subtitle, search fields, controls
// and the rail's "<Shortcut set|No shortcut> · <n> transform(s)". Its transforms are edited in
// the context's settings file; its notice asks the context's CommandReadiness.
// Affinity: UI actor.
void registerCommandsPage(AppContext& context);

// Home's Personalize row for Command Mode (home_rows): the shortcut field with "Change" and no
// "Clear", its errors in the window's message line, and how to use it, or that it needs an AI
// cleanup model.
// Affinity: GUI thread.
void addCommandsHomeRows(HomePage& home);

} // namespace fv
