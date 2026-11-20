// The engine readout of the Quick Record card: what the status dot and the resolved label say
// while the engine loads, downloads a model or works on a recording.
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>

#include "record_fixture.h"
#include "ui_widgets/downloads_label.h"

namespace {

using fv::EngineStatus;
using fv::appcore::OverlayState;

QString text(const fvtest::ShownTab& tab) {
    return tab->resolvedLabel()->text();
}

} // namespace

// from tests/test_engine_status.py::test_ready_details_and_runtime_messages_share_the_card
TEST_CASE("ready details and runtime messages share the card", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | cuda"), true);
    CHECK(tab->findChild<QLabel*>(QStringLiteral("statusLabel")) == nullptr);
    CHECK(tab->engineCard()->isAncestorOf(tab->resolvedLabel()));
    CHECK(text(tab) == QStringLiteral("Parakeet TDT 0.6B v3 | cuda"));
    tab->setStatus(QStringLiteral("Copied to clipboard"));
    CHECK(text(tab) == QStringLiteral("Copied to clipboard"));
    tab->setStatus(QStringLiteral("Ready to record"));
    CHECK(text(tab) == QStringLiteral("Parakeet TDT 0.6B v3 | cuda"));
}

// from tests/test_engine_status.py::test_background_news_never_covers_a_problem
TEST_CASE("background news never covers a problem", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("parakeet-v3 | cpu"), true);
    tab->setStatus(QStringLiteral("Model download failed: no internet connection"));
    tab->setAmbientMessage(QStringLiteral("Could not check for updates."));
    CHECK(text(tab) == QStringLiteral("Model download failed: no internet connection"));
    tab->setStatus(QStringLiteral("Ready"));
    tab->setAmbientMessage(QStringLiteral("Could not check for updates."));
    CHECK(text(tab) == QStringLiteral("Could not check for updates."));
}

// from tests/test_engine_status.py::test_loading_animates_and_returns_to_resolved_engine
TEST_CASE("loading animates and returns to resolved engine", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setEngineBusy(true);
    CHECK(tab->statusDot()->isAnimating());
    const QImage firstFrame = tab->statusDot()->grab().toImage();
    QTest::qWait(120);
    CHECK(tab->statusDot()->grab().toImage() != firstFrame);
    tab->setDeviceInfo(QStringLiteral("parakeet-v3 | cpu"), true);
    tab->setStatus(QStringLiteral("Whisper engine ready"));
    CHECK(tab->statusDot()->isAnimating());
    tab->setEngineBusy(false);
    CHECK_FALSE(tab->statusDot()->isAnimating());
    CHECK(tab->statusDot()->status() == EngineStatus::Ready);
    CHECK(text(tab) == QStringLiteral("parakeet-v3 | cpu"));
}

// from tests/test_engine_status.py::test_loading_failure_keeps_the_error_after_animation_stops
TEST_CASE("loading failure keeps the error after animation stops", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setEngineBusy(true);
    tab->setStatus(QStringLiteral("Engine load failed: not enough memory"));
    tab->setEngineBusy(false);
    CHECK_FALSE(tab->statusDot()->isAnimating());
    CHECK(text(tab) == QStringLiteral("Engine load failed: not enough memory"));
}

// from tests/test_engine_status.py::test_activity_animates_until_finished_without_erasing_outcome
TEST_CASE("activity animates until finished without erasing outcome",
          "[test_engine_status][widget]") {
    const OverlayState state = GENERATE(OverlayState::Processing, OverlayState::Transcribing,
                                        OverlayState::Cleaning, OverlayState::Canceling);
    fvtest::ShownTab tab;
    tab->setActivityState(state);
    CHECK(tab->statusDot()->isAnimating());
    tab->setStatus(state == OverlayState::Canceling
                       ? QStringLiteral("Transcription canceled")
                       : QStringLiteral("Error: audio could not be processed"));
    const QString message = text(tab);
    tab->setActivityState(OverlayState::None);
    CHECK_FALSE(tab->statusDot()->isAnimating());
    CHECK(text(tab) == message);
}

// from tests/test_engine_status.py::test_animation_suspends_when_hidden_and_resumes_when_shown
TEST_CASE("animation suspends when hidden and resumes when shown", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setEngineBusy(true);
    tab->hide();
    CHECK_FALSE(tab->statusDot()->isAnimating());
    tab->show();
    CHECK(tab->statusDot()->isAnimating());
}

// from tests/test_engine_status.py::test_long_messages_keep_details_and_download_action
TEST_CASE("long messages keep details and download action", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setStatus(QStringLiteral("Install model in Downloads."));
    QApplication::processEvents();
    const int originalWidth = tab->engineCard()->minimumSizeHint().width();
    QString message = QStringLiteral("Install ");
    for (int i = 0; i < 8; ++i) {
        message += QStringLiteral("a very long speech model name ");
    }
    message += QStringLiteral("in Downloads.");
    QSignalSpy opened(tab.tab.get(), &fv::QuickRecordTab::engineDownloadsRequested);
    tab->setStatus(message);
    QApplication::processEvents();
    CHECK(tab->resolvedLabel()->toolTip() == message);
    CHECK(tab->resolvedLabel()->QLabel::text().contains(QStringLiteral("href=\"downloads\"")));
    Q_EMIT tab->resolvedLabel()->linkActivated(QStringLiteral("downloads"));
    CHECK(opened.count() == 1);
    CHECK(tab->engineCard()->minimumSizeHint().width() <= originalWidth);
}

// from tests/test_engine_status.py::test_download_and_reload_activity_end_independently
TEST_CASE("download and reload activity end independently", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), true);
    tab->setModelDownloading(QStringLiteral("orukeet-v0.1"), true);
    tab->setEngineBusy(true);
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), false);
    tab->setModelDownloading(QStringLiteral("orukeet-v0.1"), false);
    CHECK(tab->statusDot()->isAnimating());
    tab->setDeviceInfo(QStringLiteral("parakeet-v3 | cpu"), true);
    tab->setEngineBusy(false);
    CHECK_FALSE(tab->statusDot()->isAnimating());
}

// from
// tests/test_engine_status.py::test_cloud_processing_has_an_indicator_without_claiming_local_readiness
TEST_CASE("cloud processing has an indicator without claiming local readiness",
          "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setBackend(QStringLiteral("API"));
    tab->setDeviceInfo(QString());
    CHECK_FALSE(tab->statusDot()->isVisible());
    tab->setActivityState(OverlayState::Transcribing);
    CHECK(tab->statusDot()->isVisible());
    CHECK(tab->statusDot()->isAnimating());
    tab->setActivityState(OverlayState::None);
    CHECK_FALSE(tab->statusDot()->isVisible());
}

// from tests/test_engine_status.py::test_download_failure_stops_animation_and_preserves_reason
TEST_CASE("download failure stops animation and preserves reason", "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), true);
    tab->setStatus(QStringLiteral("Model download failed: connection interrupted"));
    tab->setModelDownloading(QStringLiteral("parakeet-v3"), false);
    CHECK_FALSE(tab->statusDot()->isAnimating());
    CHECK(text(tab) == QStringLiteral("Model download failed: connection interrupted"));
}

// from
// tests/test_engine_status.py::test_cached_download_completion_does_not_leave_a_loading_message
TEST_CASE("cached download completion does not leave a loading message",
          "[test_engine_status][widget]") {
    fvtest::ShownTab tab;
    tab->setDeviceInfo(QStringLiteral("parakeet-v3 | cpu"), true);
    tab->setModelDownloading(QStringLiteral("orukeet-v0.1"), true);
    tab->setModelDownloading(QStringLiteral("orukeet-v0.1"), false);
    CHECK(text(tab) == QStringLiteral("parakeet-v3 | cpu"));
    CHECK_FALSE(tab->statusDot()->isAnimating());
}
