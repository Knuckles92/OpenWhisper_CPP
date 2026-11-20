#include "ui_widgets/segmented_bar.h"

#include <algorithm>

#include <QButtonGroup>
#include <QEvent>
#include <QGridLayout>
#include <QLabel>
#include <QResizeEvent>
#include <QSizePolicy>
#include <QStyleOptionButton>
#include <QStylePainter>
#include <QTimer>
#include <QVBoxLayout>

#include "ui_theme/style_props.h"
#include "ui_widgets/layout_math.h"

namespace fv {

SegmentButton::SegmentButton(const QString& title, const QString& detail, bool compact,
                             QWidget* parent)
    : QPushButton(title, parent), compact_(compact) {
    setObjectName(QStringLiteral("mcpChoice"));
    setCheckable(true);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    setAccessibleDescription(detail);
    setSizePolicy(QSizePolicy::Minimum, QSizePolicy::Fixed);
    layout_ = new QVBoxLayout(this);
    if (compact) {
        layout_->setContentsMargins(12, 4, 12, 4);
    } else {
        layout_->setContentsMargins(14, 8, 14, 8);
    }
    layout_->setSpacing(2);
    if (!title.isEmpty()) {
        title_ = line(title, "mcpChoiceTitle");
    }
    if (!detail.isEmpty()) {
        detail_ = line(detail, "mcpChoiceDetail");
    }
    connect(this, &QPushButton::toggled, this, [this](bool checked) {
        if (QLayoutItem* first = layout_->itemAt(0); first != nullptr) {
            ui::setStyleProperty(first->widget(), ui::prop::selected, checked);
        }
    });
}

QLabel* SegmentButton::line(const QString& text, const char* objectName) {
    auto* label = new QLabel(text);
    label->setObjectName(QString::fromLatin1(objectName));
    label->setAlignment(Qt::AlignCenter);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    label->setWordWrap(wrapping_);
    layout_->addWidget(label);
    return label;
}

void SegmentButton::setTitle(const QString& text) {
    if (title_ != nullptr && title_->text() != text) {
        title_->setText(text);
        setAccessibleName(text);
        natural_.reset();
        updateGeometry();
    }
}

void SegmentButton::setDetail(const QString& text) {
    if (detail_ == nullptr) {
        detail_ = line(text, "mcpChoiceDetail");
    }
    detail_->setText(text);
    setAccessibleDescription(text);
    natural_.reset();
    updateGeometry();
}

int SegmentButton::naturalWidth() {
    if (!natural_) {
        const bool wrapping = wrapping_;
        setWrapping(false);
        natural_ = layout()->sizeHint().width();
        setWrapping(wrapping);
    }
    return *natural_;
}

void SegmentButton::changeEvent(QEvent* event) {
    QPushButton::changeEvent(event);
    if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) {
        natural_.reset();
    }
}

void SegmentButton::setWrapping(bool wrap) {
    if (wrap == wrapping_) {
        return;
    }
    wrapping_ = wrap;
    for (QLabel* label : {title_, detail_}) {
        if (label != nullptr) {
            label->setWordWrap(wrap);
        }
    }
    setSizePolicy(wrap ? QSizePolicy::Ignored : QSizePolicy::Minimum, QSizePolicy::Fixed);
    updateGeometry();
}

QSize SegmentButton::sizeHint() const {
    const int floor = compact_ ? 28 : 36;
    if (wrapping_ && width() > 0) {
        return {0, std::max(layout()->totalHeightForWidth(width()), floor)};
    }
    return layout()->sizeHint().expandedTo(QSize(0, floor));
}

QSize SegmentButton::minimumSizeHint() const {
    return sizeHint();
}

void SegmentButton::resizeEvent(QResizeEvent* event) {
    QPushButton::resizeEvent(event);
    if (wrapping_ && event->oldSize().width() != event->size().width()) {
        updateGeometry();
    }
}

void SegmentButton::paintEvent(QPaintEvent*) {
    QStyleOptionButton option;
    initStyleOption(&option);
    // The child labels show the title and the supporting text.
    option.text.clear();
    QStylePainter painter(this);
    painter.drawControl(QStyle::CE_PushButton, option);
}

SegmentedBar::SegmentedBar(const std::vector<std::pair<QString, QString>>& items, bool compact,
                           QWidget* parent)
    : QWidget(parent) {
    setObjectName(QStringLiteral("mcpChoiceBar"));
    setAttribute(Qt::WA_StyledBackground);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* layout = new QGridLayout(this);
    const int tray = compact ? 3 : 4;
    layout->setContentsMargins(tray, tray, tray, tray);
    layout->setSpacing(tray);
    group_ = new QButtonGroup(this);
    int index = 0;
    for (const auto& [title, detail] : items) {
        auto* button = new SegmentButton(title, detail, compact);
        group_->addButton(button, index++);
        buttons_.push_back(button);
    }
    place(static_cast<int>(buttons_.size()));
    reflowTimer_ = new QTimer(this);
    reflowTimer_->setSingleShot(true);
    connect(reflowTimer_, &QTimer::timeout, this, &SegmentedBar::reflow);
    connect(group_, &QButtonGroup::idClicked, this, &SegmentedBar::activated);
    connect(group_, &QButtonGroup::idClicked, this, &SegmentedBar::setCurrentIndex);
    setCurrentIndex(0);
}

void SegmentedBar::setDetail(int index, const QString& text) {
    SegmentButton* button = buttons_.at(static_cast<std::size_t>(index));
    if (button->detailLabel() == nullptr || button->detailLabel()->text() != text) {
        button->setDetail(text);
        reflowTimer_->start(0);
    }
}

void SegmentedBar::setTitle(int index, const QString& text) {
    buttons_.at(static_cast<std::size_t>(index))->setTitle(text);
    reflowTimer_->start(0);
}

void SegmentedBar::place(int columns) {
    if (columns == columns_) {
        return;
    }
    columns_ = columns;
    auto* grid = static_cast<QGridLayout*>(layout());
    for (SegmentButton* button : buttons_) {
        grid->removeWidget(button);
    }
    const int count = static_cast<int>(buttons_.size());
    for (int index = 0; index < count; ++index) {
        grid->addWidget(buttons_[static_cast<std::size_t>(index)], index / columns,
                        index % columns);
    }
    for (int column = 0; column < count; ++column) {
        grid->setColumnStretch(column, column < columns ? 1 : 0);
    }
}

void SegmentedBar::reflow() {
    if (buttons_.empty()) {
        return;
    }
    auto* grid = static_cast<QGridLayout*>(layout());
    const QMargins margins = grid->contentsMargins();
    const int gap = grid->horizontalSpacing();
    int cell = 0;
    for (SegmentButton* button : buttons_) {
        cell = std::max(cell, button->naturalWidth());
    }
    const int room = width() - margins.left() - margins.right() + gap;
    const SegmentedShape shape =
        segmented_columns(static_cast<int>(buttons_.size()), cell, gap, room);
    for (SegmentButton* button : buttons_) {
        button->setWrapping(shape.wrap);
        button->setMinimumWidth(shape.wrap ? 0 : cell);
    }
    place(shape.columns);
}

void SegmentedBar::resizeEvent(QResizeEvent* event) {
    QWidget::resizeEvent(event);
    reflow();
}

void SegmentedBar::changeEvent(QEvent* event) {
    QWidget::changeEvent(event);
    if ((event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange) &&
        reflowTimer_ != nullptr) {
        reflowTimer_->start(0);
    }
}

void SegmentedBar::setCurrentIndex(int index) {
    if (index == index_ || index < 0 || index >= static_cast<int>(buttons_.size())) {
        return;
    }
    index_ = index;
    buttons_[static_cast<std::size_t>(index)]->setChecked(true);
    Q_EMIT currentIndexChanged(index);
}

} // namespace fv
