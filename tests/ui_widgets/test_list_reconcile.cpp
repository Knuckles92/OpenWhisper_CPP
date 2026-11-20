// reconcileCards: the widget side of ui_qt/utils/list_reconcile.py (the decisions are
// reconcile_plan's, replayed against golden vectors in tests/uimath).
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLabel>
#include <QPointer>
#include <QVBoxLayout>
#include <QWidget>

#include <string>
#include <vector>

#include "ui_widgets/list_reconcile.h"

namespace {

struct Record {
    std::string id;
    int version;
};

struct List {
    List() : layout(&host) {}

    // Shows records, as a history list does.
    void show(const std::vector<Record>& records) {
        std::vector<fv::ReconcileItem> items;
        for (const Record& record : records) {
            items.push_back({record.id, record.version});
        }
        created.clear();
        refreshed.clear();
        table = fv::reconcileCards(
            &layout, items, table,
            [&](std::size_t index) {
                auto* card = new QLabel(QString::fromStdString(records[index].id));
                created.push_back(records[index].id);
                return card;
            },
            [&](QWidget* card, std::size_t index) {
                refreshed.push_back(records[index].id);
                static_cast<QLabel*>(card)->setToolTip(QString::number(records[index].version));
            });
    }

    // The ids of the cards in the layout, in order; "-" for anything else.
    std::vector<std::string> order() const {
        std::vector<std::string> ids;
        for (int i = 0; i < layout.count(); ++i) {
            const auto* label = qobject_cast<QLabel*>(layout.itemAt(i)->widget());
            ids.push_back(label != nullptr ? label->text().toStdString() : "-");
        }
        return ids;
    }

    QWidget host;
    QVBoxLayout layout;
    fv::CardTable table;
    std::vector<std::string> created;
    std::vector<std::string> refreshed;
};

using Ids = std::vector<std::string>;

} // namespace

TEST_CASE("the first list creates a card per record in order", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}, {"c", 1}});
    CHECK(list.order() == Ids{"a", "b", "c"});
    CHECK(list.created == Ids{"a", "b", "c"});
    CHECK(list.refreshed.empty());
    CHECK(list.table.size() == 3);
    CHECK(list.table.at("b").fingerprint == fv::Json(1));
}

TEST_CASE("an unchanged list keeps every card and refreshes it", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}});
    QWidget* first = list.table.at("a").card;
    QWidget* second = list.table.at("b").card;

    list.show({{"a", 1}, {"b", 1}});
    CHECK(list.created.empty());
    CHECK(list.refreshed == Ids{"a", "b"});
    CHECK(list.table.at("a").card == first);
    CHECK(list.table.at("b").card == second);
    CHECK(list.order() == Ids{"a", "b"});
}

TEST_CASE("a changed fingerprint replaces only that card", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}, {"c", 1}});
    QWidget* a = list.table.at("a").card;
    const QPointer<QWidget> b = list.table.at("b").card;
    QWidget* c = list.table.at("c").card;

    list.show({{"a", 1}, {"b", 2}, {"c", 1}});
    CHECK(list.created == Ids{"b"});
    CHECK(list.table.at("a").card == a);
    CHECK(list.table.at("c").card == c);
    CHECK(list.table.at("b").card != b.data());
    CHECK(list.table.at("b").fingerprint == fv::Json(2));
    CHECK(list.order() == Ids{"a", "b", "c"});

    // The old card is hidden and unparented at once, and deleted when events run.
    CHECK(b->parentWidget() == nullptr);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(b.isNull());
}

TEST_CASE("records that disappear lose their cards", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}});
    const QPointer<QWidget> a = list.table.at("a").card;
    const QPointer<QWidget> c = list.table.at("c").card;
    list.show({{"b", 1}, {"d", 1}});
    CHECK(list.order() == Ids{"b", "d"});
    CHECK(list.table.size() == 2);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    CHECK(a.isNull());
    CHECK(c.isNull());
}

TEST_CASE("a new record is inserted where it belongs", "[widget]") {
    List list;
    list.show({{"a", 1}, {"c", 1}});
    list.show({{"a", 1}, {"b", 1}, {"c", 1}});
    CHECK(list.order() == Ids{"a", "b", "c"});
    CHECK(list.created == Ids{"b"});
}

TEST_CASE("a reordered list moves its cards", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}, {"c", 1}});
    QWidget* a = list.table.at("a").card;
    list.show({{"c", 1}, {"a", 1}, {"b", 1}});
    CHECK(list.created.empty());
    CHECK(list.order() == Ids{"c", "a", "b"});
    CHECK(list.table.at("a").card == a);
}

TEST_CASE("a stretch at the end is removed with the stale cards", "[widget]") {
    List list;
    list.show({{"a", 1}});
    list.layout.addStretch();
    REQUIRE(list.order() == Ids{"a", "-"});
    list.show({{"a", 1}});
    CHECK(list.order() == Ids{"a"});
}

TEST_CASE("an empty list clears the layout", "[widget]") {
    List list;
    list.show({{"a", 1}, {"b", 1}});
    list.show({});
    CHECK(list.order().empty());
    CHECK(list.table.empty());
}

TEST_CASE("fingerprints compare by Python equality", "[widget]") {
    List list;
    list.show({{"a", 1}});
    QWidget* card = list.table.at("a").card;
    // 1 == 1.0 == true in Python, so these keep the card.
    std::vector<fv::ReconcileItem> items{{"a", 1.0}};
    auto table = fv::reconcileCards(&list.layout, items, list.table,
                                    [](std::size_t) -> QWidget* { return new QLabel("x"); });
    CHECK(table.at("a").card == card);
}
