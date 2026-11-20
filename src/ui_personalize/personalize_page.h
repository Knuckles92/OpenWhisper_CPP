#pragma once

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

#include "appkit/registries.h"
#include "appkit/settings_page.h"
#include "base/json.h"
#include "ui_settings/settings_page.h"

class QVBoxLayout;

namespace fv {

class AppContext;
class PersonalizeHost;
class ShowWatcher;

// The base of a Personalize page, in place of the module protocol of OpenWhisper's settings_*
// pages (TITLE, SUBTITLE, SEARCH_FIELDS, CONTROL_ATTRS, build, load, refresh, rail_value,
// save_drafts, cancel_capture): the page widget is what `build` made, `load` is loadSettings,
// `refresh` is refreshPage, `cancel_capture` and `save_drafts` are the SettingsPageModule hooks,
// and the static facts (title, subtitle, search fields, controls, rail value) are the
// PageDescriptor given to registerPersonalizePage.
//
// A page content widget the Settings window puts inside the shared scroll area, with its own
// column (margins 0, spacing 8). It never writes while loading: loadSettings and sync hold the
// loading flag, so what they do to the controls is not saved, and save() refuses meanwhile.
// It re-reads the settings every time it is shown again (ShowWatcher), because other pages
// change them while it is hidden.
// Affinity: GUI thread.
class PersonalizePage : public QWidget, public SettingsPageModule {
    Q_OBJECT
public:
    // `container` is the page the Settings window made for the destination (the parent); its
    // tile grids reflow with its width. `host` outlives the page.
    PersonalizePage(PersonalizeHost& host, SettingsPage& container);
    ~PersonalizePage() override;

    PersonalizeHost& host() const { return host_; }
    SettingsPage& container() const { return container_; }
    // The page's own column.
    QVBoxLayout* pageLayout() const { return layout_; }

    // _tile_group on the page's column: a caption, an optional intro line and a grid of tiles.
    SettingsPage::TileGroup addTileGroup(const QString& title, std::vector<QWidget*> tiles,
                                         int columns = 2, const QString& intro = {});

    // True while the page loads or re-syncs, while a LoadingScope is open, and while the window
    // loads its pages: a toggle or edit then is the page showing a value, not the user's choice.
    bool loading() const;

    // Holds the loading flag for as long as it lives, for a page that sets one of its own
    // controls (the way OpenWhisper's pages set dialog._loading around a revert).
    class LoadingScope {
    public:
        explicit LoadingScope(PersonalizePage& page) : page_(&page) { ++page.loadingDepth_; }
        ~LoadingScope() { --page_->loadingDepth_; }
        LoadingScope(const LoadingScope&) = delete;
        LoadingScope& operator=(const LoadingScope&) = delete;

    private:
        PersonalizePage* page_;
    };

    // host().persist and persistMany, refused (false) while loading.
    bool save(std::string_view key, const Json& value);
    bool saveMany(const Json& updates, std::vector<std::string> drops = {});

    // Exposes a control under the name the page's descriptor lists in `controls` (the
    // CONTROL_ATTRS names): SettingsDialog::control(name) and search results reach it.
    void exposeControl(std::string name, QWidget* control);

    // SettingsPageModule: loadSettings holds the loading flag around load(); a control a
    // search result or a deep link names comes from exposeControl.
    void loadSettings(const Json& settings) final;
    QWidget* namedControl(std::string_view name) override;

protected:
    // Shows the saved values (load). Runs with the loading flag set.
    virtual void load(const Json& settings) { (void)settings; }
    // Re-reads what other pages may have changed while this one was hidden. Defaults to load().
    // Runs with the loading flag set.
    virtual void sync(const Json& settings) { load(settings); }

private:
    void resync();

    PersonalizeHost& host_;
    SettingsPage& container_;
    QVBoxLayout* layout_;
    int loadingDepth_ = 0;
    std::map<std::string, QPointer<QWidget>, std::less<>> controls_;
};

// What registerPersonalizePage needs to know of a page besides its widget: the Python module's
// TITLE, SUBTITLE, SEARCH_FIELDS, CONTROL_ATTRS and rail_value.
struct PageDescriptor {
    // A key of appkit/destinations.h.
    std::string_view destination;
    QString title;
    QString subtitle;
    // Builds the page inside `container` (its parent), over the Settings window as `host`.
    std::function<PersonalizePage*(PersonalizeHost& host, SettingsPage& container)> create;
    // The one-line value under the page's rail name, from a settings snapshot.
    std::function<QString(const Json&)> railValue = {};
    // What search lists for the page while it is unbuilt, and its help notes.
    std::vector<SettingsSearchField> searchFields = {};
    std::vector<QString> helpTexts = {};
    // The names of the controls the page exposes.
    std::vector<std::string> controls = {};
    // Inside the shared scroll area; false for a page that scrolls itself.
    bool scroll = true;
};

// Registers the page in the context's Settings pages. The page is built on first selection over
// a DialogPersonalizeHost of the Settings window it is built in; building it without one
// (a container with no dialog) throws std::logic_error.
// Affinity: UI actor.
void registerPersonalizePage(AppContext& context, PageDescriptor descriptor);

} // namespace fv
