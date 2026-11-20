#include "ui_settings/settings_page.h"

#include <algorithm>
#include <utility>

#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QLayoutItem>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "ui_settings/settings_fields.h"
#include "ui_theme/font_scale.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

SettingsPage::SettingsPage(SettingsDialog* dialog, QWidget* parent)
    : QWidget(parent), dialog_(dialog) {
    // Lets the viewport supply the width before the grids choose their columns.
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
}

QVBoxLayout* SettingsPage::pageLayout() const {
    return qobject_cast<QVBoxLayout*>(layout());
}

void SettingsPage::addTileGrid(QGridLayout* grid, std::vector<QWidget*> tiles, int maxColumns) {
    Grid entry;
    entry.grid = grid;
    entry.maxColumns = maxColumns;
    for (QWidget* tile : tiles) {
        entry.tiles.emplace_back(tile);
    }
    grids_.push_back(std::move(entry));
    reflowTiles();
}

SettingsPage::TileGroup SettingsPage::addTileGroup(QVBoxLayout* layout, const QString& title,
                                                   std::vector<QWidget*> tiles, int columns,
                                                   const QString& intro) {
    TileGroup group;
    if (!title.isEmpty()) {
        group.caption = groupTitle(layout, title);
    }
    if (!intro.isEmpty()) {
        group.intro = settingsCaption(intro);
        layout->addWidget(group.intro);
    }
    auto* grid = new QGridLayout;
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(12);
    grid->setVerticalSpacing(12);
    layout->addLayout(grid);
    group.grid = grid;
    addTileGrid(grid, std::move(tiles), columns);
    layout->addSpacing(6);
    return group;
}

std::vector<int> SettingsPage::tileColumns() const {
    std::vector<int> columns;
    for (const Grid& entry : grids_) {
        columns.push_back(entry.columns);
    }
    return columns;
}

void SettingsPage::reflowTiles() {
    const int minimumTileWidth = ui::scaled(kMinimumTileWidth);
    for (Grid& entry : grids_) {
        QGridLayout* grid = entry.grid;
        if (grid == nullptr) {
            continue;
        }
        const int spacing = grid->horizontalSpacing();
        const int fit = (width() + spacing) / (minimumTileWidth + spacing);
        const int columns = std::min(entry.maxColumns, std::max(1, fit));
        if (columns == entry.columns) {
            continue;
        }
        while (grid->count() > 0) {
            delete grid->takeAt(0);
        }
        for (int column = 0; column < entry.maxColumns; ++column) {
            grid->setColumnStretch(column, column < columns ? 1 : 0);
        }
        std::vector<QWidget*> tiles;
        for (const QPointer<QWidget>& tile : entry.tiles) {
            if (tile != nullptr) {
                tiles.push_back(tile);
            }
        }
        for (std::size_t index = 0; index < tiles.size(); ++index) {
            const int row = static_cast<int>(index) / columns;
            const int column = static_cast<int>(index) % columns;
            const int span = index + 1 == tiles.size() ? columns - column : 1;
            grid->addWidget(tiles[index], row, column, 1, span);
        }
        entry.columns = columns;
    }
}

void SettingsPage::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflowTiles();
}

void SettingsPage::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        reflowTiles();
    }
}

} // namespace fv
