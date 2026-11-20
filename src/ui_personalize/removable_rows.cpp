#include "ui_personalize/removable_rows.h"

#include <algorithm>
#include <cstddef>
#include <optional>
#include <utility>

#include <QApplication>
#include <QHBoxLayout>
#include <QVBoxLayout>

#include "ui_widgets/buttons.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

// _small_button.
Button* smallButton(const QString& text) {
    auto* button = neutralButton(new Button(text));
    fitCompactButton(button);
    return button;
}

} // namespace

RemovableRows::RemovableRows(const QString& emptyText, std::function<void(const QString&)> onRemove,
                             QWidget* picker, QWidget* parent)
    : QWidget(parent), onRemove_(std::move(onRemove)), picker_(picker) {
    setObjectName(QStringLiteral("appStylesRows"));
    column_ = new QVBoxLayout(this);
    column_->setContentsMargins(0, 0, 0, 0);
    column_->setSpacing(6);
    empty_ = new WrappedLabel(emptyText);
    empty_->setObjectName(QStringLiteral("infoLabel"));
    column_->addWidget(empty_);
}

std::vector<Button*> RemovableRows::removeButtons() const {
    return removes_;
}

void RemovableRows::setRows(const Entries& entries) {
    if (entries == shown_) {
        return;
    }
    QWidget* focused = QApplication::focusWidget();
    std::optional<std::size_t> held;
    for (std::size_t index = 0; index < rows_.size(); ++index) {
        if (focused != nullptr && rows_[index]->isAncestorOf(focused)) {
            held = index;
            break;
        }
    }
    const std::optional<QString> heldValue =
        held.has_value() ? std::optional<QString>(shown_[*held].first) : std::nullopt;
    std::vector<QWidget*> oldRows = std::move(rows_);
    rows_.clear();
    removes_.clear();
    for (QWidget* row : oldRows) {
        column_->removeWidget(row);
    }
    shown_ = entries;
    empty_->setVisible(entries.empty());
    for (const auto& [value, label] : entries) {
        auto* row = new QWidget;
        row->setObjectName(QStringLiteral("appStylesRow"));
        auto* line = new QHBoxLayout(row);
        line->setContentsMargins(0, 0, 0, 0);
        line->setSpacing(8);
        auto* text = new WrappedLabel(label);
        text->setObjectName(QStringLiteral("appStylesRowLabel"));
        line->addWidget(text, 1);
        Button* remove = smallButton(QStringLiteral("Remove"));
        remove->setAccessibleName(QStringLiteral("Remove %1").arg(value));
        connect(remove, &QPushButton::clicked, this, [this, value = value] {
            if (onRemove_) {
                onRemove_(value);
            }
        });
        line->addWidget(remove);
        column_->addWidget(row);
        // Shown now rather than when the layout gets to it: focus given to a hidden button waits
        // for it and is lost to the next focus change.
        row->show();
        rows_.push_back(row);
        removes_.push_back(remove);
    }
    chainTabOrder(removes_);
    if (held.has_value()) {
        QWidget* target = picker_.data();
        const auto same = std::ranges::find_if(
            entries, [&heldValue](const auto& entry) { return entry.first == *heldValue; });
        if (same != entries.end()) {
            target = removes_[static_cast<std::size_t>(same - entries.begin())];
        } else if (!removes_.empty()) {
            target = removes_[std::min(*held, removes_.size() - 1)];
        }
        // Before the old rows go: deleting or hiding the focused one drops focus or hands it to
        // the window header.
        if (target != nullptr) {
            target->setFocus(Qt::OtherFocusReason);
        }
    }
    for (QWidget* row : oldRows) {
        row->hide();
        row->deleteLater();
    }
}

void RemovableRows::chainTabOrder(const std::vector<Button*>& removes) {
    if (picker_ == nullptr) {
        return;
    }
    // New rows join the end of the window's Tab order; put them back between whatever comes
    // before the list and the picker under it.
    QWidget* before = picker_->previousInFocusChain();
    while (before != nullptr && before != picker_ &&
           (before->focusPolicy() == Qt::NoFocus || isAncestorOf(before))) {
        before = before->previousInFocusChain();
    }
    if (before == nullptr) {
        return;
    }
    std::vector<QWidget*> chain(removes.begin(), removes.end());
    chain.push_back(picker_.data());
    for (QWidget* widget : chain) {
        if (before != widget) {
            QWidget::setTabOrder(before, widget);
        }
        before = widget;
    }
}

} // namespace fv
