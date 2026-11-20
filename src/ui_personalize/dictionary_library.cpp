#include "ui_personalize/dictionary_library.h"

#include <algorithm>
#include <utility>

#include <QApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "pyport/text.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/empty_state.h"
#include "ui_widgets/reflow.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

QLabel* badge(const QString& text, const QString& tone) {
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("dictionaryBadge"));
    ui::setStyleProperty(label, ui::prop::tone, tone);
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return label;
}

QString joined(const std::vector<std::string>& parts, const char* separator) {
    std::string out;
    for (const std::string& part : parts) {
        out += (out.empty() ? "" : separator) + part;
    }
    return qt::toQString(out);
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

} // namespace

QWidget* actionGroup(const std::vector<QWidget*>& buttons) {
    auto* holder = new QWidget;
    holder->setObjectName(QStringLiteral("dictionaryActions"));
    auto* row = new QHBoxLayout(holder);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);
    for (QWidget* button : buttons) {
        row->addWidget(button);
    }
    return holder;
}

DictionaryRow::DictionaryRow(const text::DictionaryTerm& term, QWidget* parent)
    : QFrame(parent), term_(term) {
    setObjectName(QStringLiteral("dictionaryRow"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto* frame = new QVBoxLayout(this);
    frame->setContentsMargins(8, 7, 8, 7);
    frame->setSpacing(0);
    auto* lead = new QWidget;
    lead->setObjectName(QStringLiteral("dictionaryRowText"));
    auto* row = new QHBoxLayout(lead);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(8);

    star_ = new QPushButton;
    star_->setObjectName(QStringLiteral("dictionaryStar"));
    star_->setCheckable(true);
    star_->setFlat(true);
    star_->setCursor(Qt::PointingHandCursor);
    star_->setIconSize(QSize(18, 18));
    star_->setFixedSize(30, 30);
    connect(star_, &QPushButton::clicked, this,
            [this](bool checked) { Q_EMIT starToggled(qt::toQString(term_.id), checked); });
    row->addWidget(star_, 0, Qt::AlignTop);

    auto* text = new QWidget;
    text->setObjectName(QStringLiteral("dictionaryRowText"));
    auto* column = new QVBoxLayout(text);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(3);
    name_ = new ElidingLabel;
    name_->setObjectName(QStringLiteral("dictionaryTerm"));
    column->addWidget(name_);
    // The word gets a line to itself; its badges sit with the detail.
    auto* secondLine = new QHBoxLayout;
    secondLine->setContentsMargins(0, 0, 0, 0);
    secondLine->setSpacing(6);
    learnedBadge_ = badge(QStringLiteral("Learned"), QStringLiteral("learned"));
    newBadge_ = badge(QStringLiteral("New"), QStringLiteral("new"));
    keptBadge_ = badge(QStringLiteral("Kept as said"), QStringLiteral("kept"));
    keptBadge_->setToolTip(QStringLiteral(
        "You put this word back after AI cleanup changed it, so cleanup leaves it alone"));
    secondLine->addWidget(learnedBadge_);
    secondLine->addWidget(newBadge_);
    secondLine->addWidget(keptBadge_);
    detail_ = new ElidingLabel;
    detail_->setObjectName(QStringLiteral("dictionaryDetail"));
    secondLine->addWidget(detail_, 1);
    secondLine->addStretch();
    column->addLayout(secondLine);
    row->addWidget(text, 1, Qt::AlignVCenter);

    edit_ = neutralButton(new Button(QStringLiteral("Edit")));
    edit_->setObjectName(QStringLiteral("dictionaryEditButton"));
    fitCompactButton(edit_);
    connect(edit_, &QPushButton::clicked, this,
            [this] { Q_EMIT editRequested(qt::toQString(term_.id)); });
    remove_ = neutralButton(new Button(QStringLiteral("Remove")));
    remove_->setObjectName(QStringLiteral("dictionaryRemoveButton"));
    fitCompactButton(remove_);
    connect(remove_, &QPushButton::clicked, this,
            [this] { Q_EMIT removeRequested(qt::toQString(term_.id)); });
    layoutRow_ = new Reflow(lead, actionGroup({edit_, remove_}), 170);
    frame->addWidget(layoutRow_);
    setTerm(term);
}

void DictionaryRow::setTerm(const text::DictionaryTerm& term) {
    term_ = term;
    const QString word = qt::toQString(term.term);
    name_->setText(word);
    star_->setChecked(term.starred);
    ui::setStyleProperty(star_, ui::prop::starred, term.starred);
    star_->setToolTip(term.starred ? QStringLiteral("Unstar")
                                   : QStringLiteral("Star: listened for first"));
    star_->setAccessibleName(QStringLiteral("%1 %2").arg(
        term.starred ? QStringLiteral("Unstar") : QStringLiteral("Star"), word));
    learnedBadge_->setVisible(term.learned);
    newBadge_->setVisible(term.is_new);
    keptBadge_->setVisible(term.is_protected);
    const QString detail =
        term.heard.empty() ? QString() : QStringLiteral("Sounds like ") + joined(term.heard, ", ");
    learnedBadge_->setToolTip(QStringLiteral("Learned from your corrections"));
    detail_->setText(detail);
    detail_->setVisible(!detail.isEmpty());
    const bool undo = term.learned && term.is_new;
    remove_->setText(undo ? QStringLiteral("Undo") : QStringLiteral("Remove"));
    // As wide as "Remove" while it reads "Undo", so every row's buttons line up.
    remove_->ensurePolished();
    remove_->setBaseMinimumSize(
        remove_->fontMetrics().horizontalAdvance(QStringLiteral("Remove")) + 40, 34);
    remove_->setToolTip(undo ? QStringLiteral("Forget this learned word")
                             : QStringLiteral("Remove this word from your dictionary"));
    remove_->setAccessibleName(QStringLiteral("%1 %2").arg(remove_->text(), word));
    edit_->setAccessibleName(QStringLiteral("Edit %1").arg(word));
}

DictionaryLibrary::DictionaryLibrary(QWidget* focusWhenEmpty, QWidget* parent)
    : QWidget(parent), focusWhenEmpty_(focusWhenEmpty) {
    setObjectName(QStringLiteral("dictionaryLibrary"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    search_ = new QLineEdit;
    search_->setObjectName(QStringLiteral("dictionarySearchInput"));
    search_->setPlaceholderText(QStringLiteral("Find a word"));
    search_->setClearButtonEnabled(true);
    search_->setAccessibleName(QStringLiteral("Find a word"));
    connect(search_, &QLineEdit::textChanged, this, [this] { render(); });
    layout->addWidget(search_);

    list_ = new QWidget;
    list_->setObjectName(QStringLiteral("dictionaryRows"));
    listLayout_ = new QVBoxLayout(list_);
    listLayout_->setContentsMargins(0, 0, 0, 0);
    listLayout_->setSpacing(6);
    layout->addWidget(list_);

    empty_ = new EmptyState;
    layout->addWidget(empty_);

    more_ = new WrappedLabel;
    more_->setObjectName(QStringLiteral("infoLabel"));
    layout->addWidget(more_);
    render();
}

std::vector<QString> DictionaryLibrary::visibleIds() const {
    std::vector<QString> ids;
    for (int index = 0; index < listLayout_->count(); ++index) {
        if (auto* shown = qobject_cast<DictionaryRow*>(listLayout_->itemAt(index)->widget())) {
            ids.push_back(qt::toQString(shown->term().id));
        }
    }
    return ids;
}

DictionaryRow* DictionaryLibrary::row(const QString& termId) const {
    const auto found = rows_.find(termId);
    return found == rows_.end() ? nullptr : found->second;
}

void DictionaryLibrary::setTerms(std::vector<text::DictionaryTerm> terms) {
    terms_ = std::move(terms);
    render();
}

std::vector<text::DictionaryTerm> DictionaryLibrary::matching() const {
    const std::string needle = py::casefold(py::strip(qt::toUtf8(search_->text())));
    if (needle.empty()) {
        return terms_;
    }
    std::vector<text::DictionaryTerm> found;
    for (const text::DictionaryTerm& term : terms_) {
        const bool hit = contains(py::casefold(term.term), needle) ||
                         std::ranges::any_of(term.heard, [&needle](const std::string& variant) {
                             return contains(py::casefold(variant), needle);
                         });
        if (hit) {
            found.push_back(term);
        }
    }
    return found;
}

std::optional<std::size_t>
DictionaryLibrary::focusedRowIndex(const std::vector<QString>& keep) const {
    QWidget* focused = QApplication::focusWidget();
    if (focused == nullptr) {
        return std::nullopt;
    }
    const std::vector<QString> ids = visibleIds();
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (std::ranges::find(keep, ids[index]) != keep.end()) {
            continue;
        }
        const DictionaryRow* held = row(ids[index]);
        if (held != nullptr && held->isAncestorOf(focused)) {
            return index;
        }
    }
    return std::nullopt;
}

void DictionaryLibrary::placeRows(const std::vector<text::DictionaryTerm>& shown) {
    for (std::size_t index = 0; index < shown.size(); ++index) {
        const text::DictionaryTerm& term = shown[index];
        const QString id = qt::toQString(term.id);
        DictionaryRow* placed = row(id);
        if (placed == nullptr) {
            placed = new DictionaryRow(term);
            connect(placed, &DictionaryRow::starToggled, this, &DictionaryLibrary::starToggled);
            connect(placed, &DictionaryRow::editRequested, this, &DictionaryLibrary::editRequested);
            connect(placed, &DictionaryRow::removeRequested, this,
                    &DictionaryLibrary::removeRequested);
            rows_[id] = placed;
        } else if (placed->term() != term) {
            placed->setTerm(term);
        }
        const int at = static_cast<int>(index);
        if (listLayout_->indexOf(placed) != at) {
            listLayout_->removeWidget(placed);
            listLayout_->insertWidget(at, placed);
        }
        placed->show();
    }
}

void DictionaryLibrary::chainTabOrder(const std::vector<text::DictionaryTerm>& shown) {
    // New rows join the end of the window's Tab order, after the tiles below the list; the
    // search box sits where the list starts.
    QWidget* previous = search_;
    for (const text::DictionaryTerm& term : shown) {
        const DictionaryRow* placed = row(qt::toQString(term.id));
        for (QWidget* widget :
             {static_cast<QWidget*>(placed->star()), static_cast<QWidget*>(placed->edit()),
              static_cast<QWidget*>(placed->remove())}) {
            QWidget::setTabOrder(previous, widget);
            previous = widget;
        }
    }
}

void DictionaryLibrary::refocusAfterRemoval(std::size_t held,
                                            const std::vector<text::DictionaryTerm>& shown) {
    QWidget* target = nullptr;
    if (!shown.empty()) {
        target = row(qt::toQString(shown[std::min(held, shown.size() - 1)].id))->remove();
    } else {
        target = search_->isVisible() ? search_ : focusWhenEmpty_.data();
    }
    if (target != nullptr) {
        target->setFocus(Qt::OtherFocusReason);
    }
}

void DictionaryLibrary::render() {
    search_->setVisible(terms_.size() >= kSearchFrom || !search_->text().isEmpty());
    const std::vector<text::DictionaryTerm> found = matching();
    const std::vector<text::DictionaryTerm> shown(
        found.begin(),
        found.begin() + static_cast<std::ptrdiff_t>(std::min(found.size(), kMaxRows)));
    std::vector<QString> keep;
    for (const text::DictionaryTerm& term : shown) {
        keep.push_back(qt::toQString(term.id));
    }
    const std::optional<std::size_t> held = focusedRowIndex(keep);
    std::vector<DictionaryRow*> stale;
    for (auto it = rows_.begin(); it != rows_.end();) {
        if (std::ranges::find(keep, it->first) == keep.end()) {
            stale.push_back(it->second);
            it = rows_.erase(it);
        } else {
            ++it;
        }
    }
    for (DictionaryRow* gone : stale) {
        listLayout_->removeWidget(gone);
    }
    placeRows(shown);
    chainTabOrder(shown);
    if (held.has_value()) {
        // Before the removed row hides, which would pass its focus to whatever follows the list.
        refocusAfterRemoval(*held, shown);
    }
    for (DictionaryRow* gone : stale) {
        gone->hide();
        gone->deleteLater();
    }
    list_->setVisible(!shown.empty());
    if (terms_.empty()) {
        empty_->setText(
            QStringLiteral("No words yet"),
            QStringLiteral("Add names, product terms, or anything dictation tends to get wrong."));
    } else if (found.empty()) {
        empty_->setText(QStringLiteral("No words match “%1”")
                            .arg(qt::toQString(py::strip(qt::toUtf8(search_->text())))),
                        QStringLiteral("Try a shorter search."));
    }
    empty_->setVisible(shown.empty());
    const std::size_t hidden = found.size() - shown.size();
    more_->setText(hidden != 0 ? QStringLiteral("Showing %1 of %2. Search to find the rest.")
                                     .arg(shown.size())
                                     .arg(found.size())
                               : QString());
    more_->setVisible(hidden != 0);
}

} // namespace fv
