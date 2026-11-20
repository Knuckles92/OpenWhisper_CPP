#include "ui_automation/action_gallery.h"

#include <QEvent>
#include <QGridLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QTimer>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "pyport/number.h"
#include "ui_automation/action_fields.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

// Read by the Settings search palette, which leaves out a part marked false; not a style
// property.
constexpr const char* kSettingsSearch = "settingsSearch";

} // namespace

const automation::Preset& action_gallery::ownLink() {
    static const automation::Preset preset{
        .id = "own-link",
        .name = "Your own link",
        .kind = std::string(automation::ActionKind::OPEN_LINK),
        .params = Json{{"template", ""}},
        .description = "Open any web address or app link with your words in it.",
        .icon = "world-blue.svg",
    };
    return preset;
}

std::vector<automation::Preset> action_gallery::galleryPresets() {
    std::vector<automation::Preset> presets;
    for (const automation::Preset& preset : automation::PRESETS()) {
        if (preset.id != automation::AGENT_ACTION_ID) {
            presets.push_back(preset);
        }
    }
    std::size_t at = presets.size();
    for (std::size_t index = presets.size(); index > 0; --index) {
        if (presets[index - 1].kind == automation::ActionKind::OPEN_LINK) {
            at = index;
            break;
        }
    }
    presets.insert(presets.begin() + static_cast<std::ptrdiff_t>(at), ownLink());
    return presets;
}

// ---- PresetTile ----

PresetTile::PresetTile(const automation::Preset& preset, QWidget* parent)
    : TileBase(qt::toQString(preset.name), qt::toQString(preset.description),
               ui::designIcon(qt::toQString(preset.icon)), parent),
      preset_(preset) {
    ui::setStyleProperty(this, ui::prop::kind, QStringLiteral("toggle"));
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionPreset"));
    action_fields::tintIcon(this, qt::toQString(preset.icon));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(qt::toQString(preset.name));
    setAccessibleDescription(qt::toQString(preset.description));
    if (!preset.note.empty()) {
        WrappedLabel* note = action_fields::hintLabel(qt::toQString(preset.note));
        ui::setStyleProperty(note, ui::prop::tone, QStringLiteral("warning"));
        addBody(note);
    }
}

void PresetTile::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        event->accept();
        Q_EMIT clicked(preset_);
        return;
    }
    TileBase::mousePressEvent(event);
}

void PresetTile::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        event->accept();
        Q_EMIT clicked(preset_);
        return;
    }
    TileBase::keyPressEvent(event);
}

// ---- PresetGallery ----

PresetGallery::PresetGallery(QWidget* parent) : QWidget(parent) {
    ui::setStyleProperty(this, ui::prop::tileId, QStringLiteral("actionGallery"));
    // Starting points, not settings: Settings search leaves them out.
    ui::setStyleProperty(this, kSettingsSearch, false);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);
    auto* title = new QLabel(QStringLiteral("Choose a starting point"));
    title->setObjectName(QStringLiteral("actionSectionLabel"));
    cancel_ = action_fields::pageLink(QStringLiteral("Cancel"),
                                      QStringLiteral("Close without adding an action"));
    connect(cancel_, &QPushButton::clicked, this, &PresetGallery::cancelled);
    layout->addWidget(new Reflow(title, cancel_));
    gridBox_ = new QWidget;
    ui::setStyleProperty(gridBox_, ui::prop::tileId, QStringLiteral("actionRow"));
    grid_ = new QGridLayout(gridBox_);
    grid_->setContentsMargins(0, 0, 0, 0);
    grid_->setSpacing(10);
    layout->addWidget(gridBox_);
    for (const automation::Preset& preset : action_gallery::galleryPresets()) {
        auto* tile = new PresetTile(preset);
        connect(tile, &PresetTile::clicked, this, &PresetGallery::picked);
        tiles_.push_back(tile);
    }
    reflowTimer_ = new QTimer(this);
    reflowTimer_->setSingleShot(true);
    connect(reflowTimer_, &QTimer::timeout, this, &PresetGallery::arrange);
    arrange();
}

PresetTile* PresetGallery::tile(const QString& presetId) const {
    for (PresetTile* tile : tiles_) {
        if (qt::toQString(tile->preset().id) == presetId) {
            return tile;
        }
    }
    return nullptr;
}

void PresetGallery::arrange() {
    const int from = static_cast<int>(
        py::round_half_even(action_gallery::kTwoColumnsFrom * ui::currentUiFontScale()));
    const int columns = width() >= from ? 2 : 1;
    if (columns == columns_) {
        return;
    }
    columns_ = columns;
    for (PresetTile* tile : tiles_) {
        grid_->removeWidget(tile);
    }
    for (std::size_t index = 0; index < tiles_.size(); ++index) {
        const int at = static_cast<int>(index);
        grid_->addWidget(tiles_[index], at / columns, at % columns);
    }
    for (int column = 0; column < 2; ++column) {
        grid_->setColumnStretch(column, column < columns ? 1 : 0);
    }
}

void PresetGallery::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    arrange();
}

void PresetGallery::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        reflowTimer_ != nullptr) {
        columns_ = 0;
        reflowTimer_->start(0);
    }
}

} // namespace fv
