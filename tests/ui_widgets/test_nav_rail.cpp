// Layout and chapter behaviour of the Settings navigation rail.
// From tests/test_nav_rail.py.
#include <catch2/catch_test_macros.hpp>

#include <map>
#include <memory>
#include <utility>
#include <vector>

#include <QApplication>
#include <QLabel>
#include <QListWidgetItem>
#include <QMoveEvent>
#include <QScrollBar>
#include <QTest>
#include <QVBoxLayout>
#include <QWindow>

#include "app_theme.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/nav_rail.h"

namespace {

QLabel* nameLabelOf(QWidget* row) {
    auto* label = row->findChild<QLabel*>(QStringLiteral("modelManagerRailName"));
    if (label == nullptr) {
        label = row->findChild<QLabel*>(QStringLiteral("settingsRailChapterName"));
    }
    REQUIRE(label != nullptr);
    return label;
}

// A rail of one chapter of 24 pages in a 300 px tall host, so it scrolls.
template <class Rail = fv::NavRail> struct ScrollingRail {
    ScrollingRail() {
        rail = new Rail;
        rail->addChapter(QStringLiteral("Chapter"));
        for (int index = 0; index < 24; ++index) {
            rail->addDestination(QStringLiteral("page%1").arg(index),
                                 QStringLiteral("Page %1").arg(index));
        }
        rail->select(QStringLiteral("page0"));
        (new QVBoxLayout(&host))->addWidget(rail);
        host.resize(fv::NavRail::kRailWidth, 300);
    }

    void assertRowsFit() const {
        CHECK(rail->verticalScrollBar()->isVisible());
        const int viewport = rail->viewport()->width();
        for (const QString& key : rail->keys()) {
            INFO(key.toStdString());
            CHECK(rail->destinationRow(key)->geometry().right() <= viewport - 1);
        }
    }

    QWidget host;
    Rail* rail = nullptr;
};

// The rail refreshes its layout after the scroll bar's value changed and before Qt moves the
// viewport's child widgets.
class RefreshingRail : public fv::NavRail {
protected:
    void scrollContentsBy(int dx, int dy) override {
        updateGeometries();
        fv::NavRail::scrollContentsBy(dx, dy);
    }
};

// Records every move of a row to a place other than its item's.
class PositionObserver : public QObject {
public:
    PositionObserver(fv::NavRail* rail, std::map<QObject*, QListWidgetItem*> items)
        : rail_(rail), items_(std::move(items)) {}

    bool eventFilter(QObject* row, QEvent* event) override {
        if (event->type() == QEvent::Move) {
            const int expected = rail_->visualItemRect(items_.at(row)).top();
            const int actual = static_cast<QMoveEvent*>(event)->pos().y();
            if (actual != expected) {
                misplaced.emplace_back(actual, expected);
            }
        }
        return false;
    }

    std::vector<std::pair<int, int>> misplaced;

private:
    fv::NavRail* rail_;
    std::map<QObject*, QListWidgetItem*> items_;
};

void clickAt(QWidget& host, QWidget* target) {
    QTest::mouseClick(host.windowHandle(), Qt::LeftButton, Qt::NoModifier,
                      target->mapTo(&host, target->rect().center()));
    QApplication::processEvents();
}

} // namespace

// from tests/test_nav_rail.py::TestNavRail::test_rows_fit_beside_the_scroll_bar_when_first_shown
TEST_CASE("rows fit beside the scroll bar when first shown", "[test_nav_rail][widget]") {
    ScrollingRail<> scrolling;
    // Settings lays the rail out while hidden, at Qt's default 640 px width.
    scrolling.rail->doItemsLayout();
    scrolling.host.show();
    QApplication::processEvents();
    scrolling.assertRowsFit();
}

// from tests/test_nav_rail.py::TestNavRail::test_rows_follow_a_narrower_rail
TEST_CASE("rows follow a narrower rail", "[test_nav_rail][widget]") {
    ScrollingRail<> scrolling;
    scrolling.host.show();
    QApplication::processEvents();
    scrolling.host.resize(fv::NavRail::kRailWidth - 20, 300);
    QApplication::processEvents();
    scrolling.assertRowsFit();
}

// from
// tests/test_nav_rail.py::TestNavRail::test_scrollbar_moves_keep_labels_over_their_click_targets
TEST_CASE("scrollbar moves keep labels over their click targets", "[test_nav_rail][widget]") {
    fvtest::AppTheme theme(QStringLiteral("dark"));
    ScrollingRail<> scrolling;
    fv::NavRail* rail = scrolling.rail;
    for (const QString& key : rail->keys()) {
        rail->setValue(key, QStringLiteral("Current setting"));
    }
    scrolling.host.show();
    QApplication::processEvents();
    QScrollBar* bar = rail->verticalScrollBar();
    for (const int value : {bar->maximum(), bar->maximum() / 2, 0}) {
        bar->setValue(value);
        QApplication::processEvents();
        for (const QString& key : rail->keys()) {
            QLabel* label = nameLabelOf(rail->destinationRow(key));
            const QPoint point = label->mapTo(rail->viewport(), label->rect().center());
            if (rail->viewport()->rect().contains(point)) {
                INFO("value " << value << " key " << key.toStdString());
                CHECK(rail->itemAt(point) == rail->destinationItem(key));
            }
        }
    }
}

// from tests/test_nav_rail.py::TestNavRail::test_scroll_geometry_refresh_keeps_rows_aligned
TEST_CASE("scroll geometry refresh keeps rows aligned", "[test_nav_rail][widget]") {
    ScrollingRail<RefreshingRail> scrolling;
    fv::NavRail* rail = scrolling.rail;
    scrolling.host.show();
    QApplication::processEvents();
    std::map<QObject*, QListWidgetItem*> itemsByRow;
    for (const QString& key : rail->keys()) {
        itemsByRow[rail->destinationRow(key)] = rail->destinationItem(key);
    }
    PositionObserver observer(rail, itemsByRow);
    for (const QString& key : rail->keys()) {
        rail->destinationRow(key)->installEventFilter(&observer);
    }
    QScrollBar* bar = rail->verticalScrollBar();
    for (const int value : {bar->maximum(), bar->maximum() / 2, 0, bar->maximum()}) {
        bar->setValue(value);
        QApplication::processEvents();
        for (const QString& key : rail->keys()) {
            INFO("value " << value << " key " << key.toStdString());
            const QRect expected = rail->visualItemRect(rail->destinationItem(key));
            CHECK(rail->destinationRow(key)->geometry().top() == expected.top());
            CHECK(rail->destinationRow(key)->geometry().bottom() == expected.bottom());
        }
        for (const QString& key : rail->keys()) {
            QLabel* label = nameLabelOf(rail->destinationRow(key));
            const QPoint point = label->mapTo(rail->viewport(), label->rect().center());
            if (!rail->viewport()->rect().contains(point)) {
                continue;
            }
            INFO("value " << value << " clicked " << key.toStdString());
            CHECK(rail->itemAt(point) == rail->destinationItem(key));
            clickAt(scrolling.host, label);
            CHECK(rail->currentKey() == key);
        }
    }
    CHECK(observer.misplaced.empty());
    for (const QString& key : rail->keys()) {
        rail->destinationRow(key)->removeEventFilter(&observer);
    }
}

namespace {

// Home, then chapter First (a, b) and chapter Second (c, d, e), in a 500 px tall host.
struct Chapters {
    Chapters() {
        rail = new fv::NavRail;
        rail->addDestination(QStringLiteral("home"), QStringLiteral("Home"));
        const std::vector<std::pair<QString, QStringList>> chapters{
            {QStringLiteral("First"), {QStringLiteral("a"), QStringLiteral("b")}},
            {QStringLiteral("Second"),
             {QStringLiteral("c"), QStringLiteral("d"), QStringLiteral("e")}}};
        for (const auto& [chapter, pages] : chapters) {
            rail->addChapter(chapter);
            for (const QString& key : pages) {
                rail->addDestination(key, key.toUpper());
            }
        }
        (new QVBoxLayout(&host))->addWidget(rail);
        host.resize(fv::NavRail::kRailWidth, 500);
        host.show();
        rail->select(QStringLiteral("home"));
        QApplication::processEvents();
        QObject::connect(rail, &fv::NavRail::destinationChanged,
                         [this](const QString& key) { changes << key; });
    }

    QStringList visible() const {
        QStringList shown;
        for (const QString& key : rail->keys()) {
            if (!rail->destinationItem(key)->isHidden()) {
                shown << key;
            }
        }
        return shown;
    }

    void clickChapter(const QString& title) { clickAt(host, nameLabelOf(rail->chapterRow(title))); }

    std::map<QString, int> headingTops() const {
        std::map<QString, int> tops;
        for (const QString& title : rail->chapters()) {
            tops[title] = rail->visualItemRect(rail->chapterItem(title)).top();
        }
        return tops;
    }

    QStringList active() const {
        QStringList titles;
        for (const QString& title : rail->chapters()) {
            if (rail->chapterRow(title)->property(fv::ui::prop::active).toBool()) {
                titles << title;
            }
        }
        return titles;
    }

    QWidget host;
    fv::NavRail* rail = nullptr;
    QStringList changes;
};

QStringList keys(std::initializer_list<const char*> names) {
    QStringList list;
    for (const char* name : names) {
        list << QString::fromLatin1(name);
    }
    return list;
}

} // namespace

// from tests/test_nav_rail.py::TestChapters::test_opening_a_chapter_leaves_the_others_open
TEST_CASE("opening a chapter leaves the others open", "[test_nav_rail][widget]") {
    Chapters chapters;
    CHECK(chapters.visible() == keys({"home"}));
    chapters.rail->select(QStringLiteral("d"));
    CHECK(chapters.visible() == keys({"home", "c", "d", "e"}));
    chapters.rail->select(QStringLiteral("a"));
    CHECK(chapters.visible() == keys({"home", "a", "b", "c", "d", "e"}));
    chapters.rail->select(QStringLiteral("home"));
    CHECK(chapters.visible() == keys({"home", "a", "b", "c", "d", "e"}));
}

// from tests/test_nav_rail.py::TestChapters::test_a_clicked_heading_stays_under_the_pointer
TEST_CASE("a clicked heading stays under the pointer", "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("First"));
    const int before = chapters.headingTops().at(QStringLiteral("Second"));
    chapters.clickChapter(QStringLiteral("Second"));
    CHECK(chapters.headingTops().at(QStringLiteral("Second")) == before);
    CHECK(chapters.rail->currentKey() == QStringLiteral("c"));
}

// from tests/test_nav_rail.py::TestChapters::test_the_current_pages_chapter_carries_the_pill
TEST_CASE("the current pages chapter carries the pill", "[test_nav_rail][widget]") {
    Chapters chapters;
    CHECK(chapters.active().isEmpty());
    chapters.clickChapter(QStringLiteral("Second"));
    CHECK(chapters.active() == keys({"Second"}));
    CHECK(chapters.rail->destinationRow(QStringLiteral("c"))
              ->property(fv::ui::prop::selected)
              .toBool());
    chapters.rail->select(QStringLiteral("a"));
    CHECK(chapters.active() == keys({"First"}));
    chapters.rail->select(QStringLiteral("home"));
    CHECK(chapters.active().isEmpty());
}

// from
// tests/test_nav_rail.py::TestChapters::test_clicking_the_chapter_you_are_in_folds_it_and_keeps_its_pill
TEST_CASE("clicking the chapter you are in folds it and keeps its pill",
          "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("First"));
    chapters.clickChapter(QStringLiteral("First"));
    CHECK_FALSE(chapters.rail->isExpanded(QStringLiteral("First")));
    CHECK(chapters.visible() == keys({"home"}));
    CHECK(chapters.rail->currentKey() == QStringLiteral("a"));
    CHECK(chapters.active() == keys({"First"}));
    chapters.clickChapter(QStringLiteral("First"));
    CHECK(chapters.rail->isExpanded(QStringLiteral("First")));
    CHECK(chapters.changes == keys({"a"}));
}

// from
// tests/test_nav_rail.py::TestChapters::test_clicking_an_open_chapter_you_are_not_in_goes_there
TEST_CASE("clicking an open chapter you are not in goes there", "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("First"));
    chapters.rail->select(QStringLiteral("home"));
    chapters.clickChapter(QStringLiteral("First"));
    CHECK(chapters.rail->isExpanded(QStringLiteral("First")));
    CHECK(chapters.rail->currentKey() == QStringLiteral("a"));
}

// from tests/test_nav_rail.py::TestChapters::test_a_chapter_click_reopens_its_last_page
TEST_CASE("a chapter click reopens its last page", "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("Second"));
    CHECK(chapters.rail->currentKey() == QStringLiteral("c"));
    chapters.rail->select(QStringLiteral("e"));
    chapters.clickChapter(QStringLiteral("First"));
    CHECK(chapters.rail->currentKey() == QStringLiteral("a"));
    chapters.clickChapter(QStringLiteral("Second"));
    CHECK(chapters.rail->currentKey() == QStringLiteral("e"));
    CHECK(chapters.changes == keys({"c", "e", "a", "e"}));
}

// from tests/test_nav_rail.py::TestChapters::test_a_chapter_heading_is_never_current
TEST_CASE("a chapter heading is never current", "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("First"));
    CHECK(chapters.rail->chapter(chapters.rail->currentKey()) == QStringLiteral("First"));
    REQUIRE(chapters.rail->currentItem() != nullptr);
    CHECK(chapters.rail->currentItem()->data(Qt::UserRole).isValid());
}

// from tests/test_nav_rail.py::TestChapters::test_arrow_keys_walk_every_page_and_open_chapters
TEST_CASE("arrow keys walk every page and open chapters", "[test_nav_rail][widget]") {
    Chapters chapters;
    fv::NavRail* rail = chapters.rail;
    rail->setFocus();
    std::vector<std::pair<QString, QString>> walked;
    for (int i = 0; i < 6; ++i) {
        QTest::keyClick(rail, Qt::Key_Down);
        walked.emplace_back(rail->currentKey(), rail->activeChapter());
    }
    const QString first = QStringLiteral("First");
    const QString second = QStringLiteral("Second");
    CHECK(walked == std::vector<std::pair<QString, QString>>{{QStringLiteral("a"), first},
                                                             {QStringLiteral("b"), first},
                                                             {QStringLiteral("c"), second},
                                                             {QStringLiteral("d"), second},
                                                             {QStringLiteral("e"), second},
                                                             {QStringLiteral("e"), second}});
    QTest::keyClick(rail, Qt::Key_Up);
    QTest::keyClick(rail, Qt::Key_Up);
    QTest::keyClick(rail, Qt::Key_Up);
    CHECK(rail->currentKey() == QStringLiteral("b"));
    CHECK(rail->activeChapter() == first);
    QTest::keyClick(rail, Qt::Key_Home);
    CHECK(rail->currentKey() == QStringLiteral("home"));
    CHECK(rail->activeChapter().isEmpty());
}

// from tests/test_nav_rail.py::TestChapters::test_arrow_keys_step_out_of_a_folded_chapter
TEST_CASE("arrow keys step out of a folded chapter", "[test_nav_rail][widget]") {
    Chapters chapters;
    chapters.clickChapter(QStringLiteral("First"));
    chapters.clickChapter(QStringLiteral("First"));
    chapters.rail->setFocus();
    QTest::keyClick(chapters.rail, Qt::Key_Down);
    CHECK(chapters.rail->currentKey() == QStringLiteral("b"));
    CHECK(chapters.rail->isExpanded(QStringLiteral("First")));
}

// from tests/test_nav_rail.py::TestChapters::test_a_first_value_grows_a_closed_chapters_row
TEST_CASE("a first value grows a closed chapters row", "[test_nav_rail][widget]") {
    Chapters chapters;
    QWidget* row = chapters.rail->destinationRow(QStringLiteral("c"));
    QListWidgetItem* item = chapters.rail->destinationItem(QStringLiteral("c"));
    const int single = item->sizeHint().height();
    chapters.rail->setValue(QStringLiteral("c"), QStringLiteral("Current setting"));
    CHECK(item->sizeHint().height() > single);
    chapters.rail->select(QStringLiteral("c"));
    QApplication::processEvents();
    CHECK(row->geometry() == chapters.rail->visualItemRect(item));
}

TEST_CASE("page keys, End and the rail's lookups", "[widget]") {
    Chapters chapters;
    fv::NavRail* rail = chapters.rail;
    CHECK(rail->objectName() == QStringLiteral("modelManagerRail"));
    CHECK(rail->minimumWidth() == fv::NavRail::kRailMinWidth);
    CHECK(rail->keys() == keys({"home", "a", "b", "c", "d", "e"}));
    CHECK(rail->chapters() == keys({"First", "Second"}));
    CHECK(rail->chapterKeys(QStringLiteral("Second")) == keys({"c", "d", "e"}));
    CHECK(rail->chapterKeys(QStringLiteral("None")).isEmpty());
    CHECK(rail->name(QStringLiteral("d")) == QStringLiteral("D"));
    CHECK(rail->chapter(QStringLiteral("home")).isEmpty());
    rail->setValue(QStringLiteral("d"), QStringLiteral("A value long enough to need eliding "
                                                       "in a rail of this width"));
    CHECK(rail->value(QStringLiteral("d")) ==
          QStringLiteral("A value long enough to need eliding in a rail of this width"));
    CHECK(rail->value(QStringLiteral("missing")).isEmpty());
    rail->setFocus();
    QTest::keyClick(rail, Qt::Key_PageDown);
    CHECK(rail->currentKey() == QStringLiteral("e"));
    QTest::keyClick(rail, Qt::Key_PageUp);
    CHECK(rail->currentKey() == QStringLiteral("home"));
    QTest::keyClick(rail, Qt::Key_End);
    CHECK(rail->currentKey() == QStringLiteral("e"));
}

TEST_CASE("dragging across the rail selects nothing", "[widget]") {
    Chapters chapters;
    fv::NavRail* rail = chapters.rail;
    rail->select(QStringLiteral("a"));
    QApplication::processEvents();
    QWidget* from = nameLabelOf(rail->destinationRow(QStringLiteral("a")));
    QWidget* to = nameLabelOf(rail->destinationRow(QStringLiteral("b")));
    QWindow* window = chapters.host.windowHandle();
    QTest::mousePress(window, Qt::LeftButton, Qt::NoModifier,
                      from->mapTo(&chapters.host, from->rect().center()));
    QTest::mouseMove(window, to->mapTo(&chapters.host, to->rect().center()));
    QTest::mouseRelease(window, Qt::LeftButton, Qt::NoModifier,
                        to->mapTo(&chapters.host, to->rect().center()));
    QApplication::processEvents();
    CHECK(rail->currentKey() == QStringLiteral("a"));
}
