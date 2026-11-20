#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QAction>
#include <QKeySequence>
#include <QObject>
#include <QPointer>
#include <QString>

#include "base/json.h"

class QMenu;
class QWidget;

namespace fv {

// A main-window tab or context side panel. Created lazily on first show, as OpenWhisper's
// placeholder tabs; `order` comes from appkit/order.h (order::tab, order::side_panel).
// A screen with a `key` can be created through ScreenRegistry::ensure and receive queued
// updates; screens of one `group` take the same updates (Quick Record and Upload File, as
// MainWindow.update_transcription_tabs).
struct ScreenSpec {
    int order;
    QString title;
    std::function<QWidget*(QWidget* parent)> create;
    std::string key = {};
    std::string group = {};
};

// MainWindow's _ensure_screen and update_screen: screens are built on first use, and the latest
// value of each setter call made before then is kept and replayed when the screen is built.
// Updates are kept per method (per method and `coalesce` value when given, as
// set_model_downloading is per model), in the order of each one's latest call: a repeated call
// moves to the end. On creation a screen first takes its groups' updates, then its own, then
// screenCreated is emitted.
// Affinity: UI actor.
class ScreenRegistry : public QObject {
    Q_OBJECT

public:
    using Factory = std::function<QWidget*(QWidget* parent)>;
    using Apply = std::function<void(QWidget*)>;
    using MergedApply = std::function<void(QWidget*, const Json&)>;
    // Runs between a screen's creation and the replay of its queued updates: where the main
    // window swaps the placeholder for the screen and connects its signals.
    using Placer = std::function<void(std::string_view key, QWidget* screen)>;

    explicit ScreenRegistry(QObject* parent = nullptr);
    ~ScreenRegistry() override;

    // Keys are unique across tabs and side panels; a second spec with a used key throws
    // std::invalid_argument.
    void addScreen(ScreenSpec);
    void addSidePanel(ScreenSpec);
    std::vector<ScreenSpec> screens() const;    // by order, then registration
    std::vector<ScreenSpec> sidePanels() const; // by order, then registration

    // A keyed tab: addScreen({order, title, factory, key, group}).
    void define(std::string key, int order, QString title, Factory, std::string group = {});
    bool isDefined(std::string_view key) const;
    void setPlacer(Placer);

    // Creates the screen once, under parent, and replays the queued calls. Throws
    // std::invalid_argument for an undefined key and std::logic_error when the factory returns
    // no widget. A screen deleted from outside is created again by the next ensure.
    QWidget* ensure(std::string_view key, QWidget* parent);
    QWidget* existing(std::string_view key) const; // null until ensured

    // Keeps the call for replay and applies it now when the screen exists. A key may be updated
    // before it is defined.
    void update(std::string_view key, std::string method, Apply apply, std::string coalesce = {});
    // A dict update (MainWindow's set_meeting_state): patch is merged over the previously kept
    // value as Python's {**previous, **patch}, and the screen is called with the merged object.
    // Throws std::invalid_argument when patch is not an object.
    void updateMerged(std::string_view key, std::string method, const Json& patch,
                      MergedApply apply);
    // The merged value kept for (key, method); null when none (meeting_is_active reads it).
    const Json* mergedState(std::string_view key, std::string_view method) const;

    // MainWindow.update_transcription_tabs: kept for the group, applied to its existing screens.
    void updateGroup(std::string_view group, std::string method, Apply apply);
    // The transcript-collapse sync (main_window.py:1236): kept without moving the method in the
    // replay order, and applied to every existing screen of the group but source.
    void syncGroup(std::string_view group, std::string method, Apply apply, const QWidget* source);
    // The group's existing screens, by order (existing_transcription_tabs).
    std::vector<QWidget*> existingInGroup(std::string_view group) const;

Q_SIGNALS:
    void screenCreated(const QString& key, QWidget* screen);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

enum class Menu { File, View, Help, Tray };

// One item of a title-bar menu or the tray menu. `create` builds the action (it may keep it to
// change its text, check state or visibility later); an item without `create` is a separator.
// `order` comes from appkit/order.h. A non-empty `id` makes the last action built for the item
// reachable through MenuRegistry::action.
struct MenuItemSpec {
    Menu menu;
    int order;
    std::function<QAction*(QObject* parent)> create;
    std::string id = {};
};

// An item whose action the registry builds at once (QMenu.addAction(text, slot) with the
// setters OpenWhisper calls after it). Features connect the returned action's triggered().
// QAction's default role is TextHeuristicRole; NoRole is what OpenWhisper sets on the items
// whose label Qt would otherwise rewrite, and is harmless on the others outside macOS.
struct MenuItem {
    std::string id;
    Menu menu;
    int order;
    QString text;
    QKeySequence shortcut = {};
    QAction::MenuRole role = QAction::NoRole;
    bool checkable = false;
    bool separatorBefore = false;
};

// Affinity: UI actor.
class MenuRegistry : public QObject {
    Q_OBJECT

public:
    explicit MenuRegistry(QObject* parent = nullptr);
    ~MenuRegistry() override;

    // Ids are unique; a used id throws std::invalid_argument.
    void addItem(MenuItemSpec);
    void addSeparator(Menu, int order);
    std::vector<MenuItemSpec> items(Menu) const; // by order, then registration

    // Builds the action now, parented to the registry, and registers it.
    QAction* add(const MenuItem&);
    QAction* action(std::string_view id) const; // null when none was built

    // Appends the menu's items to `menu`, by order then registration; separators become
    // QMenu separators. An item whose create returns null is left out.
    void populate(Menu, QMenu* menu);

private:
    std::vector<MenuItemSpec> items_;
    std::map<std::string, QPointer<QAction>, std::less<>> actions_;
};

// A tile or field of a Settings page that search finds before the page is built
// (settings_metadata.PAGE_SEARCH_FIELDS): the control's name (SettingsPageModule::namedControl),
// its title and the description search also matches.
struct SettingsSearchField {
    std::string target;
    QString label;
    QString description;
};

// One Settings page, as OpenWhisper's _add_page(key, title, subtitle, builder, scroll=True). The
// page is built on first selection. `destination` is a key of appkit/destinations.h.
struct SettingsPageSpec {
    std::string_view destination;
    QString title;
    QString subtitle;
    std::function<QWidget*(QWidget* parent)> create;
    bool scroll = true; // inside the shared QScrollArea#settingsPageScroll
    // The page's one-line value under its rail name, from a settings snapshot; unset shows none.
    // The shell catches what it throws, so one page's bad value cannot break the rail.
    std::function<QString(const Json&)> railValue = {};
    // What search lists for the page while it is unbuilt (PAGE_SEARCH_FIELDS) and its help notes
    // (PAGE_HELP_TEXT).
    std::vector<SettingsSearchField> searchFields = {};
    std::vector<QString> helpTexts = {};
    // The names of the controls the page exposes (CONTROL_DESTINATIONS): asking the shell for one
    // builds this page.
    std::vector<std::string> controls = {};
};

// Affinity: UI actor.
class SettingsPageRegistry {
public:
    SettingsPageRegistry();
    ~SettingsPageRegistry();
    SettingsPageRegistry(const SettingsPageRegistry&) = delete;
    SettingsPageRegistry& operator=(const SettingsPageRegistry&) = delete;

    // The destination is resolved through destinations::resolve_destination and copied, so
    // the spec's view need not outlive the call. An empty or already registered destination
    // throws std::invalid_argument.
    void addPage(SettingsPageSpec);
    // Accepts a destination key or a legacy alias. The returned spec's destination views the
    // registry's own copy of the key.
    const SettingsPageSpec* page(std::string_view destination) const; // null when none
    // In the rail order of destinations.h (Home, then each chapter's pages), then pages the
    // rail does not list, in registration order.
    std::vector<const SettingsPageSpec*> pages() const;
    // The destination key of the page that lists `control` in its controls (the
    // CONTROL_DESTINATIONS lookup); "" when no registered page does. The last page registered
    // with the name wins, as merge_page_module's overwrite does.
    std::string_view controlDestination(std::string_view control) const;

private:
    struct Entry;
    std::vector<std::unique_ptr<Entry>> entries_;
};

} // namespace fv
