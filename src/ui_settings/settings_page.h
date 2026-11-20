#pragma once

#include <vector>

#include <QPointer>
#include <QString>
#include <QWidget>

class QGridLayout;
class QLabel;
class QVBoxLayout;

namespace fv {

class SettingsDialog;
class WrappedLabel;

// One Settings page (settings_dialog.py _SettingsPage, object name "settingsPage_<key>"): the
// container the dialog builds for each registered destination, inside the shared scroll area
// unless the page scrolls itself. A page's SettingsPageSpec::create gets it as the parent and
// either fills its layout and returns it, or returns a content widget the dialog adds to it.
//
// Tile grids reflow with the page's width: columns = min(max, max(1, (width + spacing) //
// (300 * scale + spacing))), the last tile spanning what is left of its row so no column stays
// empty; again on resize and on a font or style change.
// Affinity: GUI thread.
class SettingsPage : public QWidget {
    Q_OBJECT
public:
    // The width a tile needs before the grid adds a column, at 100 % text.
    static constexpr int kMinimumTileWidth = 300;

    explicit SettingsPage(SettingsDialog* dialog = nullptr, QWidget* parent = nullptr);

    // The dialog the page belongs to (null for a page built on its own).
    SettingsDialog* dialog() const { return dialog_; }
    // The page's own column (margins 0, spacing 8), which the dialog creates.
    QVBoxLayout* pageLayout() const;

    // Lays tiles out in grid, reflowed with the page's width (add_tile_grid).
    void addTileGrid(QGridLayout* grid, std::vector<QWidget*> tiles, int maxColumns);

    struct TileGroup {
        QLabel* caption = nullptr;     // null without a title
        WrappedLabel* intro = nullptr; // null without an intro
        QGridLayout* grid = nullptr;
    };
    // _tile_group: a caption, an optional intro line and a grid of tiles (12 px apart) added to
    // layout, then 6 px of space. A trailing odd tile spans the rest of its row. Returns the
    // caption and intro for callers that gate them.
    TileGroup addTileGroup(QVBoxLayout* layout, const QString& title, std::vector<QWidget*> tiles,
                           int columns = 2, const QString& intro = {});

    // The grids' current column counts, in the order they were added.
    std::vector<int> tileColumns() const;

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    struct Grid {
        QPointer<QGridLayout> grid;
        std::vector<QPointer<QWidget>> tiles;
        int maxColumns = 1;
        int columns = 0;
    };

    void reflowTiles();

    SettingsDialog* dialog_;
    std::vector<Grid> grids_;
};

// What a built page can be deep-linked to beyond its named controls: the API keys page's
// credential (focus_api_keys) and the Downloads page's engine filter and component
// (show_downloads). The dialog finds it with dynamic_cast on the widget the page's create
// returned, after selecting the page. Every hook has a default that does nothing.
// Affinity: GUI thread.
class SettingsDeepLinks {
public:
    virtual ~SettingsDeepLinks() = default;

    // Selects the credential whose environment variable is env ("" keeps the current one).
    virtual void focusCredential(const QString& env) { (void)env; }
    // Filters the catalog to one speech backend.
    virtual void showBackend(const QString& backend) { (void)backend; }
    // Scrolls to and focuses one optional component.
    virtual void focusComponent(const QString& componentId) { (void)componentId; }
    // Shows and selects one speech model's row, clearing the filters that hide it (reveal_model,
    // a search result's model).
    virtual void revealModel(const QString& modelName) { (void)modelName; }

protected:
    SettingsDeepLinks() = default;
    SettingsDeepLinks(const SettingsDeepLinks&) = default;
    SettingsDeepLinks& operator=(const SettingsDeepLinks&) = default;
};

} // namespace fv
