#include "ui_widgets/cards.h"

#include <algorithm>

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QVBoxLayout>

#include "ui_widgets/collapse_animation.h"
#include "ui_widgets/collapsible_header.h"

namespace fv {

Card::Card(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("card"));
    layout_ = new QVBoxLayout(this);
    layout_->setContentsMargins(16, 16, 16, 16);
    layout_->setSpacing(12);
    setMinimumHeight(100);
}

ControlPanel::ControlPanel(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("controlPanel"));
    layout_ = new QHBoxLayout(this);
    layout_->setContentsMargins(16, 16, 16, 16);
    layout_->setSpacing(12);
}

HeaderCard::HeaderCard(const QString& title, bool collapsible, QWidget* parent)
    : Card(parent), collapsible_(collapsible) {
    headerLayout_ = new QHBoxLayout;
    headerLayout_->setContentsMargins(0, 0, 0, 0);
    headerLayout_->setSpacing(8);

    if (collapsible) {
        CollapsibleSectionToggleOptions options;
        options.expanded = false;
        options.expandTooltip = QStringLiteral("Show transcription output");
        options.collapseTooltip = QStringLiteral("Hide transcription output");
        sectionToggle_ = new CollapsibleSectionToggle(title, options);
        connect(sectionToggle_, &CollapsibleSectionToggle::toggledExpanded, this,
                &HeaderCard::onSectionToggled);

        headerLayout_->addStretch();
        headerLayout_->addWidget(sectionToggle_);
        headerLayout_->addStretch();
    } else {
        titleLabel_ = new QLabel(title);
        titleLabel_->setObjectName(QStringLiteral("headerLabel"));
        QFont titleFont(QStringLiteral("Segoe UI"), 14);
        titleFont.setBold(true);
        titleLabel_->setFont(titleFont);
        titleLabel_->setAlignment(Qt::AlignCenter);

        headerLayout_->addStretch();
        headerLayout_->addWidget(titleLabel_);
        headerLayout_->addStretch();
    }

    layout_->insertLayout(0, headerLayout_);
    layout_->insertSpacing(1, 12);

    if (collapsible) {
        contentWidget_ = new QWidget;
        contentLayout_ = new QVBoxLayout(contentWidget_);
        contentLayout_->setContentsMargins(0, 0, 0, 0);
        contentLayout_->setSpacing(12);
        layout_->addWidget(contentWidget_);
        setMinimumHeight(0);
    }
}

void HeaderCard::addContentWidget(QWidget* widget) {
    if (collapsible_) {
        contentLayout_->addWidget(widget);
    } else {
        layout_->addWidget(widget);
    }
}

void HeaderCard::setTitle(const QString& title) {
    if (sectionToggle_ != nullptr) {
        sectionToggle_->setTitle(title);
    } else if (titleLabel_ != nullptr) {
        titleLabel_->setText(title);
    }
}

void HeaderCard::onSectionToggled(bool expanded) {
    setCollapsed(!expanded);
}

void HeaderCard::setCollapsed(bool collapsed, bool emitSignal) {
    if (!collapsible_ || collapsed == collapsed_) {
        return;
    }

    if (collapsed) {
        contentHeight_ = std::max(contentWidget_->height(), contentWidget_->sizeHint().height());
    }

    collapsed_ = collapsed;

    if (sectionToggle_ != nullptr) {
        sectionToggle_->setExpanded(!collapsed, false);
    }

    if (emitSignal) {
        Q_EMIT toggled(collapsed);
        animateContentVisibility(collapsed);
    } else {
        applyCollapsedImmediate(collapsed);
    }
}

void HeaderCard::applyCollapsedImmediate(bool collapsed) {
    if (contentAnim_ != nullptr) {
        contentAnim_->stop();
    }
    contentWidget_->setVisible(!collapsed);
    contentWidget_->setMaximumHeight(kUnlimitedHeight);
    applyCardSizePolicy(collapsed);
}

QPropertyAnimation* HeaderCard::contentAnimation() {
    if (contentAnim_ == nullptr) {
        contentAnim_ = createMaxHeightAnimation(contentWidget_);
    }
    return contentAnim_;
}

int HeaderCard::contentNaturalHeight() {
    contentWidget_->setVisible(true);
    contentWidget_->adjustSize();
    return std::max(contentWidget_->sizeHint().height(),
                    contentWidget_->minimumSizeHint().height());
}

void HeaderCard::applyCardSizePolicy(bool collapsed) {
    if (collapsed) {
        setMinimumHeight(0);
        layout_->activate();
        setMaximumHeight(sizeHint().height());
    } else {
        setMinimumHeight(0);
        setMaximumHeight(kUnlimitedHeight);
        updateGeometry();
    }
}

void HeaderCard::animateContentVisibility(bool collapsed) {
    const int natural = contentNaturalHeight();
    contentWidget_->setVisible(true);
    setMinimumHeight(0);
    setMaximumHeight(kUnlimitedHeight);
    contentWidget_->setMinimumHeight(0);

    int start = 0;
    int end = 0;
    if (collapsed) {
        const int current = contentWidget_->height();
        start = current != 0 ? current : natural;
        end = 0;
    } else {
        start = 0;
        end = natural;
    }

    runMaxHeightAnimation(contentAnimation(), start, end, [this, collapsed] {
        contentWidget_->setMaximumHeight(kUnlimitedHeight);
        if (collapsed) {
            contentWidget_->setVisible(false);
        }
        applyCardSizePolicy(collapsed);
    });
}

} // namespace fv
