#pragma once

#include <vector>

#include <QString>
#include <QWidget>

#include "automation/action_model.h"
#include "ui_widgets/setting_tile.h"

class QGridLayout;
class QPushButton;
class QTimer;

// Add action: a gallery of starting points, each a card you can click
// (ui_qt/widgets/action_gallery.py).
namespace fv {

namespace action_gallery {
// Tiles sit two to a row from this page width (at 100 % font scale).
inline constexpr int kTwoColumnsFrom = 600;
// A link of your own, for anything the named presets don't cover (OWN_LINK).
const automation::Preset& ownLink();
// The presets to offer: every preset but the agent hotkey, which has its own card above, with
// ownLink() after the last open-link preset (gallery_presets).
std::vector<automation::Preset> galleryPresets();
} // namespace action_gallery

// One starting point (tileId "actionPreset", kind "toggle"); a left click, Return, Enter or
// Space picks it. A preset's note shows under it in the warning tone. Affinity: GUI thread.
class PresetTile : public TileBase {
    Q_OBJECT
public:
    explicit PresetTile(const automation::Preset& preset, QWidget* parent = nullptr);

    const automation::Preset& preset() const { return preset_; }

Q_SIGNALS:
    void clicked(const automation::Preset& preset);

protected:
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    automation::Preset preset_;
};

// Choose what a new action starts as; the editor opens with it filled in (tileId
// "actionGallery"). Starting points, not settings: Settings search leaves them out.
// Affinity: GUI thread.
class PresetGallery : public QWidget {
    Q_OBJECT
public:
    explicit PresetGallery(QWidget* parent = nullptr);

    QPushButton* cancelLink() const { return cancel_; }
    QGridLayout* grid() const { return grid_; }
    const std::vector<PresetTile*>& tiles() const { return tiles_; }
    // The tile of `presetId`, or null.
    PresetTile* tile(const QString& presetId) const;
    int columns() const { return columns_; }

Q_SIGNALS:
    // chosen in Python: the preset the person picked.
    void picked(const automation::Preset& preset);
    void cancelled();

protected:
    void resizeEvent(QResizeEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    void arrange();

    QPushButton* cancel_;
    QWidget* gridBox_;
    QGridLayout* grid_;
    std::vector<PresetTile*> tiles_;
    int columns_ = 0;
    QTimer* reflowTimer_ = nullptr;
};

} // namespace fv
