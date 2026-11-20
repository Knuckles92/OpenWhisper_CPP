// The golden vectors of ui_qt.overlays.waveform_overlay's layout methods, and the geometry
// cases of tests/test_streaming_overlay_layout.py that need no widget: the header clear of the
// badge and chip, shorter than the footer, the footer's margins, Snap and Done giving the
// header's clock a line of its own, the compact look under the badge row.
#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <vector>

#include "overlay_test_support.h"
#include "uimath/overlay_layout.h"
#include "uimath/overlay_look.h"

using fv::Json;
using fv::overlay_test::matches_result;

namespace {

constexpr char kModule[] = "ui_qt.overlays.waveform_overlay";

template <class T> T get(const Json& params, const char* key, T fallback) {
    return params.contains(key) && !params.at(key).is_null() ? params.at(key).get<T>() : fallback;
}

std::vector<int> ints(const Json& list) {
    return fv::golden::decode<std::vector<int>>(list);
}

fv::Rect rect_of(const Json& list) {
    const auto v = ints(list);
    return fv::Rect{v[0], v[1], v[2], v[3]};
}

std::optional<fv::Rect> optional_rect(const Json& params, const char* key) {
    if (!params.contains(key) || params.at(key).is_null()) {
        return std::nullopt;
    }
    return rect_of(params.at(key));
}

Json rect_json(const fv::Rect& r) {
    return Json::array({r.x, r.y, r.width, r.height});
}

Json rect_json(const fv::RectD& r) {
    return Json::array({r.x, r.y, r.width, r.height});
}

// What the overlay is doing, as the case file's stub sees it.
struct Scene {
    explicit Scene(const Json& case_params) : params(case_params) {
        const std::string state = get<std::string>(params, "state", "recording");
        listening = state == "recording" || state == "streaming" || state == "command_listening";
        const bool destination = !get<std::string>(params, "destination", "").empty();
        badge = destination ||
                (listening && (get(params, "hands_free", false) || state == "command_listening"));
        chip = !get<std::string>(params, "language", "").empty() &&
               params.value("choices", Json::array()).size() >= 2;
        controls = params.contains("controls") && !params.at("controls").is_null();
        dot = get(params, "dot", true);
        clock = get(params, "clock", true);
        live = get(params, "live", true);
        const auto widths = fv::golden::decode<std::vector<double>>(
            params.value("label_widths", Json::array({40.0, 12.0})));
        with_clock = widths[0];
        without_clock = widths[1];
    }

    fv::TopRowInput top_row() const {
        fv::TopRowInput in;
        in.badge_shown = badge;
        in.badge_width = get(params, "badge_width", 60.0);
        in.listening = listening;
        in.chip_visible = chip;
        in.chip_width = get(params, "chip_width", 30.0);
        in.controls_clickable = controls;
        in.controls_left = controls ? params.at("controls").get<double>() : 0.0;
        return in;
    }

    // The live label's width at its widest, when a live look draws one.
    std::optional<double> label() const {
        if (live && (dot || clock)) {
            return clock ? with_clock : without_clock;
        }
        return std::nullopt;
    }

    fv::PreviewLayout layout() const {
        return get<std::string>(params, "layout", "header") == "footer" ? fv::PreviewLayout::Footer
                                                                        : fv::PreviewLayout::Header;
    }

    Json params;
    bool listening = false;
    bool badge = false;
    bool chip = false;
    bool controls = false;
    bool dot = true;
    bool clock = true;
    bool live = true;
    double with_clock = 0.0;
    double without_clock = 0.0;
};

} // namespace

TEST_CASE("the overlay's layout replays the golden vectors", "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "layout", [](const fv::golden::Case& c) {
        const Json& params = c.args().at(0);
        const Scene scene(params);
        const fv::TopRowInput top = scene.top_row();
        const int base = get(params, "base_height", 80);
        const int height = get(params, "height", 80);
        const bool words = get(params, "words", false);
        const int header = fv::header_height(top, scene.label());
        const fv::Margins margins =
            fv::preview_text_margins(scene.layout(), base, fv::top_row_in_use(top), header);
        const fv::Span span = fv::top_row_span(top);
        const int band = fv::band_top(top);
        const fv::Rect text = fv::preview_text_rect(fv::kOverlayWidth, height, margins);
        // The stub's header room is empty until a case says otherwise.
        const auto room = fv::golden::decode<std::vector<double>>(
            params.value("room", Json::array({0.0, 0.0, 0.0, 0.0})));
        const fv::Rect status =
            fv::status_rect(words, scene.layout(), fv::kOverlayWidth, height, band, base,
                            fv::RectD{room[0], room[1], room[2], room[3]});
        Json divider = nullptr;
        if (words) {
            divider = fv::divider_y(scene.layout(), height, base, header);
        }
        const Json actual{{"in_use", fv::top_row_in_use(top)},
                          {"band_top", band},
                          {"span", Json::array({span.left, span.right})},
                          {"shares", fv::live_shares_top_row(top, scene.label())},
                          {"header_height", header},
                          {"margins", Json::array({margins.above, margins.below})},
                          {"text_rect", rect_json(text)},
                          {"divider", divider},
                          {"status", rect_json(status)}};
        CHECK(matches_result(c, actual));
    });
}

TEST_CASE("the header row replays the golden vectors", "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "header_row", [](const fv::golden::Case& c) {
        const Json& params = c.args().at(0);
        const Scene scene(params);
        const fv::TopRowInput top = scene.top_row();
        fv::HeaderRowInput in;
        in.width = fv::kOverlayWidth;
        in.shares_top_row = fv::live_shares_top_row(top, scene.label());
        in.top_row_span = fv::top_row_span(top);
        in.dot = scene.dot;
        in.clock = scene.clock;
        in.live = scene.live;
        in.label_width = scene.clock ? scene.with_clock : scene.without_clock;
        in.label_width_without_clock = scene.without_clock;
        in.caption = get(params, "caption", false);
        const fv::HeaderRow row = fv::header_row(in);
        const Json actual{{"label", row.draw_label
                                        ? Json::array({rect_json(row.label_rect), row.label_clock})
                                        : Json(nullptr)},
                          {"room", rect_json(row.room)},
                          {"look", row.draw_look ? rect_json(row.room) : Json(nullptr)}};
        CHECK(matches_result(c, actual));
    });
}

TEST_CASE("a look draws small as Python's _draw_small_look does", "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "small_look", [](const fv::golden::Case& c) {
        const auto a = c.arg<std::vector<double>>(1);
        const fv::RectD area{a[0], a[1], a[2], a[3]};
        const bool live = c.arg<bool>(2);
        // Classic has no look class and always fits.
        const std::string small = live ? c.arg<std::string>(0) : std::string(fv::kSmallFit);
        const fv::SmallLook look = fv::small_look(small, area, fv::kOverlayWidth);
        const Json actual{
            {"clip", rect_json(look.clip)},
            {"draw", Json::array({look.origin_x, look.origin_y, look.scale_x, look.scale_y,
                                  look.band_width ? Json(*look.band_width) : Json(nullptr)})},
            {"band_width_after", nullptr}};
        CHECK(matches_result(c, actual));
    });
}

TEST_CASE("the tallest the overlay grows replays the golden vectors", "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "max_height", [](const fv::golden::Case& c) {
        const Json& params = c.args().at(0);
        fv::MaxHeightInput in;
        in.base_height = get(params, "base_height", 80);
        in.configured_max = get(params, "configured_max", 400);
        in.embedded = get(params, "embedded", false);
        in.parent_height = get(params, "parent_height", 0);
        in.placed = get(params, "placed", false);
        in.y = get(params, "y", 0);
        const auto available = optional_rect(params, "available");
        if (get(params, "has_screen", true)) {
            in.screen_available = available;
        }
        in.anchor_available = available;
        if (params.contains("anchor") && !params.at("anchor").is_null()) {
            const auto a = ints(params.at("anchor"));
            in.anchor = fv::Point{a[0], a[1]};
        }
        CHECK(matches_result(c, Json(fv::effective_max_height(in))));
    });
}

TEST_CASE("where the overlay goes after it grew replays the golden vectors",
          "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "reposition", [](const fv::golden::Case& c) {
        const Json& params = c.args().at(0);
        fv::RepositionInput in;
        in.preview = get(params, "preview", false);
        in.embedded = get(params, "embedded", false);
        if (params.contains("parent")) {
            const auto p = ints(params.at("parent"));
            in.parent = fv::Size{p[0], p[1]};
        }
        in.overlay = fv::Size{fv::kOverlayWidth, get(params, "height", 80)};
        in.placed = get(params, "placed", false);
        if (params.contains("at")) {
            const auto at = ints(params.at("at"));
            in.current = fv::Point{at[0], at[1]};
        }
        in.cornered = get(params, "cornered", false);
        const auto available = optional_rect(params, "available");
        if (get(params, "has_screen", true)) {
            in.screen_available = available;
        }
        in.anchor_available = available;
        if (params.contains("anchor") && !params.at("anchor").is_null()) {
            const auto a = ints(params.at("anchor"));
            in.anchor = fv::Point{a[0], a[1]};
        }
        const auto moved = fv::reposition_near_anchor(in);
        CHECK(matches_result(c, moved ? Json::array({moved->x, moved->y}) : Json(nullptr)));
    });
}

TEST_CASE("the layout constants are Python's", "[golden][overlay_layout]") {
    fv::golden::for_each(kModule, "tables", [](const fv::golden::Case& c) {
        const Json& t = c.raw().at("result");
        CHECK(t.at("_BADGE_HEIGHT").get<int>() == fv::kBadgeHeight);
        CHECK(t.at("_BADGE_MARGIN").get<int>() == fv::kBadgeMargin);
        CHECK(t.at("_LOOK_HEIGHT").get<double>() == fv::kLookHeight);
        CHECK(t.at("_HEADER_HEIGHT").get<int>() == fv::kHeaderHeight);
        CHECK(t.at("_HEADER_LOOK_HEIGHT").get<double>() == fv::kHeaderLookHeight);
        CHECK(t.at("_HEADER_LOOK_MIN_WIDTH").get<double>() == fv::kHeaderLookMinWidth);
        CHECK(t.at("_TEXT_MARGIN").get<int>() == fv::kTextMargin);
        CHECK(t.at("_TOP_ROW").get<int>() == fv::kTopRow);
        CHECK(t.at("_LIVE_ROW").get<int>() == fv::kLiveRow);
        CHECK(t.at("_PARK_MARGIN").get<int>() == fv::kParkMargin);
    });
}

// The widths below stand for what a font measured on Windows: Segoe UI 8 DemiBold gives the
// Hands-free badge 83 px, the chip is 30 px, the live label "● 0:00" about 40.
namespace {

const fv::TopRowInput kBare{};
fv::TopRowInput latched_with_chip() {
    fv::TopRowInput in;
    in.badge_shown = true;
    in.badge_width = 83;
    in.listening = true;
    in.chip_visible = true;
    in.chip_width = 30;
    return in;
}
fv::TopRowInput walkthrough() {
    fv::TopRowInput in = latched_with_chip();
    in.badge_width = 90;
    in.controls_clickable = true;
    in.controls_left = 150;
    return in;
}

} // namespace

// the geometry of
// tests/test_streaming_overlay_layout.py::test_the_header_keeps_the_look_clear_of_the_badge_chip_and_words
TEST_CASE("the header keeps the look clear of the badge, chip and words",
          "[test_streaming_overlay_layout][overlay_layout]") {
    const fv::TopRowInput in = latched_with_chip();
    fv::HeaderRowInput row_in;
    row_in.shares_top_row = fv::live_shares_top_row(in, 40.0);
    row_in.top_row_span = fv::top_row_span(in);
    row_in.label_width = 40.0;
    row_in.label_width_without_clock = 12.0;
    const fv::HeaderRow row = fv::header_row(row_in);
    CHECK(row.draw_look);
    CHECK(row.room.width >= fv::kHeaderLookMinWidth);
    CHECK(row.room.x > fv::kBadgeMargin + in.badge_width); // right of the badge
    CHECK(row.room.x + row.room.width < 300 - fv::kBadgeMargin - in.chip_width); // left of the chip
    const fv::Margins margins =
        fv::preview_text_margins(fv::PreviewLayout::Header, 80, true, fv::header_height(in, 40.0));
    CHECK(row.room.y + row.room.height < margins.above); // the words start under the header
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_the_header_is_shorter_than_the_footer_for_the_same_words
TEST_CASE("the header grows the overlay less than the footer for the same words",
          "[test_streaming_overlay_layout][overlay_layout]") {
    // The words wrap to some height; the header's margins are smaller than the footer's.
    for (const int text_height : {40, 90, 160}) {
        const auto grow = [&](fv::PreviewLayout layout) {
            fv::HeightInput in;
            in.has_words = true;
            in.band_top = 0;
            in.margins =
                fv::preview_text_margins(layout, 80, false, fv::header_height(kBare, 40.0));
            in.text_height = text_height;
            return fv::target_height(in);
        };
        const int header = grow(fv::PreviewLayout::Header);
        const int footer = grow(fv::PreviewLayout::Footer);
        CHECK(footer > header);
        CHECK(footer - header < 80);
    }
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_the_footer_puts_the_look_and_its_label_under_the_words
TEST_CASE("the footer puts the look and its label under the words",
          "[test_streaming_overlay_layout][overlay_layout]") {
    const fv::TopRowInput in = latched_with_chip();
    const fv::Margins margins = fv::preview_text_margins(fv::PreviewLayout::Footer, 80, true, 0);
    CHECK(margins.above > fv::kTopRow - 1); // the words start under the badge row
    CHECK(margins.below > 80);              // the compact band fits under the words
    fv::HeightInput height;
    height.has_words = true;
    height.margins = margins;
    height.text_height = 60;
    const int total = fv::target_height(height);
    const fv::Rect status =
        fv::status_rect(true, fv::PreviewLayout::Footer, 300, total, fv::band_top(in), 80, {});
    CHECK(status.y + status.height > total - 10);
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_a_caption_takes_the_headers_look_room
TEST_CASE("a caption takes the header's look room",
          "[test_streaming_overlay_layout][overlay_layout]") {
    fv::HeaderRowInput row_in;
    row_in.shares_top_row = true;
    row_in.top_row_span = fv::top_row_span(kBare);
    row_in.label_width = 40.0;
    row_in.label_width_without_clock = 12.0;
    row_in.caption = true;
    const fv::HeaderRow row = fv::header_row(row_in);
    CHECK_FALSE(row.draw_look);
    const fv::Rect status =
        fv::status_rect(true, fv::PreviewLayout::Header, 300, 150, 0, 80, row.room);
    const fv::Rect room = fv::to_rect(row.room);
    // The status row is the room widened 12 px each side, and its text is inset by 12.
    CHECK(status.x + fv::kStatusInset == room.x);
    CHECK(status.width - 2 * fv::kStatusInset == room.width);
    CHECK(status.y == room.y);
    CHECK(status.height == room.height);
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_snap_and_done_give_the_headers_clock_and_look_a_line_of_their_own
TEST_CASE("Snap and Done give the header's clock and look a line of their own",
          "[test_streaming_overlay_layout][overlay_layout]") {
    const fv::TopRowInput in = walkthrough();
    CHECK_FALSE(fv::live_shares_top_row(in, 40.0));
    CHECK(fv::header_height(in, 40.0) == fv::kHeaderHeight + fv::kLiveRow);
    fv::HeaderRowInput row_in;
    row_in.shares_top_row = false;
    row_in.top_row_span = fv::top_row_span(in);
    row_in.label_width = 40.0;
    row_in.label_width_without_clock = 12.0;
    const fv::HeaderRow row = fv::header_row(row_in);
    CHECK(row.draw_label);
    CHECK(row.label_clock); // the clock keeps its place
    const double snap_bottom = fv::kBadgeMargin + fv::kBadgeHeight;
    CHECK(row.label_rect.y > snap_bottom);
    CHECK(row.room.y > snap_bottom);
    CHECK(row.room.width >= fv::kHeaderLookMinWidth);
    const fv::Margins margins =
        fv::preview_text_margins(fv::PreviewLayout::Header, 80, true, fv::header_height(in, 40.0));
    CHECK(row.room.y + row.room.height < margins.above); // the words start under the header
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_the_compact_look_sits_under_snap_and_done
TEST_CASE("the compact look sits under Snap and Done",
          "[test_streaming_overlay_layout][overlay_layout]") {
    const fv::TopRowInput in = walkthrough();
    const int top = fv::band_top(in);
    CHECK(top >= fv::kBadgeMargin + fv::kBadgeHeight);
    fv::HeightInput height;
    height.band_top = top;
    CHECK(fv::target_height(height) == 80 + top);
    CHECK(fv::status_rect(false, fv::PreviewLayout::Header, 300, 80 + top, top, 80, {}).y +
              fv::kStatusHeight <
          80 + top);
}

// the geometry of
// tests/test_streaming_overlay_layout.py::test_the_overlay_drops_back_to_compact_once_the_badge_row_empties
TEST_CASE("the overlay drops back to compact once the badge row empties",
          "[test_streaming_overlay_layout][overlay_layout]") {
    fv::TopRowInput in = walkthrough();
    fv::HeightInput height;
    height.band_top = fv::band_top(in);
    CHECK(fv::target_height(height) > 80);
    in.controls_clickable = false;
    in.badge_shown = false;
    in.listening = false;
    height.band_top = fv::band_top(in);
    CHECK(fv::target_height(height) == 80);
}

TEST_CASE("the overlay's height with no words is 80 plus the band's top", "[overlay_layout]") {
    fv::HeightInput height;
    CHECK(fv::target_height(height) == 80);
    height.band_top = fv::kTopRow;
    CHECK(fv::target_height(height) == 107);
}

TEST_CASE("the words' rectangle and divider follow the two layouts", "[overlay_layout]") {
    const fv::Margins header{42, 12};
    const fv::Rect text = fv::preview_text_rect(300, 150, header);
    CHECK(text == fv::Rect{12, 42, 276, 96});
    // Little room left: the rectangle keeps 20 px.
    CHECK(fv::preview_text_rect(300, 60, header).height == 20);
    CHECK(fv::divider_y(fv::PreviewLayout::Header, 150, 80, 36) == 36);
    CHECK(fv::divider_y(fv::PreviewLayout::Footer, 150, 80, 36) == 70);
    CHECK(fv::footer_band_top(150, 80) == 70);
}

TEST_CASE("the embedded overlay shows the newest lines that fit, bottom-aligned",
          "[overlay_layout]") {
    const fv::Rect text{12, 40, 276, 50};
    // Five 20 px lines: the last two fit in 50 px.
    const fv::VisibleLines lines = fv::visible_preview_lines({20, 20, 20, 20, 20}, text);
    CHECK(lines.first == 3);
    CHECK(lines.height == 40.0);
    // bottom() is y + height - 1 = 89; the lines start at bottom - height + 1.
    CHECK(lines.y == 50.0);
    const fv::VisibleLines all = fv::visible_preview_lines({10, 10}, text);
    CHECK(all.first == 0);
    CHECK(all.height == 20.0);
    CHECK(fv::visible_preview_lines({}, text).first == 0);
    // A line taller than the box shows nothing, and the lines above it are not tried.
    CHECK(fv::visible_preview_lines({10, 60}, text).first == 2);
}

TEST_CASE("the words grow the overlay between its base height and the room there is",
          "[overlay_layout]") {
    fv::HeightInput in;
    in.has_words = true;
    in.margins = fv::Margins{42, 12};
    in.text_height = 10;
    CHECK(fv::target_height(in) == 80); // never below the base
    in.text_height = 100;
    CHECK(fv::target_height(in) == 154);
    in.text_height = 600;
    CHECK(fv::target_height(in) == 400); // capped
    in.max_height = 200;
    CHECK(fv::target_height(in) == 200);
    CHECK(fv::text_flow_width(300) == 276);
}

TEST_CASE("the room to grow is limited by the screen, the anchor or the window",
          "[overlay_layout]") {
    fv::MaxHeightInput in;
    in.embedded = true;
    in.parent_height = 900;
    CHECK(fv::effective_max_height(in) == 300);
    in.parent_height = 100;
    CHECK(fv::effective_max_height(in) == 80);
    in.parent_height = 3000;
    CHECK(fv::effective_max_height(in) == 400);
    in = {};
    CHECK(fv::effective_max_height(in) == 400); // no screen known: the configured maximum
    in.placed = true;
    in.screen_available = fv::Rect{0, 0, 1920, 1040};
    in.y = 900;
    CHECK(fv::effective_max_height(in) == 132); // 1040 - 8 - 900
    in.y = 1030;
    CHECK(fv::effective_max_height(in) == 80); // never below the base
    in.y = 0;
    CHECK(fv::effective_max_height(in) == 400);
}

TEST_CASE("the small look's transform and width", "[overlay_layout]") {
    const fv::RectD area{100, 6, 120, 24};
    const double scale = 24.0 / fv::kLookHeight;
    const fv::SmallLook squash = fv::small_look(fv::kSmallSquash, area, 300);
    CHECK(squash.scale_x == 1.0);
    CHECK(squash.scale_y == scale);
    CHECK(squash.band_width == 120.0);
    const fv::SmallLook shrink = fv::small_look(fv::kSmallShrink, area, 300);
    CHECK(shrink.scale_x == scale);
    CHECK(shrink.scale_y == scale);
    CHECK(shrink.band_width == 120.0 / scale);
    const fv::SmallLook fit = fv::small_look(fv::kSmallFit, area, 300);
    // The whole 300 px overlay drawing scales into 120 px, centred in the room.
    CHECK(fit.scale_x == 120.0 / 300.0);
    CHECK_FALSE(fit.band_width.has_value());
    CHECK(fit.origin_x == 100.0);
    CHECK(fit.origin_y == 6.0 + (24.0 - 56.0 * (120.0 / 300.0)) / 2);
}

// What PyQt6 6.11's QRectF(...).toRect() gives for each.
TEST_CASE("QRectF's toRect rounds the corner, then the size by half the corner's move",
          "[overlay_layout]") {
    CHECK(fv::to_rect(fv::RectD{0.5, 0.5, 1.0, 1.0}) == fv::Rect{1, 1, 1, 1});
    CHECK(fv::to_rect(fv::RectD{78.0, 6.0, 194.5, 24.0}) == fv::Rect{78, 6, 195, 24});
    // The corner rounds half away from zero; the size keeps 3 (3 + 0.25 rounds to 3).
    CHECK(fv::to_rect(fv::RectD{-0.5, -1.5, 3.0, 3.0}) == fv::Rect{-1, -2, 3, 3});
    // Rounding the right edge on its own would give 16 - 10 = 6.
    CHECK(fv::to_rect(fv::RectD{10.3, 0.0, 5.3, 1.0}) == fv::Rect{10, 0, 5, 1});
    CHECK(fv::to_rect(fv::RectD{98.6, 6.0, 121.4, 24.0}) == fv::Rect{99, 6, 121, 24});
    CHECK(fv::to_rect(fv::RectD{0.25, 0.0, 1.5, 1.0}) == fv::Rect{0, 0, 2, 1});
    CHECK(fv::to_rect(fv::RectD{-2.5, 0.0, 1.0, 1.0}) == fv::Rect{-3, 0, 1, 1});
}
