// The History card's chip row measures its chips once per change, not on every layout query
// (tests/test_personalize_fix2_g3_chip_flow.py). OpenWhisper counts ElidingLabel.sizeHint calls by
// patching the class; here a chip that counts its own sizeHint calls stands in, and the card's
// own chips are checked through the geometry the flow gives them.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include <QApplication>
#include <QEvent>
#include <QFont>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include <QWidget>

#include "history_ui_support.h"
#include "ui_history/chip_flow.h"
#include "ui_history/history_item.h"
#include "ui_widgets/eliding_label.h"

namespace {

// Every sizeHint() call of every CountingChip, which is what measuring a chip costs.
std::vector<const QWidget*>& measured() {
    static std::vector<const QWidget*> calls;
    return calls;
}

class CountingChip : public fv::ElidingLabel {
public:
    explicit CountingChip(const QString& text) : fv::ElidingLabel(text) {}
    QSize sizeHint() const override {
        measured().push_back(this);
        return fv::ElidingLabel::sizeHint();
    }
};

CountingChip* chip(const QString& text) {
    auto* made = new CountingChip(text);
    made->setFont(QFont(QStringLiteral("Segoe UI"), 9));
    made->setFixedHeight(20);
    return made;
}

fv::ChipFlow* flowOf(const std::vector<QString>& texts) {
    std::vector<QWidget*> chips;
    for (const QString& text : texts) {
        chips.push_back(chip(text));
    }
    return new fv::ChipFlow(std::move(chips));
}

// Lets queued layout requests run, as the Python test's _settle does.
void settle() {
    for (int i = 0; i < 3; ++i) {
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    }
}

// `widget` at `width`, given exactly the height it asks for; the host owns it.
std::unique_ptr<QWidget> hosted(QWidget* widget, int width) {
    auto host = std::make_unique<QWidget>();
    auto* layout = new QVBoxLayout(host.get());
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(widget);
    layout->addStretch(1);
    host->resize(width, 400);
    host->show();
    settle();
    return host;
}

// A chip is an ElidingLabel: setText goes through its own, which keeps the full text.
fv::ElidingLabel* label(QWidget* widget) {
    return static_cast<fv::ElidingLabel*>(widget);
}

} // namespace

// from tests/test_personalize_fix2_g3_chip_flow.py::test_layout_queries_reuse_the_measurement
TEST_CASE("layout queries reuse the measurement", "[test_personalize_fix2_g3_chip_flow][widget]") {
    std::unique_ptr<fv::ChipFlow> flow(
        flowOf({QStringLiteral("Polish"), QStringLiteral(u"✦ Medium · gpt-4o-mini"),
                QStringLiteral("Slack")}));
    const int wide = flow->heightForWidth(2000);
    const int narrow = flow->heightForWidth(120);
    CHECK(narrow > wide);
    measured().clear();
    for (int i = 0; i < 5; ++i) {
        CHECK(flow->heightForWidth(2000) == wide);
        CHECK(flow->heightForWidth(120) == narrow);
        flow->sizeHint();
        flow->minimumSizeHint();
    }
    CHECK(measured().empty());
}

// from tests/test_personalize_fix2_g3_chip_flow.py::test_a_chip_that_grows_is_measured_again
TEST_CASE("a chip that grows is measured again", "[test_personalize_fix2_g3_chip_flow][widget]") {
    fv::ChipFlow* flow = flowOf({QStringLiteral("Polish"), QStringLiteral("Slack")});
    const auto host = hosted(flow, 200);
    const int line = flow->minimumSizeHint().height();
    CHECK(flow->height() == line);

    label(flow->chips()[1])
        ->setText(QStringLiteral(u"Google Docs — Q3 planning notes and the long tail"));
    settle();
    CHECK(flow->heightForWidth(200) == 2 * line + fv::ChipFlow::kSpacing);
    CHECK(flow->height() == 2 * line + fv::ChipFlow::kSpacing);
}

// from tests/test_personalize_fix2_g3_chip_flow.py::
//      test_a_chip_that_grows_within_its_line_gets_the_room
TEST_CASE("a chip that grows within its line gets the room",
          "[test_personalize_fix2_g3_chip_flow][widget]") {
    fv::ChipFlow* flow = flowOf({QStringLiteral("Polish"), QStringLiteral("Slack")});
    const auto host = hosted(flow, 400);
    const int height = flow->height();

    fv::ElidingLabel* grown = label(flow->chips()[1]);
    grown->setText(QStringLiteral("Slack huddle"));
    settle();
    CHECK(flow->height() == height);
    CHECK(grown->width() == grown->sizeHint().width());
    // The shown text, not the full one: nothing was elided.
    CHECK(grown->QLabel::text() == QStringLiteral("Slack huddle"));
}

// from tests/test_personalize_fix2_g3_chip_flow.py::
//      test_a_chip_changed_while_hidden_is_measured_again_when_shown
TEST_CASE("a chip changed while hidden is measured again when shown",
          "[test_personalize_fix2_g3_chip_flow][widget]") {
    fv::ChipFlow* flow = flowOf({QStringLiteral("Polish"), QStringLiteral("Slack")});
    const auto host = hosted(flow, 200);
    const int line = flow->height();

    host->hide();
    flow->chips()[1]->setFont(QFont(QStringLiteral("Segoe UI"), 30));
    host->show();
    settle();
    CHECK(flow->heightForWidth(200) > line);
    CHECK(flow->height() > line);
}

// from tests/test_personalize_fix2_g3_chip_flow.py::
//      test_the_arrangement_matches_the_chips_size_hints
TEST_CASE("the arrangement matches the chips' size hints",
          "[test_personalize_fix2_g3_chip_flow][widget]") {
    fv::NewEntry fields;
    fields.text = "Polished.";
    fields.model = "base";
    fields.source_name = "Polish";
    fields.raw_text = "draft";
    fields.context = {{"entry_kind", "transform"},
                      {"cleanup_level", "medium"},
                      {"app_name", "Google Docs — Q3 planning notes"}};
    fv::HistoryEntry entry = fv::create_entry(fields);
    entry.cleanup_model = "gpt-4o-mini";
    fvtest::FakeStore store;
    auto* card = new fv::HistoryItemWidget(entry, store);
    const auto host = hosted(card, 240);

    for (const int width : {240, 420, 900, 300}) {
        host->resize(width, 400);
        settle();
        const fv::ChipFlow* flow = card->chips();
        int x = 0;
        int y = 0;
        int line = 0;
        for (const QWidget* each : flow->chips()) {
            line = std::max(line, each->sizeHint().height());
        }
        for (const QWidget* each : flow->chips()) {
            const int chipWidth = std::min(each->sizeHint().width(), flow->width());
            if (x != 0 && x + chipWidth > flow->width()) {
                x = 0;
                y += line + fv::ChipFlow::kSpacing;
            }
            INFO("width " << width);
            CHECK(each->geometry() == QRect(x, y, chipWidth, line));
            x += chipWidth + fv::ChipFlow::kSpacing;
        }
        CHECK(flow->height() == y + line);
    }
}

// from tests/test_personalize_fix2_g3_chip_flow.py::
//      test_adding_a_card_to_a_long_list_does_not_remeasure_the_others
// What every finished dictation does to an open History list. The counted chips sit in cards of
// the same shape (a chip row above a text line) in a scrolling column, as the sidebar holds
// them.
TEST_CASE("adding a card to a long list does not remeasure the others",
          "[test_personalize_fix2_g3_chip_flow][widget]") {
    const auto card = [](int index) {
        auto* made = new QWidget;
        auto* column = new QVBoxLayout(made);
        column->addWidget(
            flowOf({QStringLiteral(u"✦ Medium · gpt-4o-mini"), QStringLiteral("Slack")}));
        column->addWidget(new QLabel(QStringLiteral("Note %1.").arg(index)));
        return made;
    };

    auto* content = new QWidget;
    auto* column = new QVBoxLayout(content);
    for (int index = 0; index < 30; ++index) {
        column->addWidget(card(index));
    }
    QScrollArea area;
    area.setWidgetResizable(true);
    area.setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    area.setWidget(content);
    area.resize(380, 900);
    area.show();
    settle();

    measured().clear();
    column->insertWidget(0, card(30));
    settle();
    // The new card's two chips, a few times over; not 30 cards' worth.
    CHECK(measured().size() <= 12);
}
