// Where the recording overlay puts its look once live preview words arrive
// (tests/test_streaming_overlay_layout.py).
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/generators/catch_generators_range.hpp>

#include <memory>
#include <string>
#include <vector>

#include <QPainter>

#include "overlay_test_support.h"
#include "settings/keys.h"
#include "settings/testing/memory_store.h"
#include "uimath/overlay_layout.h"
#include "uimath/overlay_look.h"

using fv::PreviewLayout;
using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

const QString kWords = QStringLiteral(
    "So I was thinking about the launch plan and June works if we freeze the API by May "
    "fifteenth.");

// _listen: streaming in layout with look, the words in; latched adds the hands-free badge and
// the language chip.
void listen(Overlay& overlay, PreviewLayout layout, const QString& look = QStringLiteral("bars"),
            bool latched = false) {
    overlay.access.streamingLayout() = layout;
    overlay->setRecordingLook(look);
    overlay->setState(WaveformOverlay::kStateStreaming);
    overlay->timer().stop();
    if (latched) {
        overlay->setHandsFree(true);
        overlay->setLanguage(QStringLiteral("en"), {QStringLiteral("en"), QStringLiteral("es")});
    }
    overlay->updateStreamingText(kWords);
    // Twice: the header leaves room for where the badge and chip were last frame.
    fvtest::paint(overlay);
    fvtest::paint(overlay);
}

std::string layoutSetting(const fv::Json& saved) {
    return fv::keys::streaming_overlay_layout.read(saved);
}

} // namespace

// from
// tests/test_streaming_overlay_layout.py::test_header_is_the_default_and_unknown_values_fall_back_to_it
TEST_CASE("header is the default and unknown values fall back to it",
          "[test_streaming_overlay_layout]") {
    CHECK(layoutSetting(fv::Json::object()) == "header");
    CHECK(layoutSetting(fv::Json{{"streaming_overlay_layout", "sideways"}}) == "header");
    CHECK(layoutSetting(fv::Json{{"streaming_overlay_layout", "footer"}}) == "footer");
}

// from
// tests/test_streaming_overlay_layout.py::test_the_header_keeps_the_look_clear_of_the_badge_chip_and_words
TEST_CASE("the header keeps the look clear of the badge chip and words",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    listen(overlay, PreviewLayout::Header, QStringLiteral("bars"), true);
    const QRectF room = overlay.access.headerRoom();
    CHECK(room.width() >= fv::kHeaderLookMinWidth);
    CHECK(room.left() > overlay.access.badgeRect().right());
    CHECK(room.right() < overlay.access.chipRect().left());
    // The words start under the header.
    CHECK(room.bottom() < overlay.access.previewTextMargins().above);
}

// from
// tests/test_streaming_overlay_layout.py::test_the_header_is_shorter_than_the_footer_for_the_same_words
TEST_CASE("the header is shorter than the footer for the same words",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    listen(overlay, PreviewLayout::Header);
    const int headerHeight = overlay->height();
    listen(overlay, PreviewLayout::Footer);
    CHECK(overlay->height() > headerHeight);
    CHECK(overlay->height() - headerHeight < overlay.access.baseHeight());
}

// from
// tests/test_streaming_overlay_layout.py::test_the_footer_puts_the_look_and_its_label_under_the_words
TEST_CASE("the footer puts the look and its label under the words",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    listen(overlay, PreviewLayout::Footer, QStringLiteral("bars"), true);
    const fv::Margins margins = overlay.access.previewTextMargins();
    // The words start under the badge row.
    CHECK(margins.above > overlay.access.badgeRect().bottom());
    // The compact band fits under the words.
    CHECK(margins.below > overlay.access.baseHeight());
    CHECK(overlay.access.statusRect().bottom() > overlay->height() - 10);
}

// from tests/test_streaming_overlay_layout.py::test_a_caption_takes_the_headers_look_room
TEST_CASE("a caption takes the headers look room", "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    listen(overlay, PreviewLayout::Header);
    overlay->showCaption(QStringLiteral("Switched to the USB microphone"));
    fvtest::paint(overlay);
    const QRect status = overlay.access.statusRect().adjusted(12, 0, -12, 0);
    CHECK(status == overlay.access.headerRoom().toRect());
}

// from tests/test_streaming_overlay_layout.py::test_changing_the_layout_reflows_the_preview
TEST_CASE("changing the layout reflows the preview", "[widget][test_streaming_overlay_layout]") {
    fv::MemorySettingsStore settings;
    Overlay overlay(&settings);
    listen(overlay, PreviewLayout::Header);
    const int headerHeight = overlay->height();
    settings.update(fv::Json{{"streaming_overlay_layout", "footer"}});
    overlay->refreshStreamingLayout();
    CHECK(overlay->streamingLayout() == PreviewLayout::Footer);
    CHECK(overlay->height() > headerHeight);
}

// update_streaming_text, clear_streaming_text and refresh_streaming_font_size
// (waveform_overlay.py:598-654): the words are stripped and drawn at the saved size, 16 pt unless
// set and kept within 10 to 48, and a new size reflows them.
TEST_CASE("the words are stripped and a new font size reflows them", "[widget]") {
    fv::MemorySettingsStore settings;
    Overlay overlay(&settings);
    CHECK(overlay->streamingFontSize() == 16);
    overlay->setState(WaveformOverlay::kStateStreaming);
    overlay->timer().stop();

    overlay->updateStreamingText(QStringLiteral("  \n") + kWords + QStringLiteral(" \n"), false);
    CHECK(overlay->streamingText() == kWords);
    const int atDefault = overlay->height();
    CHECK(atDefault > overlay.access.baseHeight());

    settings.update(fv::Json{{"streaming_overlay_font_size", 30}});
    overlay->refreshStreamingFontSize();
    CHECK(overlay->streamingFontSize() == 30);
    CHECK(overlay->height() > atDefault);
    fvtest::paint(overlay);

    settings.update(fv::Json{{"streaming_overlay_font_size", 400}});
    overlay->refreshStreamingFontSize();
    CHECK(overlay->streamingFontSize() == 48);
    settings.update(fv::Json{{"streaming_overlay_font_size", 2}});
    overlay->refreshStreamingFontSize();
    CHECK(overlay->streamingFontSize() == 10);
    CHECK(overlay->height() < atDefault);
    fvtest::paint(overlay);

    overlay->clearStreamingText();
    CHECK(overlay->streamingText().isEmpty());
    CHECK(overlay->height() == overlay.access.baseHeight());
}

// from tests/test_streaming_overlay_layout.py::test_every_look_draws_small_in_the_header
TEST_CASE("every look draws small in the header", "[widget][test_streaming_overlay_layout]") {
    std::vector<QString> looks;
    for (const std::string_view look : fv::kLooks) {
        looks.push_back(QString::fromUtf8(look.data(), static_cast<qsizetype>(look.size())));
    }
    const QString look = GENERATE_COPY(from_range(looks));
    CAPTURE(look.toStdString());
    Overlay overlay;
    listen(overlay, PreviewLayout::Header, look);
    // The look had room in the header. Python then checks that the small copy put the look's
    // band_width back; here the width is an argument of each draw, so there is nothing to put
    // back, and the compact look after the words draws at the overlay's own width.
    CHECK(overlay.access.headerRoom().width() >= fv::kHeaderLookMinWidth);
    overlay->setState(WaveformOverlay::kStateRecording);
    fvtest::paint(overlay);
}

namespace {

// The walkthrough fixture: the overlay with a walkthrough's badge, Snap and Done, on a platform
// that takes overlay clicks.
void walkthrough(Overlay& overlay) {
    overlay.access.setTakesClicks(true);
    overlay->setRecordingDestination(QStringLiteral("Walkthrough"));
    overlay->setWalkthroughControls(true);
}

// The look's work drawing, recorded: the painter's vertical shift each time it draws.
class WorkSpy final : public fv::Visual {
public:
    explicit WorkSpy(std::vector<double>& shifts) : Visual(fv::LookContext{}), shifts_(shifts) {}
    void draw(QPainter& /*painter*/, double /*grow*/, double /*width*/) override {}
    void drawWork(QPainter& painter, double /*morph*/, double /*clock*/,
                  double /*width*/) override {
        shifts_.push_back(painter.transform().dy());
    }

private:
    std::vector<double>& shifts_;
};

} // namespace

// from
// tests/test_streaming_overlay_layout.py::test_snap_and_done_give_the_headers_clock_and_look_a_line_of_their_own
// Python spies on draw_live_label; the overlay keeps the header row it last painted instead.
TEST_CASE("snap and done give the headers clock and look a line of their own",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    walkthrough(overlay);
    listen(overlay, PreviewLayout::Header, QStringLiteral("bars"), true);
    const QRectF snap = overlay.access.controlRect(QStringLiteral("snap"));
    fvtest::paint(overlay);
    const fv::HeaderRow& row = overlay.access.headerRow();
    REQUIRE(row.draw_label);
    // The clock keeps its place.
    CHECK(row.label_clock);
    CHECK(row.label_rect.y > snap.bottom());
    const QRectF room = overlay.access.headerRoom();
    CHECK(room.top() > snap.bottom());
    CHECK(room.width() >= fv::kHeaderLookMinWidth);
    // The words start under the header.
    CHECK(room.bottom() < overlay.access.previewTextMargins().above);
}

// from tests/test_streaming_overlay_layout.py::test_the_compact_look_sits_under_snap_and_done
// Python spies on the top the band is drawn at; it is bandTop() as the frame paints.
TEST_CASE("the compact look sits under snap and done", "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    walkthrough(overlay);
    overlay->setRecordingLook(QStringLiteral("bars"));
    overlay->setState(WaveformOverlay::kStateRecording);
    overlay->timer().stop();
    fvtest::paint(overlay);
    const int top = overlay.access.bandTop();
    CHECK(top >= overlay.access.controlRect(QStringLiteral("snap")).bottom());
    CHECK(overlay->height() == overlay.access.baseHeight() + top);
    CHECK(overlay.access.statusRect().bottom() < overlay->height());
}

// from
// tests/test_streaming_overlay_layout.py::test_the_work_stages_stay_under_the_walkthrough_badge
TEST_CASE("the work stages stay under the walkthrough badge",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    walkthrough(overlay);
    overlay->setRecordingLook(QStringLiteral("bars"));
    overlay->setState(WaveformOverlay::kStateRecording);
    overlay->setState(WaveformOverlay::kStateTranscribing);
    overlay->timer().stop();
    overlay->setWalkthroughControls(false);
    std::vector<double> shifts;
    // The look stays alive beside the spy: the particles hold its emitter.
    std::unique_ptr<fv::Visual> look = std::move(overlay.access.visualSlot());
    overlay.access.visualSlot() = std::make_unique<WorkSpy>(shifts);
    fvtest::paint(overlay);
    overlay.access.visualSlot() = std::move(look);
    CHECK(shifts == std::vector<double>{static_cast<double>(fv::kTopRow)});
    CHECK(overlay->height() == overlay.access.baseHeight() + fv::kTopRow);
}

// from
// tests/test_streaming_overlay_layout.py::test_the_overlay_drops_back_to_compact_once_the_badge_row_empties
TEST_CASE("the overlay drops back to compact once the badge row empties",
          "[widget][test_streaming_overlay_layout]") {
    Overlay overlay;
    walkthrough(overlay);
    overlay->setState(WaveformOverlay::kStateRecording);
    overlay->timer().stop();
    CHECK(overlay->height() > overlay.access.baseHeight());
    overlay->setWalkthroughControls(false);
    overlay->setState(WaveformOverlay::kStateCopied);
    overlay->timer().stop();
    CHECK(overlay->height() == overlay.access.baseHeight());
}

// The header's numbers (waveform_overlay.py:405-516): a 36 px header under a badge and chip with
// room for the label and look, the words 6 px under it.
TEST_CASE("the header is the badge row and its words start under it", "[widget]") {
    Overlay overlay;
    listen(overlay, PreviewLayout::Header, QStringLiteral("bars"), true);
    CHECK(overlay.access.headerHeight() == fv::kHeaderHeight);
    CHECK(overlay.access.previewTextMargins().above == fv::kHeaderHeight + 6);
    CHECK(overlay.access.previewTextMargins().below == 12);
    CHECK(overlay->height() > overlay.access.baseHeight());
    CHECK(overlay->height() <= fv::kOverlayStreamingMaxHeight);
    overlay->clearStreamingText();
    // Without words the compact look sits under the badge row again.
    CHECK(overlay->height() == overlay.access.baseHeight() + fv::kTopRow);
}
