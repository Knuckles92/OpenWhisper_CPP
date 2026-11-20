#include "ui_widgets/nav_rail.h"

#include <algorithm>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QSizePolicy>
#include <QTransform>
#include <QVBoxLayout>

#include "ui_theme/icons.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"

namespace fv {
namespace {

constexpr int kKeyRole = Qt::UserRole;
constexpr int kChapterRole = Qt::UserRole + 1;

} // namespace

// OpenWhisper's _RailItem and _ChapterRow; a leading underscore and a capital is a name C++
// reserves.
namespace nav_rail_detail {

// A page row: name plus current-value line.
class RailItem : public QWidget {
    Q_OBJECT
public:
    // Fallback caption width before the list has laid out its rows.
    static constexpr int kValueWidth = 178;
    // Both kinds of row leave the value this much of margins.
    static constexpr int kValueMargins = 47;

    RailItem(const QString& name, const QIcon& icon, bool nested) {
        setObjectName(QStringLiteral("modelManagerRailItem"));
        // A plain QWidget does not paint a stylesheet background without this, so the selected
        // pill would be invisible.
        setAttribute(Qt::WA_StyledBackground, true);
        ui::setStyleProperty(this, ui::prop::nested, nested);
        setCursor(Qt::PointingHandCursor);

        auto* row = new QHBoxLayout(this);
        row->setSpacing(9);
        iconLabel = new QLabel(this);
        iconLabel->setObjectName(QStringLiteral("modelManagerRailIcon"));
        iconLabel->setFixedSize(18, 18);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(2);
        // A chapter's page lines its name up with the chapter's name.
        if (nested) {
            // The guide runs the full row height, under the chapter's icon, so an open
            // chapter's pages read as one connected group.
            row->setContentsMargins(18, 0, 10, 0);
            row->setSpacing(17);
            guide = new QFrame;
            guide->setObjectName(QStringLiteral("settingsRailGuide"));
            guide->setFixedWidth(2);
            row->addWidget(guide);
            copy->setContentsMargins(0, 7, 0, 7);
            iconLabel->hide();
        } else {
            row->setContentsMargins(10, 8, 10, 8);
            if (!icon.isNull()) {
                iconLabel->setPixmap(icon.pixmap(16, 16));
            }
            row->addWidget(iconLabel, 0, Qt::AlignTop);
            copy->setContentsMargins(0, 0, 0, 0);
        }
        nameLabel = new QLabel(name);
        nameLabel->setObjectName(QStringLiteral("modelManagerRailName"));
        valueLabel = new QLabel(QString());
        valueLabel->setObjectName(QStringLiteral("modelManagerRailValue"));
        valueLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        copy->addWidget(nameLabel);
        copy->addWidget(valueLabel);
        // Shown with its first value, so a page without one stays one line.
        valueLabel->setVisible(false);
        row->addLayout(copy, 1);

        setSelected(false);
    }

    const QString& value() const { return value_; }

    void setValue(const QString& text) {
        value_ = text;
        const QFontMetrics metrics = valueLabel->fontMetrics();
        // Room for the row margins, icon and spacing at any rail width. A list item may briefly
        // be wider than the viewport during a resize.
        int itemWidth = width();
        if (parentWidget() != nullptr) {
            itemWidth = std::min(itemWidth, parentWidget()->width());
        }
        const int available = itemWidth > kValueMargins ? itemWidth - kValueMargins : kValueWidth;
        valueLabel->setText(metrics.elidedText(value_, Qt::ElideRight, available));
        valueLabel->setVisible(!value_.isEmpty());
    }

    // An item widget covers the view's own selection painting, so the state lives on this
    // widget for the stylesheet to see it.
    void setSelected(bool selected) {
        for (QWidget* widget : {static_cast<QWidget*>(this), static_cast<QWidget*>(nameLabel),
                                static_cast<QWidget*>(valueLabel), static_cast<QWidget*>(guide)}) {
            ui::setStyleProperty(widget, ui::prop::selected, selected);
        }
    }

    QLabel* iconLabel = nullptr;
    QFrame* guide = nullptr;
    QLabel* nameLabel = nullptr;
    QLabel* valueLabel = nullptr;

protected:
    void resizeEvent(QResizeEvent* event) override {
        QWidget::resizeEvent(event);
        if (!value_.isEmpty()) {
            setValue(value_);
        }
    }

private:
    QString value_;
};

// A chapter heading: its icon, its name, and whether its pages show. It carries the selection
// pill while the current page is one of its own, open or closed, so the top level of the rail
// always says where you are.
class ChapterRow : public QWidget {
    Q_OBJECT
public:
    ChapterRow(const QString& title, const QIcon& icon) {
        setObjectName(QStringLiteral("settingsRailChapter"));
        setAttribute(Qt::WA_StyledBackground, true);
        setCursor(Qt::PointingHandCursor);
        auto* row = new QHBoxLayout(this);
        row->setContentsMargins(10, 9, 10, 9);
        row->setSpacing(9);
        iconLabel = new QLabel;
        iconLabel->setObjectName(QStringLiteral("modelManagerRailIcon"));
        iconLabel->setFixedSize(18, 18);
        if (!icon.isNull()) {
            iconLabel->setPixmap(icon.pixmap(16, 16));
        }
        row->addWidget(iconLabel);
        nameLabel = new QLabel(title);
        nameLabel->setObjectName(QStringLiteral("settingsRailChapterName"));
        row->addWidget(nameLabel, 1);
        chevron = new QLabel;
        chevron->setObjectName(QStringLiteral("settingsRailChevron"));
        chevron->setFixedSize(12, 12);
        row->addWidget(chevron);
        setOpen(false);
        setActive(false);
        constructed_ = true;
    }

    void setOpen(bool open) {
        open_ = open;
        // The stylesheet's chevrons use the same per-theme drawing.
        const QString suffix = ui::currentPalette().qCss(u"icon-suffix");
        QPixmap pixmap =
            ui::tablerPixmap(QStringLiteral("chevron-down-slate%1.svg").arg(suffix), 12);
        if (!open) {
            pixmap = pixmap.transformed(QTransform().rotate(-90));
        }
        chevron->setPixmap(pixmap);
    }

    // Whether the current page is one of this chapter's.
    void setActive(bool active) {
        for (QWidget* widget : {static_cast<QWidget*>(this), static_cast<QWidget*>(nameLabel)}) {
            ui::setStyleProperty(widget, ui::prop::active, active);
        }
    }

    QLabel* iconLabel = nullptr;
    QLabel* nameLabel = nullptr;
    QLabel* chevron = nullptr;

protected:
    void changeEvent(QEvent* event) override {
        QWidget::changeEvent(event);
        if (event->type() == QEvent::StyleChange && constructed_) {
            // A theme switch restyles the row; the chevron is redrawn for it.
            setOpen(open_);
        }
    }

private:
    bool open_ = false;
    bool constructed_ = false;
};

} // namespace nav_rail_detail

using nav_rail_detail::ChapterRow;
using nav_rail_detail::RailItem;

NavRail::NavRail(QWidget* parent) : QListWidget(parent) {
    setObjectName(QStringLiteral("modelManagerRail"));
    setMinimumWidth(kRailMinWidth);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    // Per item, the range counts a closed chapter's hidden rows, and scrolling to the last page
    // of an open one falls short.
    setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setSelectionMode(QAbstractItemView::SingleSelection);
    setUniformItemSizes(false);
    // Rows space themselves, so an open chapter's guide line is unbroken.
    setSpacing(0);
    connect(this, &QListWidget::currentItemChanged, this,
            [this](QListWidgetItem* current, QListWidgetItem*) { onCurrentChanged(current); });
}

void NavRail::updateGeometries() {
    QListWidget::updateGeometries();
    // QListView places item widgets before it narrows its contents to the viewport, so a
    // shrinking rail would leave rows under the scroll bar.
    updateEditorGeometries();
}

void NavRail::scrollContentsBy(int, int) {
    // Layout refreshes already place rows at the scroll bar's current offset; that absolute
    // geometry is the only source of position, since Qt's relative viewport scroll would move
    // the same rows a second time. Zero deltas keep QListView's scroll bookkeeping without
    // moving its child widgets. This small rail can repaint instead of pixel-scroll.
    QListWidget::scrollContentsBy(0, 0);
    updateEditorGeometries();
    viewport()->update();
}

void NavRail::addChapter(const QString& title, const QIcon& icon) {
    auto* item = new QListWidgetItem(this);
    // Enabled for the hover cursor, never selectable: a click opens the chapter's page instead.
    item->setFlags(Qt::ItemIsEnabled);
    item->setData(kChapterRole, title);
    auto* row = new ChapterRow(title, icon);
    item->setSizeHint(QSize(0, row->sizeHint().height()));
    setItemWidget(item, row);
    if (!chapters_.contains(title)) {
        chapterOrder_.append(title);
    }
    chapters_.insert(title, Chapter{item, row, {}, {}});
    adding_ = title;
}

void NavRail::addDestination(const QString& key, const QString& name, const QIcon& icon) {
    auto* item = new QListWidgetItem(this);
    item->setData(kKeyRole, key);
    const QString chapterTitle = adding_;
    auto* row = new RailItem(name, icon, !chapterTitle.isEmpty());
    item->setSizeHint(QSize(0, row->sizeHint().height()));
    setItemWidget(item, row);
    if (!items_.contains(key)) {
        keyOrder_.append(key);
    }
    rows_.insert(key, row);
    items_.insert(key, item);
    if (!chapterTitle.isEmpty()) {
        chapters_[chapterTitle].keys.append(key);
        chapterOf_.insert(key, chapterTitle);
        item->setHidden(!expanded_.contains(chapterTitle));
    }
}

void NavRail::setValue(const QString& key, const QString& text) {
    auto* row = static_cast<RailItem*>(rows_.value(key));
    if (row == nullptr) {
        return;
    }
    const bool shown = !row->valueLabel->isHidden();
    row->setValue(text);
    if (shown == row->valueLabel->isHidden()) {
        // The row's nested layouts hear of the caption on the next event-loop pass; they are
        // re-measured now, or a closed chapter's pages keep one line's height when it opens.
        row->layout()->invalidate();
        row->layout()->activate();
        items_.value(key)->setSizeHint(QSize(0, row->sizeHint().height()));
    }
}

QString NavRail::value(const QString& key) const {
    const auto* row = static_cast<const RailItem*>(rows_.value(key));
    return row != nullptr ? row->value() : QString();
}

QString NavRail::name(const QString& key) const {
    const auto* row = static_cast<const RailItem*>(rows_.value(key));
    return row != nullptr ? row->nameLabel->text() : QString();
}

QString NavRail::chapter(const QString& key) const {
    return chapterOf_.value(key);
}

QStringList NavRail::chapterKeys(const QString& title) const {
    const auto found = chapters_.constFind(title);
    return found != chapters_.constEnd() ? found->keys : QStringList();
}

QString NavRail::activeChapter() const {
    return chapter(currentKey());
}

void NavRail::setExpanded(const QString& title, bool expanded) {
    const auto found = chapters_.constFind(title);
    if (found == chapters_.constEnd() || expanded == expanded_.contains(title)) {
        return;
    }
    if (expanded) {
        expanded_.insert(title);
    } else {
        expanded_.remove(title);
    }
    static_cast<ChapterRow*>(found->row)->setOpen(expanded);
    for (const QString& key : found->keys) {
        items_.value(key)->setHidden(!expanded);
    }
}

void NavRail::select(const QString& key) {
    QListWidgetItem* item = items_.value(key);
    if (item != nullptr) {
        setExpanded(chapter(key), true);
        setCurrentItem(item);
    }
}

void NavRail::selectChapter(const QString& title) {
    const auto found = chapters_.constFind(title);
    if (found != chapters_.constEnd() && !found->keys.isEmpty()) {
        select(found->last.isEmpty() ? found->keys.first() : found->last);
    }
}

QString NavRail::currentKey() const {
    const QListWidgetItem* item = currentItem();
    return item != nullptr ? item->data(kKeyRole).toString() : QString();
}

QWidget* NavRail::destinationRow(const QString& key) const {
    return rows_.value(key);
}

QListWidgetItem* NavRail::chapterItem(const QString& title) const {
    const auto found = chapters_.constFind(title);
    return found != chapters_.constEnd() ? found->item : nullptr;
}

QWidget* NavRail::chapterRow(const QString& title) const {
    const auto found = chapters_.constFind(title);
    return found != chapters_.constEnd() ? found->row : nullptr;
}

void NavRail::clickChapter(const QString& title) {
    if (title == activeChapter() && isExpanded(title)) {
        setExpanded(title, false);
    } else {
        selectChapter(title);
    }
}

void NavRail::mousePressEvent(QMouseEvent* event) {
    const QListWidgetItem* item = itemAt(event->position().toPoint());
    const QString title = item != nullptr ? item->data(kChapterRole).toString() : QString();
    if (!title.isEmpty()) {
        if (event->button() == Qt::LeftButton) {
            clickChapter(title);
        }
        event->accept();
        return;
    }
    QListWidget::mousePressEvent(event);
}

void NavRail::mouseMoveEvent(QMouseEvent* event) {
    // Dragging would select whatever row it crosses, chapter headings too.
    if (event->buttons() != Qt::NoButton) {
        event->accept();
        return;
    }
    QListWidget::mouseMoveEvent(event);
}

QModelIndex NavRail::moveCursor(CursorAction cursorAction, Qt::KeyboardModifiers modifiers) {
    const QString current = currentKey();
    const qsizetype index = keyOrder_.indexOf(current);
    if (keyOrder_.isEmpty() || index < 0) {
        return QListWidget::moveCursor(cursorAction, modifiers);
    }
    const qsizetype count = keyOrder_.size();
    qsizetype step = 0;
    switch (cursorAction) {
    case MoveDown:
    case MoveNext:
        step = 1;
        break;
    case MoveUp:
    case MovePrevious:
        step = -1;
        break;
    case MovePageDown:
        step = kPageStep;
        break;
    case MovePageUp:
        step = -kPageStep;
        break;
    case MoveHome:
        step = -count;
        break;
    case MoveEnd:
        step = count;
        break;
    default:
        return QListWidget::moveCursor(cursorAction, modifiers);
    }
    const QString target = keyOrder_.at(std::clamp<qsizetype>(index + step, 0, count - 1));
    // The target's chapter opens first: a hidden row cannot be current.
    setExpanded(chapter(target), true);
    return indexFromItem(items_.value(target));
}

void NavRail::onCurrentChanged(QListWidgetItem* current) {
    for (auto it = rows_.constBegin(); it != rows_.constEnd(); ++it) {
        static_cast<RailItem*>(it.value())->setSelected(items_.value(it.key()) == current);
    }
    const QString key = current != nullptr ? current->data(kKeyRole).toString() : QString();
    const QString title = key.isEmpty() ? QString() : chapter(key);
    for (auto it = chapters_.constBegin(); it != chapters_.constEnd(); ++it) {
        static_cast<ChapterRow*>(it->row)->setActive(it.key() == title);
    }
    if (key.isEmpty()) {
        return;
    }
    if (!title.isEmpty()) {
        chapters_[title].last = key;
        setExpanded(title, true);
    }
    Q_EMIT destinationChanged(key);
}

} // namespace fv

#include "nav_rail.moc"
