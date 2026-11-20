// The a32 checklist's ST-02 hover claim: pointing at a day of the words chart shows that day's
// words (stats_charts.py BarChart.mouseMoveEvent's tooltip).
#include <catch2/catch_test_macros.hpp>

#include <memory>

#include <QApplication>
#include <QMouseEvent>
#include <QToolTip>

#include "history_ui_support.h"
#include "stats_test_support.h"
#include "ui_history/stats_charts.h"
#include "ui_history/stats_dialog.h"

using fv::StatsDialog;
using fvtest::LocalStats;

TEST_CASE("a32 ST-02: hovering a day of the words chart names it and its words", "[widget]") {
    LocalStats stats;
    fvtest::SyncIo io;
    fvtest::seedThreeDays(stats);
    auto dialog = std::make_unique<StatsDialog>(stats.reads(), io);
    dialog->show();
    QApplication::processEvents();
    fv::BarChart* bars = dialog->bars();
    REQUIRE(bars != nullptr);
    REQUIRE_FALSE(bars->bars().empty());
    const fv::ChartBar today = bars->bars().back();
    REQUIRE(today.words == 7);

    // From the right edge inwards until the pointer is over the last bar.
    for (int x = bars->width() - 1; x >= 0 && !QToolTip::isVisible(); --x) {
        const QPointF local(x, bars->height() / 2.0);
        QMouseEvent move(QEvent::MouseMove, local, bars->mapToGlobal(local), Qt::NoButton,
                         Qt::NoButton, Qt::NoModifier);
        QApplication::sendEvent(bars, &move);
    }
    REQUIRE(QToolTip::isVisible());
    CHECK(QToolTip::text() == today.name + QStringLiteral(" · 7 words"));
    QToolTip::hideText();
    dialog->close();
}
