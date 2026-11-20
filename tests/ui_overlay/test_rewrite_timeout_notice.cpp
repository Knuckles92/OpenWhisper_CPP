// The AI rewrite's timeout notice near the pointer (tests/test_personalize_s3_text_rewrite.py):
// its fix is in its last words, so the overlay must not cut them off. The text is
// llm::kTimedOutMessage behind the runtime's "Error: " (fv_ui_overlay's tests do not link fv_llm).
#include <catch2/catch_test_macros.hpp>

#include <QFont>
#include <QRect>

#include "overlay_test_support.h"

using fvtest::Overlay;

// from tests/test_personalize_s3_text_rewrite.py::test_the_timeout_notice_fits_by_the_pointer
TEST_CASE("the AI timeout notice fits by the pointer",
          "[widget][test_personalize_s3_text_rewrite]") {
    Overlay overlay;
    const QString notice =
        QStringLiteral("Error: The AI model took too long. Try a faster one in AI cleanup");
    overlay->showNotice(notice);
    REQUIRE(overlay.access.notice() == notice);
    // The text box drawNoticeState uses.
    const QRect rect(48, 8, overlay->width() - 48 - 14, overlay->height() - 16);
    const QFont font(QStringLiteral("Segoe UI"), 10, QFont::DemiBold);
    CHECK(overlay.access.fitNotice(font, rect) == notice);
}
