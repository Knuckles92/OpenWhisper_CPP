// The a32 checklist's MW-18 claim on the overlay: Settings > Font size scales the app's windows,
// and the live transcription overlay keeps its own size (its words follow
// streaming_overlay_font_size alone; CHANGELOG 2.5.0 "Font size").
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QSize>

#include "overlay_test_support.h"
#include "ui_theme/font_scale.h"
#include "ui_theme/theme_manager.h"

using fv::WaveformOverlay;
using fvtest::Overlay;

namespace {

QSize streamingSize(Overlay& overlay) {
    overlay->setState(WaveformOverlay::kStateStreaming);
    overlay->timer().stop();
    overlay->updateStreamingText(
        QStringLiteral("So I was thinking about the launch plan and June works if we freeze "
                       "the API by May fifteenth."));
    // Twice: the header leaves room for where the badge and chip were last frame.
    fvtest::paint(overlay);
    fvtest::paint(overlay);
    return overlay->size();
}

} // namespace

TEST_CASE("a32 MW-18: the live transcription overlay keeps its size at every font scale",
          "[widget]") {
    fv::ui::ThemeManager manager(QStringLiteral("dark"));
    fv::ui::applyUiFontScale(100, *qApp, manager);
    Overlay atDefault;
    const QSize base = streamingSize(atDefault);
    const int fontSize = atDefault->streamingFontSize();

    fv::ui::applyUiFontScale(130, *qApp, manager);
    Overlay atLarge;
    CHECK(streamingSize(atLarge) == base);
    CHECK(atLarge->streamingFontSize() == fontSize);
    // A window already open keeps its size through the change too.
    CHECK(streamingSize(atDefault) == base);

    fv::ui::applyUiFontScale(100, *qApp, manager);
    qApp->setStyleSheet(QString());
}
