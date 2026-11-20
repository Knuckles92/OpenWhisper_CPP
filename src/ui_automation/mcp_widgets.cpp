#include "ui_automation/mcp_widgets.h"

#include <algorithm>

#include <QBoxLayout>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPointer>
#include <QToolButton>
#include <QVBoxLayout>

#include "ui_theme/font_scale.h"
#include "ui_theme/icons.h"
#include "ui_widgets/setting_tile.h"
#include "ui_widgets/settings_switch.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {

// ---- McpActionButton ----

McpActionButton::McpActionButton(const QString& text, QWidget* parent) : Button(text, parent) {
    fitHeight();
}

void McpActionButton::setText(const QString& text) {
    Button::setText(text);
    fitHeight();
}

void McpActionButton::setBaseMinimumSize(int width, int height) {
    baseMinWidth_ = width;
    baseMinHeight_ = height;
    Button::setBaseMinimumSize(width, height);
    fitHeight();
}

void McpActionButton::changeEvent(QEvent* event) {
    Button::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        refit();
    }
}

void McpActionButton::refit() {
    Button::setBaseMinimumSize(baseMinWidth_, baseMinHeight_);
    fitHeight();
}

void McpActionButton::fitHeight() {
    setMinimumHeight(std::max(baseMinHeight_, sizeHint().height()));
}

// ---- McpAdaptiveRow ----

McpAdaptiveRow::McpAdaptiveRow(const std::vector<Item>& items, int spacing, QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("mcpAdaptiveRow"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    layout_ = new QBoxLayout(QBoxLayout::LeftToRight, this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(spacing);
    for (const Item& item : items) {
        if (item.widget == nullptr) {
            layout_->addStretch(1);
            stretches_.push_back(1);
        } else {
            widgets_.push_back(item.widget);
            layout_->addWidget(item.widget, item.stretch);
            stretches_.push_back(item.stretch);
        }
    }
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { reflow(); });
}

void McpAdaptiveRow::reflow() {
    int required = 0;
    int shown = 0;
    for (QWidget* widget : widgets_) {
        if (!widget->isHidden()) {
            required += widget->sizeHint().width();
            ++shown;
        }
    }
    required += layout_->spacing() * std::max(0, shown - 1);
    const bool narrow = width() < required;
    const auto direction = narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight;
    if (layout_->direction() != direction) {
        layout_->setDirection(direction);
    }
    for (std::size_t index = 0; index < stretches_.size(); ++index) {
        layout_->setStretch(static_cast<int>(index), narrow ? 0 : stretches_[index]);
    }
}

void McpAdaptiveRow::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

void McpAdaptiveRow::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (timer_ != nullptr &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
        timer_->start(0);
    }
}

// ---- McpDisclosure ----

McpDisclosure::McpDisclosure(const QString& title, bool inlineToggle, QWidget* trailing,
                             QWidget* parent)
    : QWidget(parent), inline_(inlineToggle) {
    setObjectName(QStringLiteral("mcpDisclosure"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    // An inline toggle is placed by the caller in a row of its own, which takes it over; until
    // then it belongs to this section (hidden with it), so it is deleted with it.
    toggle_ = new QToolButton(this);
    toggle_->setObjectName(QStringLiteral("mcpDisclosureToggle"));
    toggle_->setText(title);
    toggle_->setAccessibleName(title);
    toggle_->setCheckable(true);
    toggle_->setArrowType(Qt::RightArrow);
    toggle_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    toggle_->setSizePolicy(inlineToggle || trailing != nullptr ? QSizePolicy::Preferred
                                                               : QSizePolicy::Expanding,
                           QSizePolicy::Preferred);
    toggle_->setCursor(Qt::PointingHandCursor);
    if (trailing != nullptr) {
        layout->addWidget(new McpAdaptiveRow({{toggle_, 0}, {nullptr, 0}, {trailing, 0}}));
    } else if (!inlineToggle) {
        layout->addWidget(toggle_);
    }
    body_ = new QWidget;
    body_->setObjectName(QStringLiteral("mcpDisclosureBody"));
    bodyLayout_ = new QVBoxLayout(body_);
    bodyLayout_->setContentsMargins(0, 0, 0, 0);
    bodyLayout_->setSpacing(10);
    layout->addWidget(body_);
    body_->hide();
    if (inlineToggle) {
        hide();
    }
    connect(toggle_, &QToolButton::toggled, this, &McpDisclosure::expand);
}

void McpDisclosure::expand(bool expanded) {
    toggle_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
    body_->setVisible(expanded);
    if (inline_) {
        setVisible(expanded);
    }
}

// ---- McpCopyField ----

McpCopyField::McpCopyField(QWidget* field, QWidget* button, QWidget* parent)
    : QWidget(parent), field_(field), button_(button) {
    setObjectName(QStringLiteral("mcpCopyField"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    field->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 0, 0, 0);
    layout_->setSpacing(8);
    layout_->addWidget(field, 1);
    layout_->addWidget(button);
    timer_ = new QTimer(this);
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, [this] { reflow(); });
}

void McpCopyField::reflow() {
    field_->setMinimumHeight(field_->sizeHint().height());
    const bool narrow = width() < ui::scaled(180) + button_->sizeHint().width() + 8;
    layout_->setDirection(narrow ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    layout_->setStretch(0, narrow ? 0 : 1);
}

void McpCopyField::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

void McpCopyField::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if (timer_ != nullptr &&
        (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)) {
        timer_->start(0);
    }
}

// ---- McpPermissionRow ----

McpPermissionRow::McpPermissionRow(const QString& title, const QString& description,
                                   const QString& icon, QWidget* parent)
    : QFrame(parent) {
    setObjectName(QStringLiteral("mcpPermissionRow"));
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(0, 8, 0, 8);
    layout_->setSpacing(12);
    auto* mark = new QLabel;
    mark->setObjectName(QStringLiteral("settingsTileIcon"));
    mark->setPixmap(ui::designIcon(icon).pixmap(20, 20));
    layout_->addWidget(mark, 0, Qt::AlignTop);
    auto* text = new QVBoxLayout;
    text->setSpacing(2);
    text->addWidget(mcpLabel(title, "settingsTileTitle"));
    text->addWidget(mcpLabel(description, "settingsTileDescription"));
    layout_->addLayout(text, 1);
    checkbox_ = new SettingsSwitch;
    checkbox_->setAccessibleName(title);
    checkbox_->setAccessibleDescription(description);
    layout_->addWidget(checkbox_);
    setCursor(Qt::PointingHandCursor);
}

void McpPermissionRow::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && checkbox_->isEnabled()) {
        checkbox_->toggle();
        event->accept();
    } else {
        QFrame::mousePressEvent(event);
    }
}

// ---- helpers ----

WrappedLabel* mcpLabel(const QString& text, const char* objectName) {
    auto* label = new WrappedLabel(text);
    label->setObjectName(QString::fromLatin1(objectName));
    return label;
}

WrappedLabel* mcpDetail(const QString& text) {
    return mcpLabel(text, "settingsTileDescription");
}

McpCard makeMcpCard(const QString& title, const QString& description, const QString& icon) {
    McpCard made;
    made.frame = new QFrame;
    made.frame->setObjectName(QStringLiteral("mcpCard"));
    made.frame->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    made.layout = new QVBoxLayout(made.frame);
    made.layout->setContentsMargins(20, 18, 20, 18);
    made.layout->setSpacing(12);
    // The standard Settings card heading: a bare icon beside a small bold title, as on every
    // other page's tiles.
    made.header = new QHBoxLayout;
    made.header->setSpacing(12);
    auto* glyph = new QLabel;
    glyph->setObjectName(QStringLiteral("settingsTileIcon"));
    glyph->setFixedSize(TileBase::kIconSize, TileBase::kIconSize);
    glyph->setPixmap(ui::designIcon(icon).pixmap(TileBase::kIconSize, TileBase::kIconSize));
    made.header->addWidget(glyph, 0, Qt::AlignTop);
    auto* titles = new QVBoxLayout;
    titles->setSpacing(3);
    titles->addWidget(mcpLabel(title, "settingsTileTitle"));
    titles->addWidget(mcpDetail(description));
    made.header->addLayout(titles, 1);
    made.layout->addLayout(made.header);
    return made;
}

} // namespace fv
