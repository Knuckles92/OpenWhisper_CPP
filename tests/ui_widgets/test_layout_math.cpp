// The layout decisions of ButtonRow, SegmentedBar and FlowLayout (ui_widgets/layout_math.h),
// replayed against OpenWhisper's _row_sizes, SegmentedBar._reflow and FlowLayout._arrange
// (tests/golden/ui_qt.widgets.*.json, tools/parity/cases/ui_qt.widgets.*.py).
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <vector>

#include "golden/golden.h"
#include "ui_widgets/layout_math.h"

namespace golden = fv::golden;

TEST_CASE("button_row_sizes replays ButtonRow._row_sizes", "[golden]") {
    golden::for_each("ui_qt.widgets.button_row", "_row_sizes", [](const golden::Case& c) {
        INFO(c.label());
        const auto widths = c.arg<std::vector<int>>(0);
        CHECK(fv::button_row_sizes(widths, c.arg<int>(1), c.arg<int>(2)) ==
              c.result<std::vector<int>>());
    });
}

TEST_CASE("segmented_columns replays SegmentedBar._reflow", "[golden]") {
    golden::for_each("ui_qt.widgets.segmented_bar", "_reflow", [](const golden::Case& c) {
        INFO(c.label());
        const int n = c.arg<int>(0);
        const int cell = c.arg<int>(1);
        const int gap = c.arg<int>(2);
        const int width = c.arg<int>(3);
        // The stand-in tray's margin on each side is its gap, as the grid's are.
        const fv::SegmentedShape shape = fv::segmented_columns(n, cell, gap, width - 2 * gap + gap);
        const fv::Json result = c.result<fv::Json>();
        CHECK(shape.columns == result.at("columns").get<int>());
        CHECK(shape.wrap == result.at("wrap").get<bool>());
        // What each choice's minimum width becomes.
        CHECK((shape.wrap ? 0 : cell) == result.at("minimum").get<int>());
    });
}

TEST_CASE("flow_arrange replays FlowLayout._arrange", "[golden]") {
    golden::for_each("ui_qt.widgets.flow_layout", "_arrange", [](const golden::Case& c) {
        INFO(c.label());
        // Empty items are left out before arranging, as FlowLayout does, and keep no geometry.
        const auto sizes = c.arg<std::vector<std::optional<std::vector<int>>>>(0);
        std::vector<fv::Size> shown;
        for (const auto& size : sizes) {
            if (size) {
                shown.push_back({size->at(0), size->at(1)});
            }
        }
        const int width = c.arg<int>(1);
        const int spacing = c.arg<int>(2);
        const fv::Json result = c.result<fv::Json>();
        CHECK(fv::flow_height(shown, width, spacing) == result.at("height").get<int>());
        const std::vector<fv::Rect> rects = fv::flow_arrange(shown, width, spacing);
        REQUIRE(rects.size() == shown.size());
        std::size_t next = 0;
        const fv::Json& expected = result.at("rects");
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (!sizes[i]) {
                CHECK(expected.at(i).is_null());
                continue;
            }
            const fv::Rect& r = rects[next++];
            CHECK(std::vector<int>{r.x, r.y, r.width, r.height} ==
                  expected.at(i).get<std::vector<int>>());
        }
    });
}

TEST_CASE("three buttons wrap one over two, the short row first", "[layout_math]") {
    const std::vector<int> widths{90, 80, 70};
    CHECK(fv::button_row_sizes(widths, 256, 8) == std::vector<int>{3});
    CHECK(fv::button_row_sizes(widths, 200, 8) == std::vector<int>{1, 2});
    CHECK(fv::button_row_sizes(widths, 100, 8) == std::vector<int>{1, 1, 1});
    CHECK(fv::button_row_sizes(widths, 10, 8) == std::vector<int>{1, 1, 1});
    CHECK(fv::button_row_sizes({}, 300, 8).empty());
}

TEST_CASE("four choices wrap two and two, never three and one", "[layout_math]") {
    // room = width - margins + gap; three fit per row, so the rows are balanced to 2 + 2.
    CHECK(fv::segmented_columns(4, 100, 4, 3 * 104) == fv::SegmentedShape{2, false});
    CHECK(fv::segmented_columns(4, 100, 4, 4 * 104) == fv::SegmentedShape{4, false});
    CHECK(fv::segmented_columns(4, 100, 4, 103) == fv::SegmentedShape{1, true});
    CHECK(fv::segmented_columns(0, 100, 4, 500) == fv::SegmentedShape{});
}
