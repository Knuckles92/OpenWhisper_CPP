// The compact controller (Record, Stop, Cancel and a status line) and the stats strip.
#include <catch2/catch_test_macros.hpp>

#include <QApplication>
#include <QLabel>
#include <QSignalSpy>
#include <QTest>
#include <QVBoxLayout>

#include "ui_record/compact_record_controller.h"
#include "ui_record/stats_display.h"
#include "ui_widgets/buttons.h"

namespace {

QString hotkeyOf(fv::Button* button) {
    fv::HotkeyHintFilter* filter = button->hotkeyHintFilter();
    filter->showHint();
    if (filter->hint() == nullptr) {
        return {};
    }
    const QString shown = filter->hint()->findChild<QLabel*>()->text();
    filter->hideHint();
    return shown;
}

} // namespace

TEST_CASE("the compact controller has a status line and three buttons", "[widget]") {
    fv::CompactRecordController controller;
    CHECK(controller.objectName() == QStringLiteral("compactRecordController"));
    CHECK(controller.statusLabel()->objectName() == QStringLiteral("statusLabel"));
    CHECK(controller.statusLabel()->text() == QStringLiteral("Ready to record"));
    CHECK(controller.statusLabel()->alignment() == Qt::AlignCenter);
    CHECK(controller.statusLabel()->font().pointSize() == 13);
    CHECK(controller.recordButton()->text() == QStringLiteral("Record"));
    CHECK(controller.stopButton()->text() == QStringLiteral("Stop"));
    CHECK(controller.cancelButton()->text() == QStringLiteral("Cancel"));
    CHECK(controller.recordButton()->isActive());
    CHECK_FALSE(controller.stopButton()->isActive());
    CHECK_FALSE(controller.cancelButton()->isActive());
    auto* layout = qobject_cast<QVBoxLayout*>(controller.layout());
    REQUIRE(layout != nullptr);
    CHECK(layout->contentsMargins() == QMargins(12, 18, 12, 18));
    CHECK(layout->spacing() == 14);
}

TEST_CASE("the compact controller's buttons ask and its state follows the recording", "[widget]") {
    fv::CompactRecordController controller;
    controller.resize(420, 200);
    controller.show();
    QApplication::processEvents();
    QSignalSpy record(&controller, &fv::CompactRecordController::recordRequested);
    QSignalSpy stop(&controller, &fv::CompactRecordController::stopRequested);
    QSignalSpy cancel(&controller, &fv::CompactRecordController::cancelRequested);

    QTest::mouseClick(controller.recordButton(), Qt::LeftButton);
    CHECK(record.count() == 1);
    // Not recording: Stop and Cancel are inactive and swallow their clicks.
    QTest::mouseClick(controller.stopButton(), Qt::LeftButton);
    QTest::mouseClick(controller.cancelButton(), Qt::LeftButton);
    CHECK(stop.count() == 0);
    CHECK(cancel.count() == 0);

    controller.setRecordingState(true);
    CHECK_FALSE(controller.recordButton()->isActive());
    CHECK(controller.recordButton()->text() == QStringLiteral("Recording"));
    CHECK(controller.stopButton()->isActive());
    CHECK(controller.cancelButton()->isActive());
    QTest::mouseClick(controller.stopButton(), Qt::LeftButton);
    QTest::mouseClick(controller.cancelButton(), Qt::LeftButton);
    CHECK(stop.count() == 1);
    CHECK(cancel.count() == 1);

    controller.setRecordingState(false);
    CHECK(controller.recordButton()->isActive());
    CHECK(controller.recordButton()->text() == QStringLiteral("Record"));
    CHECK_FALSE(controller.stopButton()->isActive());

    controller.setStatus(QStringLiteral("Recording in progress..."));
    CHECK(controller.statusLabel()->text() == QStringLiteral("Recording in progress..."));
}

TEST_CASE("the compact controller's buttons are at least 88 by 44", "[widget]") {
    fv::CompactRecordController controller;
    controller.resize(420, 200);
    controller.show();
    QApplication::processEvents();
    for (fv::Button* button : {static_cast<fv::Button*>(controller.recordButton()),
                               static_cast<fv::Button*>(controller.stopButton()),
                               static_cast<fv::Button*>(controller.cancelButton())}) {
        CHECK(button->width() >= 88);
        CHECK(button->height() >= 44);
    }
}

TEST_CASE("the compact controller's record and stop buttons share the record key", "[widget]") {
    fv::CompactRecordController controller;
    controller.updateHotkeys(QStringLiteral("Ctrl+Alt+R"), QStringLiteral("Esc"));
    CHECK(hotkeyOf(controller.recordButton()) == QStringLiteral("Ctrl+Alt+R"));
    CHECK(hotkeyOf(controller.stopButton()) == QStringLiteral("Ctrl+Alt+R"));
    CHECK(hotkeyOf(controller.cancelButton()) == QStringLiteral("Esc"));
}

TEST_CASE("the stats strip is hidden until it has numbers and clears back", "[widget]") {
    fv::TranscriptionStatsWidget stats;
    CHECK(stats.objectName() == QStringLiteral("statsWidget"));
    CHECK(stats.isHidden());
    CHECK(stats.transcriptionTimeLabel()->text() == QStringLiteral("--"));

    QSignalSpy visibility(&stats, &fv::TranscriptionStatsWidget::visibilityChanged);
    stats.setStats(2.4, 12.7, 403200, 1.5);
    CHECK_FALSE(stats.isHidden());
    CHECK(stats.transcriptionTimeLabel()->text() == QStringLiteral("2.4s"));
    CHECK(stats.audioDurationLabel()->text() == QStringLiteral("12.7s"));
    CHECK(stats.fileSizeLabel()->text() == QStringLiteral("393.8 KB"));

    stats.clear();
    CHECK(stats.isHidden());
    CHECK(stats.audioDurationLabel()->text() == QStringLiteral("--"));
    CHECK(visibility.count() == 2);
}

TEST_CASE("the stats strip formats durations and sizes as History does", "[widget]") {
    fv::TranscriptionStatsWidget stats;
    stats.setStats(125.0, 3725.0, 5 * 1024 * 1024 + 512 * 1024);
    CHECK(stats.transcriptionTimeLabel()->text() == QStringLiteral("2m 5s"));
    CHECK(stats.audioDurationLabel()->text() == QStringLiteral("1h 2m"));
    CHECK(stats.fileSizeLabel()->text() == QStringLiteral("5.5 MB"));
    stats.setStats(0.0, 0.0, 12);
    CHECK(stats.transcriptionTimeLabel()->text() == QStringLiteral("0.0s"));
    CHECK(stats.fileSizeLabel()->text() == QStringLiteral("12 B"));
    // A file size nobody measured has no number.
    stats.setStats(1.0, 2.0, std::nullopt);
    CHECK(stats.fileSizeLabel()->text() == QStringLiteral("--"));
}
