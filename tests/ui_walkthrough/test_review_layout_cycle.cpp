#include <catch2/catch_test_macros.hpp>

#include <algorithm>

#include <QScopeGuard>
#include <QScrollArea>
#include <QScrollBar>

#include "tab_support.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

namespace fvtest {
namespace {

class ResizeCounter final : public QObject {
public:
    int count = 0;

protected:
    bool eventFilter(QObject*, QEvent* event) override {
        if (event->type() == QEvent::Resize) {
            ++count;
        }
        return false;
    }
};

} // namespace

TEST_CASE("the storyboard scroll range settles as text and previews reflow", "[widget]") {
    fv::testkit::TempDataRoot scratch;
    const auto root = scratch.path() / "captures";
    const auto name = "2026-10-20_14-32-05";
    std::filesystem::create_directories(root);
    std::filesystem::copy(fv::testkit::source_root() / "tests/fixtures/walkthrough" / name,
                          root / name, std::filesystem::copy_options::recursive);
    fv::ui::ThemeManager theme(QStringLiteral("dark"));
    fv::ui::applyUiTheme(QStringLiteral("dark"), *qApp, theme);
    const auto restore = qScopeGuard([&] {
        fv::ui::applyUiFontScale(100, *qApp, theme);
        qApp->setStyleSheet(QString());
    });
    for (int scale : {100, 130}) {
        fv::ui::applyUiFontScale(scale, *qApp, theme);
        TabRig rig({.root = root});
        rig.settle();
        rig->setFixedSize(1100, 760);
        rig->show();
        rig.settle();
        auto* review = rig->review();
        REQUIRE(review->rows().size() == 3);
        ResizeCounter resizes;
        review->board()->installEventFilter(&resizes);
        for (int width = 1000; width <= 1200; width += 4) {
            for (int height = 700; height <= 820; height += 20) {
                CAPTURE(scale, width, height);
                resizes.count = 0;
                rig->setFixedSize(width, height);
                rig.settle();
                // Allow both scrollbar widths and follow-up layouts of the three rows.
                REQUIRE(resizes.count <= 10);
                const int settled = resizes.count;
                rig.settle();
                REQUIRE(resizes.count == settled);
                const auto* scroll = review->scroll();
                CHECK(scroll->verticalScrollBar()->maximum() ==
                      std::max(0, review->board()->height() - scroll->viewport()->height()));
            }
        }
    }
}

} // namespace fvtest
