#include "ui_widgets/setting_tile.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "ui_theme/style_props.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

TileBase::TileBase(const QString& title, const QString& description, const QIcon& icon,
                   QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("settingsTile"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto* column = new QVBoxLayout(this);
    column->setContentsMargins(14, 12, 14, 12);
    column->setSpacing(10);

    row_ = new QHBoxLayout;
    row_->setContentsMargins(0, 0, 0, 0);
    row_->setSpacing(12);

    iconLabel_ = new QLabel;
    iconLabel_->setObjectName(QStringLiteral("settingsTileIcon"));
    iconLabel_->setFixedSize(kIconSize, kIconSize);
    if (!icon.isNull()) {
        iconLabel_->setPixmap(icon.pixmap(kIconSize, kIconSize));
    }
    row_->addWidget(iconLabel_, 0, Qt::AlignTop);

    // A widget boundary propagates wrapped-label size changes through the top-aligned row
    // without retaining a nested layout height.
    auto* textWidget = new QWidget;
    textWidget->setObjectName(QStringLiteral("settingsTileText"));
    auto* text = new QVBoxLayout(textWidget);
    text->setContentsMargins(0, 0, 0, 0);
    text->setSpacing(3);
    titleLabel_ = new WrappedLabel(title);
    titleLabel_->setObjectName(QStringLiteral("settingsTileTitle"));
    text->addWidget(titleLabel_);
    descriptionLabel_ = new WrappedLabel(description);
    descriptionLabel_->setObjectName(QStringLiteral("settingsTileDescription"));
    text->addWidget(descriptionLabel_);
    // The text is pinned to the top instead of followed by a stretch: an expanding spacer would
    // make the tile itself expanding, and the page layout would then pad every tile row with the
    // spare height.
    row_->addWidget(textWidget, 1, Qt::AlignTop);
    column->addLayout(row_);

    body_ = new QWidget;
    body_->setObjectName(QStringLiteral("settingsTileBody"));
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(kBodyIndent, 0, 0, 0);
    bodyLayout_->setSpacing(8);
    body_->hide();
    column->addWidget(body_);
}

void TileBase::setDescription(const QString& text) {
    descriptionLabel_->setText(text);
}

QLabel* TileBase::addTitleBadge(const QString& text, const QString& objectName) {
    auto* textLayout = static_cast<QBoxLayout*>(titleLabel_->parentWidget()->layout());
    const int index = textLayout->indexOf(titleLabel_);
    textLayout->removeWidget(titleLabel_);
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    row->addWidget(titleLabel_);
    auto* badge = new QLabel(text);
    badge->setObjectName(objectName);
    badge->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    row->addWidget(badge, 0, Qt::AlignVCenter);
    row->addStretch(1);
    textLayout->insertLayout(index, row);
    return badge;
}

void TileBase::addTrailing(QWidget* widget) {
    row_->addWidget(widget, 0, Qt::AlignVCenter);
}

void TileBase::addBody(QWidget* widget) {
    bodyLayout_->addWidget(widget);
    body_->show();
}

void TileBase::addBodyLayout(QLayout* layout) {
    bodyLayout_->addLayout(layout);
    body_->show();
}

void TileBase::setBodyIndented(bool indented) {
    bodyLayout_->setContentsMargins(indented ? kBodyIndent : 0, 0, 0, 0);
}

void TileBase::addActions(const std::vector<QWidget*>& buttons) {
    auto* row = new QHBoxLayout;
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    for (QWidget* button : buttons) {
        row->addWidget(button);
    }
    row->addStretch(1);
    addBodyLayout(row);
}

SettingTile::SettingTile(const QString& title, const QString& description, const QIcon& icon,
                         QWidget* parent)
    : TileBase(title, description, icon, parent) {
    ui::setStyleProperty(this, ui::prop::kind, QStringLiteral("toggle"));
    setCursor(Qt::PointingHandCursor);

    checkbox_ = new SettingsSwitch;
    checkbox_->setObjectName(QStringLiteral("settingsTileCheck"));
    checkbox_->setAccessibleName(title);
    checkbox_->setAccessibleDescription(description);
    connect(checkbox_, &SettingsSwitch::toggled, this,
            [this](bool checked) { ui::setStyleProperty(this, ui::prop::checked, checked); });
    // Top-aligned, so in a tall tile it stays beside the title.
    row()->addWidget(checkbox_, 0, Qt::AlignTop);
    ui::setStyleProperty(this, ui::prop::checked, false);
}

void SettingTile::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && isEnabled()) {
        checkbox_->toggle();
        event->accept();
        return;
    }
    TileBase::mousePressEvent(event);
}

void SettingTile::changeEvent(QEvent* event) {
    TileBase::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) {
        setCursor(isEnabled() ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
}

FieldTile::FieldTile(const QString& title, const QString& description, QWidget* control,
                     const QIcon& icon, bool compact, QWidget* parent)
    : TileBase(title, description, icon, parent), control_(control) {
    ui::setStyleProperty(this, ui::prop::kind, QStringLiteral("field"));
    if (compact) {
        addTrailing(control);
    } else {
        addBody(control);
    }
}

InfoTile::InfoTile(const QString& title, const QString& description, const QIcon& icon,
                   QWidget* parent)
    : TileBase(title, description, icon, parent) {
    ui::setStyleProperty(this, ui::prop::kind, QStringLiteral("field"));
}

} // namespace fv
