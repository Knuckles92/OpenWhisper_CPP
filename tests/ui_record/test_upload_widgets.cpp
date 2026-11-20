// The Upload File tab's widgets (drop zone, queue, relation picker, file card, progress panel),
// ported from tests/test_upload_file_tab.py, plus what the port adds around them.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QLabel>
#include <QPushButton>
#include <QSignalSpy>
#include <QStackedWidget>
#include <QTest>
#include <QTimer>

#include "app_theme.h"
#include "appcore/overlay_state.h"
#include "dictation/upload_progress.h"
#include "history/format_utils.h"
#include "ui_record/drop_zone.h"
#include "ui_record/file_info_card.h"
#include "ui_record/relation_picker.h"
#include "ui_record/transcription_progress.h"
#include "ui_record/upload_queue.h"
#include "ui_theme/palette.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/wrapped_label.h"
#include "upload_widgets_support.h"

namespace {

using fv::appcore::OverlayState;
using fv::dictation::ProgressStage;
using fv::dictation::StepState;

QString styleSource(const QWidget& widget) {
    return widget.property(fv::ui::prop::unscaledStyleSheet).toString();
}

QString toneOf(const QWidget* widget) {
    return widget->property(fv::ui::prop::tone).toString();
}

QList<StepState> stepStates(const fv::TranscriptionProgressPanel& panel) {
    QList<StepState> states;
    for (const fv::UploadStepChip* chip : panel.steps()) {
        states.append(chip->state());
    }
    return states;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// TestDropZoneWidget

TEST_CASE("the drop zone is a 150 px tall drop target named dropZone", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::test_init_properties
    fv::DropZoneWidget widget;
    CHECK(widget.acceptDrops());
    CHECK(widget.minimumHeight() == 150);
    CHECK(widget.objectName() == QStringLiteral("dropZone"));
}

TEST_CASE("the drop zone knows the six audio extensions in any case", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::test_is_valid_audio
    fv::DropZoneWidget widget;
    for (const char* name :
         {"sample.wav", "audio.MP3", "voice.m4a", "track.ogg", "music.flac", "test.wma"}) {
        CHECK(widget.isValidAudio(QString::fromLatin1(name)));
    }
    for (const char* name : {"document.pdf", "image.png", "video.mp4"}) {
        CHECK_FALSE(widget.isValidAudio(QString::fromLatin1(name)));
    }
}

TEST_CASE("a drag enter accepts only audio", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::test_drag_enter_accepts_only_audio
    const auto [name, accepted] = GENERATE(std::pair<const char*, bool>{"test.wav", true},
                                           std::pair<const char*, bool>{"test.txt", false});
    fv::DropZoneWidget widget;
    const auto mime =
        fvtest::mimeOfPaths({QStringLiteral("/path/to/") + QString::fromLatin1(name)});
    QDragEnterEvent event(QPoint(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);
    CHECK(event.isAccepted() == accepted);
}

TEST_CASE("a drop emits the audio paths", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::test_drop_event_emits_files_selected
    fv::DropZoneWidget widget;
    QSignalSpy spy(&widget, &fv::DropZoneWidget::filesSelected);
    const auto mime = fvtest::mimeOfPaths({QStringLiteral("/path/to/sample.mp3")});
    QDropEvent event(QPointF(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);

    REQUIRE(spy.count() == 1);
    const QStringList paths = spy.at(0).at(0).toStringList();
    REQUIRE(paths.size() == 1);
    CHECK(paths[0].contains(QStringLiteral("sample.mp3")));
    CHECK(spy.at(0).at(1).toInt() == 0);
    CHECK(event.isAccepted());
}

TEST_CASE("a mixed drop emits the audio paths in order and counts the rest", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::
    // test_drop_with_mixed_files_emits_the_audio_paths_in_order_and_counts_the_rest
    fv::DropZoneWidget widget;
    QSignalSpy spy(&widget, &fv::DropZoneWidget::filesSelected);
    const auto mime =
        fvtest::mimeOfPaths({QStringLiteral("/path/a.mp3"), QStringLiteral("/path/notes.txt"),
                             QStringLiteral("/path/b.wav")});
    QDropEvent event(QPointF(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);

    REQUIRE(spy.count() == 1);
    QStringList names;
    for (const QString& path : spy.at(0).at(0).toStringList()) {
        names.append(QFileInfo(path).fileName());
    }
    CHECK(names == QStringList{QStringLiteral("a.mp3"), QStringLiteral("b.wav")});
    CHECK(spy.at(0).at(1).toInt() == 1);
}

TEST_CASE("a drop with no audio is ignored", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::test_drop_with_no_audio_is_ignored
    fv::DropZoneWidget widget;
    QSignalSpy spy(&widget, &fv::DropZoneWidget::filesSelected);
    const auto mime = fvtest::mimeOfPaths({QStringLiteral("/path/notes.txt")});
    QDropEvent event(QPointF(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);

    CHECK(spy.count() == 0);
    CHECK_FALSE(event.isAccepted());
}

TEST_CASE("audioPathsFromMime ignores non-local URLs", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDropZoneWidget::
    // test_audio_paths_from_mime_ignores_non_local_urls
    QMimeData mime;
    mime.setUrls({QUrl(QStringLiteral("https://example.com/a.mp3")),
                  QUrl::fromLocalFile(QStringLiteral("/x/b.mp3"))});

    const fv::DroppedAudio dropped = fv::audioPathsFromMime(&mime);
    REQUIRE(dropped.paths.size() == 1);
    CHECK(QFileInfo(dropped.paths[0]).fileName() == QStringLiteral("b.mp3"));
    CHECK(dropped.skipped == 1);

    QMimeData none;
    none.setText(QStringLiteral("no urls"));
    CHECK(fv::audioPathsFromMime(&none).paths.isEmpty());
    CHECK(fv::audioPathsFromMime(&none).skipped == 0);
}

TEST_CASE("the drop zone shows the accept style for audio and the reject style for the rest",
          "[widget]") {
    // Lead trap: three complete sheets, because child rules repeat their ancestors.
    fv::DropZoneWidget widget;
    const QString idle = styleSource(widget);
    CHECK(idle.contains(QStringLiteral("2px dashed @border-strong")));
    CHECK(idle.contains(QStringLiteral("QFrame#dropZone QLabel#dropZoneIcon")));

    const auto audio = fvtest::mimeOfPaths({QStringLiteral("/x/a.mp3")});
    QDragEnterEvent accept(QPoint(1, 1), Qt::CopyAction, audio.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &accept);
    CHECK(accept.isAccepted());
    const QString hover = styleSource(widget);
    CHECK(hover.contains(QStringLiteral("2px solid @accent")));
    CHECK(hover.contains(QStringLiteral("QFrame#dropZone QLabel {")));

    QDragLeaveEvent leave;
    fvtest::deliver(widget, &leave);
    CHECK(styleSource(widget) == idle);

    // URLs but no audio: the reject style, then the drop is ignored.
    const auto text = fvtest::mimeOfPaths({QStringLiteral("/x/a.txt")});
    QDragEnterEvent reject(QPoint(1, 1), Qt::CopyAction, text.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &reject);
    CHECK_FALSE(reject.isAccepted());
    const QString rejected = styleSource(widget);
    CHECK(rejected.contains(QStringLiteral("2px dashed @danger")));
    CHECK(rejected.contains(QStringLiteral("QFrame#dropZone QLabel {")));
    CHECK(rejected != idle);
}

TEST_CASE("a drag without URLs leaves the style alone", "[widget]") {
    fv::DropZoneWidget widget;
    const QString idle = styleSource(widget);
    QMimeData plain;
    plain.setText(QStringLiteral("hello"));
    QDragEnterEvent event(QPoint(1, 1), Qt::CopyAction, &plain, Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);
    CHECK_FALSE(event.isAccepted());
    CHECK(styleSource(widget) == idle);
}

TEST_CASE("dropping only non-audio files says so in the notice", "[widget]") {
    fv::DropZoneWidget widget;
    widget.show();
    CHECK(widget.noticeLabel()->isHidden());
    const auto mime = fvtest::mimeOfPaths({QStringLiteral("/x/notes.txt")});
    QDropEvent event(QPointF(1, 1), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(widget, &event);
    CHECK(widget.noticeLabel()->isVisible());
    CHECK(widget.noticeLabel()->text() ==
          QStringLiteral("Only audio files can be transcribed (WAV, MP3, M4A, OGG, FLAC, WMA)."));

    widget.setNotice(QString());
    CHECK(widget.noticeLabel()->isHidden());
}

TEST_CASE("a click asks for the file browser and the dialog's filters are OpenWhisper's",
          "[widget]") {
    fv::DropZoneWidget widget;
    widget.resize(300, 160);
    widget.show();
    QSignalSpy browse(&widget, &fv::DropZoneWidget::browseRequested);
    QTest::mouseClick(&widget, Qt::LeftButton);
    CHECK(browse.count() == 1);
    QTest::mouseClick(&widget, Qt::RightButton);
    CHECK(browse.count() == 1);
    CHECK(fv::audioFileFilters() ==
          QStringLiteral("Audio Files (*.wav *.mp3 *.m4a *.ogg *.flac *.wma);;"
                         "WAV Files (*.wav);;MP3 Files (*.mp3);;All Files (*.*)"));
}

// ---------------------------------------------------------------------------------------------
// TestFileInfoCard

TEST_CASE("setPreview fills the chips", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::test_set_preview_fills_the_chips
    fv::FileInfoCard card;
    card.setPreview(fvtest::samplePreview());

    CHECK(card.filenameLabel()->text() == QStringLiteral("sample.wav"));
    CHECK(card.sizeChip()->text() == QStringLiteral("1.0 MB"));
    CHECK(card.durationChip()->text() == QStringLiteral("1m 0s"));
    CHECK(card.rateChip()->text() == QStringLiteral("44.1 kHz"));
    CHECK(card.channelsChip()->text() == QStringLiteral("Stereo"));
    CHECK(card.chunkLabel()->text() == QStringLiteral("One pass"));
    CHECK(toneOf(card.chunkLabel()) == QStringLiteral("ok"));
}

TEST_CASE("a large file shows the chunk count as a warning", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_large_file_shows_the_chunk_count_as_a_warning
    fv::FileInfoCard card;
    fv::audio::AudioFilePreview preview = fvtest::samplePreview();
    preview.needs_splitting = true;
    preview.estimated_chunks = 5;
    card.setPreview(preview);

    CHECK(card.chunkLabel()->text() == QStringLiteral("About 5 chunks"));
    CHECK(toneOf(card.chunkLabel()) == QStringLiteral("warn"));
}

TEST_CASE("one channel is Mono and more than two read N ch", "[widget]") {
    fv::FileInfoCard card;
    fv::audio::AudioFilePreview preview = fvtest::samplePreview();
    preview.channels = 1;
    card.setPreview(preview);
    CHECK(card.channelsChip()->text() == QStringLiteral("Mono"));
    preview.channels = 6;
    card.setPreview(preview);
    CHECK(card.channelsChip()->text() == QStringLiteral("6 ch"));
}

TEST_CASE("a file still being read shows ellipses and Reading", "[widget]") {
    fv::FileInfoCard card;
    card.setPreview(fvtest::samplePreview());
    card.setLoading(QStringLiteral("interview.mp3"));
    CHECK(card.filenameLabel()->text() == QStringLiteral("interview.mp3"));
    for (const QLabel* chip :
         {card.sizeChip(), card.durationChip(), card.rateChip(), card.channelsChip()}) {
        CHECK(chip->text() == QStringLiteral(u"\u2026"));
    }
    CHECK(card.chunkLabel()->text() == QStringLiteral(u"Reading\u2026"));
    CHECK(toneOf(card.chunkLabel()).isEmpty());
}

TEST_CASE("setTranscribing swaps the footer for progress", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_set_transcribing_swaps_the_footer_for_progress
    fv::FileInfoCard card;
    card.setTranscribing(true, true);
    CHECK(card.isTranscribing());
    CHECK(card.isProgressShown());
    CHECK_FALSE(card.transcribeButton()->isEnabled());
    CHECK_FALSE(card.removeButton()->isEnabled());
    CHECK(card.progressPanel()->stage() == ProgressStage::Preparing);
    CHECK_FALSE(card.progressPanel()->steps()[2]->isHidden());

    card.setTranscribing(false);
    CHECK_FALSE(card.isTranscribing());
    CHECK_FALSE(card.isProgressShown());
    CHECK(card.transcribeButton()->isEnabled());
    CHECK(card.removeButton()->isEnabled());
}

TEST_CASE("the cleanup step only appears when cleanup is on", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_cleanup_step_only_appears_when_cleanup_is_on
    fv::FileInfoCard card;
    card.setTranscribing(true, false);
    CHECK(card.progressPanel()->steps()[2]->isHidden());
    CHECK(card.progressPanel()->connector(1)->isHidden());
}

TEST_CASE("finishing holds the result before the actions return", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_finish_holds_the_result_before_the_actions_return
    // The user should see Done land, not have the panel vanish under them.
    fv::FileInfoCard card;
    QList<bool> shown;
    QObject::connect(&card, &fv::FileInfoCard::progressShown,
                     [&shown](bool value) { shown.append(value); });
    card.setTranscribing(true, true);

    card.finishTranscribing(true);
    CHECK_FALSE(card.isTranscribing());
    CHECK(card.isProgressShown());
    CHECK(card.progressPanel()->stage() == ProgressStage::Done);
    for (const fv::UploadStepChip* chip : card.progressPanel()->steps()) {
        CHECK(chip->state() == StepState::Done);
    }
    CHECK(card.settleTimer()->isActive());
    CHECK(card.settleTimer()->interval() == 1400);

    fvtest::expire(card.settleTimer());
    CHECK_FALSE(card.isProgressShown());
    CHECK(shown == QList<bool>{true, false});
}

TEST_CASE("starting again cancels a pending return to the actions", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_starting_again_cancels_a_pending_return_to_actions
    fv::FileInfoCard card;
    card.setTranscribing(true);
    card.finishTranscribing(true);

    card.setTranscribing(true);
    CHECK_FALSE(card.settleTimer()->isActive());
    CHECK(card.progressPanel()->stage() == ProgressStage::Preparing);
}

TEST_CASE("setCopyEnabled turns the Copy button's activity on and off", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::test_set_copy_enabled
    fv::FileInfoCard card;
    card.setCopyEnabled(true);
    CHECK(card.copyButton()->isActive());
    card.setCopyEnabled(false);
    CHECK_FALSE(card.copyButton()->isActive());
}

TEST_CASE("Stop in the card's panel emits stopClicked", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::test_stop_emits_cancel
    fv::FileInfoCard card;
    QSignalSpy canceled(&card, &fv::FileInfoCard::stopClicked);
    card.setTranscribing(true);
    CHECK(card.progressPanel()->stopButton()->isEnabled());

    card.progressPanel()->stopButton()->click();
    CHECK(canceled.count() == 1);
}

TEST_CASE("a canceled panel stays canceled when the job ends", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_a_canceled_panel_stays_canceled_when_the_job_ends
    // The runtime follows a cancel with an error transcript; that is not a failure.
    fv::FileInfoCard card;
    card.setTranscribing(true, true);
    card.progressPanel()->setStage(ProgressStage::Transcribing);
    card.progressPanel()->applyOverlayState(OverlayState::Canceling);

    card.finishTranscribing(false);

    CHECK(card.progressPanel()->stage() == ProgressStage::Canceled);
    CHECK_FALSE(card.isTranscribing());
    CHECK(card.settleTimer()->isActive());
    CHECK_FALSE(card.progressPanel()->stopButton()->isEnabled());
}

TEST_CASE("readiness gates Transcribe without latching", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_readiness_gates_transcribe_without_latching
    fv::FileInfoCard card;
    card.setReady(false);
    CHECK_FALSE(card.transcribeButton()->isEnabled());

    card.setTranscribing(true);
    card.finishTranscribing(true);
    CHECK_FALSE(card.transcribeButton()->isEnabled());

    card.setReady(true);
    CHECK(card.transcribeButton()->isEnabled());
}

TEST_CASE("single mode shows the header and queue mode swaps it", "[widget]") {
    // from tests/test_upload_file_tab.py::TestFileInfoCard::
    // test_single_mode_shows_the_header_and_queue_mode_swaps_it
    fv::FileInfoCard card;
    CHECK(card.mode() == fv::FileInfoCard::Mode::Single);
    CHECK(card.queue()->isHidden());
    CHECK(card.copyButton()->text() == QStringLiteral("Copy"));

    card.setMode(fv::FileInfoCard::Mode::Queue);
    CHECK(card.headerRow()->isHidden());
    CHECK_FALSE(card.queue()->isHidden());
    CHECK(card.copyButton()->text() == QStringLiteral("Copy all"));

    card.setMode(fv::FileInfoCard::Mode::Single);
    CHECK_FALSE(card.headerRow()->isHidden());
    CHECK(card.queue()->isHidden());
    CHECK(card.copyButton()->text() == QStringLiteral("Copy"));
}

TEST_CASE("the card's controls match the Python layout", "[widget]") {
    fv::FileInfoCard card;
    CHECK(card.objectName() == QStringLiteral("uploadFileCard"));
    CHECK(card.acceptDrops());
    CHECK(card.copyButton()->objectName() == QStringLiteral("uploadCopyButton"));
    CHECK(card.copyButton()->minimumWidth() >= 96);
    CHECK(card.footer()->objectName() == QStringLiteral("uploadFooter"));
    CHECK(card.footer()->count() == 2);
    CHECK(card.footer()->widget(1) == card.progressPanel());
    CHECK(card.resultLabel()->objectName() == QStringLiteral("uploadResultNote"));
    CHECK(card.resultLabel()->isHidden());
    CHECK_FALSE(card.copyButton()->isActive());
}

TEST_CASE("the result note reads time, speed and cleanup in bold runs", "[widget]") {
    // Spec reference: "Transcribed in 1m 2s · 13.6× realtime".
    fv::FileInfoCard card;
    card.setResult(62.0, 843.2);
    CHECK(card.resultLabel()->isHidden() == false);
    QString text = card.resultLabel()->text();
    CHECK(text.contains(QStringLiteral("Transcribed in ")));
    CHECK(text.contains(QStringLiteral(">1m 2s</span>")));
    CHECK(text.contains(QStringLiteral(">13.6\u00d7</span>")));
    CHECK(text.contains(QStringLiteral(" realtime")));
    CHECK_FALSE(text.contains(QStringLiteral("Cleaned in")));

    card.setResult(2.0, 0.0, 3.2);
    text = card.resultLabel()->text();
    CHECK_FALSE(text.contains(QStringLiteral("realtime")));
    CHECK(text.contains(QStringLiteral("Cleaned in ")));
    CHECK(text.contains(QStringLiteral(">3.2s</span>")));

    card.setResult(2.0, 10.0, 0.0);
    CHECK_FALSE(card.resultLabel()->text().contains(QStringLiteral("Cleaned in")));

    fv::RemoteResultTiming remote{QStringLiteral("studio"), 0.031};
    card.setResult(2.0, 10.0, std::nullopt, remote);
    text = card.resultLabel()->text();
    CHECK(text.contains(QStringLiteral(" on studio")));
    CHECK(text.contains(QStringLiteral(">31 ms</span>")));
    CHECK(text.contains(QStringLiteral(" network")));
}

TEST_CASE("an error note is danger-toned with the message as its tooltip", "[widget]") {
    fv::FileInfoCard card;
    card.setResult(2.0, 10.0);
    card.setErrorNote(QStringLiteral("The model is not loaded."));
    CHECK(toneOf(card.resultLabel()) == QStringLiteral("danger"));
    CHECK(card.resultLabel()->toolTip() == QStringLiteral("The model is not loaded."));
    CHECK(card.resultLabel()->text() == QStringLiteral("The model is not loaded."));
    CHECK_FALSE(card.resultLabel()->isRevealing());

    card.clearResult();
    CHECK(toneOf(card.resultLabel()).isEmpty());
    CHECK(card.resultLabel()->toolTip().isEmpty());
    CHECK(card.resultLabel()->isHidden());

    // setError, the spec's name for it, is the same note.
    card.setError(QStringLiteral("Stopped."));
    CHECK(toneOf(card.resultLabel()) == QStringLiteral("danger"));
    CHECK(card.resultLabel()->toolTip() == QStringLiteral("Stopped."));
    CHECK(card.resultLabel()->text() == QStringLiteral("Stopped."));
    CHECK_FALSE(card.resultLabel()->isHidden());
}

TEST_CASE("a new job clears the previous result", "[widget]") {
    fv::FileInfoCard card;
    card.setResult(2.0, 10.0);
    card.setTranscribing(true);
    CHECK(card.resultLabel()->isHidden());
    CHECK(card.resultLabel()->text().isEmpty());
}

TEST_CASE("the card accepts audio drops until a job runs", "[widget]") {
    fv::FileInfoCard card;
    QSignalSpy dropped(&card, &fv::FileInfoCard::filesDropped);
    const auto audio =
        fvtest::mimeOfPaths({QStringLiteral("/x/a.mp3"), QStringLiteral("/x/n.txt")});
    {
        QDragEnterEvent enter(QPoint(1, 1), Qt::CopyAction, audio.get(), Qt::NoButton,
                              Qt::NoModifier);
        fvtest::deliver(card, &enter);
        CHECK(enter.isAccepted());
        QDropEvent drop(QPointF(1, 1), Qt::CopyAction, audio.get(), Qt::NoButton, Qt::NoModifier);
        fvtest::deliver(card, &drop);
        CHECK(drop.isAccepted());
    }
    REQUIRE(dropped.count() == 1);
    CHECK(dropped.at(0).at(0).toStringList().size() == 1);
    CHECK(dropped.at(0).at(1).toInt() == 1);

    const auto text = fvtest::mimeOfPaths({QStringLiteral("/x/n.txt")});
    QDragEnterEvent notAudio(QPoint(1, 1), Qt::CopyAction, text.get(), Qt::NoButton,
                             Qt::NoModifier);
    fvtest::deliver(card, &notAudio);
    CHECK_FALSE(notAudio.isAccepted());

    card.setTranscribing(true);
    QDragEnterEvent while_running(QPoint(1, 1), Qt::CopyAction, audio.get(), Qt::NoButton,
                                  Qt::NoModifier);
    fvtest::deliver(card, &while_running);
    CHECK_FALSE(while_running.isAccepted());
    QDropEvent drop(QPointF(1, 1), Qt::CopyAction, audio.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(card, &drop);
    CHECK_FALSE(drop.isAccepted());
    CHECK(dropped.count() == 1);
}

TEST_CASE("the footer keeps one height whichever page shows", "[widget]") {
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::FileInfoCard card;
    card.setPreview(fvtest::samplePreview());
    card.resize(560, 220);
    card.show();
    QApplication::processEvents();
    const int idleHeight = card.footer()->height();
    const int cardHeight = card.height();
    card.setTranscribing(true);
    QApplication::processEvents();
    CHECK(card.footer()->height() == idleHeight);
    CHECK(card.height() == cardHeight);
}

// ---------------------------------------------------------------------------------------------
// TestTranscriptionProgressPanel

TEST_CASE("overlay states map onto stages", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_overlay_states_map_onto_stages
    using fv::dictation::stage_for_overlay_state;
    CHECK(stage_for_overlay_state(OverlayState::Processing) == ProgressStage::Preparing);
    CHECK(stage_for_overlay_state(OverlayState::Transcribing) == ProgressStage::Transcribing);
    CHECK(stage_for_overlay_state(OverlayState::Cleaning) == ProgressStage::Cleaning);
    CHECK(stage_for_overlay_state(OverlayState::Canceling) == ProgressStage::Canceled);
    CHECK_FALSE(stage_for_overlay_state(OverlayState::Recording).has_value());
    CHECK_FALSE(stage_for_overlay_state(OverlayState::None).has_value());
}

TEST_CASE("progress stages for the new overlay states", "[widget]") {
    // from tests/test_personalize_foundation_ui_shell.py::
    // test_progress_stages_for_the_new_overlay_states
    using fv::dictation::stage_for_overlay_state;
    CHECK(stage_for_overlay_state(OverlayState::Rewriting) == ProgressStage::Cleaning);
    CHECK_FALSE(stage_for_overlay_state(OverlayState::CommandListening).has_value());
}

TEST_CASE("the steps follow the stage", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_steps_follow_the_stage
    fv::TranscriptionProgressPanel panel;
    panel.start(true);
    CHECK(stepStates(panel) ==
          QList<StepState>{StepState::Active, StepState::Pending, StepState::Pending});

    panel.setStage(ProgressStage::Transcribing);
    CHECK(stepStates(panel) ==
          QList<StepState>{StepState::Done, StepState::Active, StepState::Pending});

    panel.setStage(ProgressStage::Cleaning);
    CHECK(stepStates(panel) ==
          QList<StepState>{StepState::Done, StepState::Done, StepState::Active});
}

TEST_CASE("a cancel marks the step that was running", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_cancel_marks_the_step_that_was_running
    fv::TranscriptionProgressPanel panel;
    panel.start(true);
    panel.setStage(ProgressStage::Transcribing);

    panel.applyOverlayState(OverlayState::Canceling);
    CHECK(panel.stage() == ProgressStage::Canceled);
    CHECK_FALSE(panel.isRunning());
    CHECK(stepStates(panel) ==
          QList<StepState>{StepState::Done, StepState::Failed, StepState::Pending});
}

TEST_CASE("a stage in Failed turns the running step failed and keeps the others", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    panel.start(true);
    panel.setStage(ProgressStage::Cleaning);
    panel.finish(false);
    CHECK(panel.stage() == ProgressStage::Failed);
    CHECK(stepStates(panel) ==
          QList<StepState>{StepState::Done, StepState::Done, StepState::Failed});
}

TEST_CASE("the large-file notice names the size", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_large_file_notice_names_the_size
    fv::TranscriptionProgressPanel panel;
    panel.start(false);

    panel.setLargeFile(30.0);
    CHECK(panel.stage() == ProgressStage::Splitting);
    CHECK(panel.detailLabel()->text() == QStringLiteral("30.0 MB file"));
}

TEST_CASE("the bar sweeps while running and fills when done", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_bar_sweeps_while_running_and_fills_when_done
    fv::TranscriptionProgressPanel panel;
    panel.start(false);
    CHECK(panel.bar()->isIndeterminate());

    panel.finish(true);
    CHECK_FALSE(panel.bar()->isIndeterminate());
    CHECK(panel.bar()->fraction() == 1.0);
}

TEST_CASE("elapsed time formats as m:ss", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::test_elapsed_format
    CHECK(fv::dictation::format_elapsed(0) == "0:00");
    CHECK(fv::dictation::format_elapsed(65.9) == "1:05");
}

TEST_CASE("a batch position sets the header label and a determinate fraction", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_batch_position_sets_the_header_label_and_a_determinate_fraction
    fv::TranscriptionProgressPanel panel;
    panel.start(true, 3);
    CHECK(panel.isBatch());
    CHECK_FALSE(panel.bar()->isIndeterminate());
    CHECK_FALSE(panel.batchLabel()->isHidden());

    panel.setBatchPosition(2, 3, QStringLiteral("b.mp3"));
    CHECK(panel.batchLabel()->text() == QStringLiteral(u"File 2 of 3  \u00b7  b.mp3"));
    CHECK(panel.bar()->fraction() == Catch::Approx(1.0 / 3.0));
}

TEST_CASE("stage changes do not re-arm the sweep during a batch", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_stage_changes_do_not_rearm_the_sweep_during_a_batch
    fv::TranscriptionProgressPanel panel;
    panel.start(true, 2);

    panel.setStage(ProgressStage::Transcribing);
    CHECK_FALSE(panel.bar()->isIndeterminate());
    CHECK(panel.bar()->fraction() == Catch::Approx(0.2 / 2));

    panel.setBatchPosition(2, 2, QStringLiteral("b.mp3"));
    panel.setStage(ProgressStage::Cleaning);
    CHECK(panel.bar()->fraction() == Catch::Approx((1 + 0.85) / 2));
}

TEST_CASE("a single file's bar still sweeps", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_single_file_bar_still_sweeps
    fv::TranscriptionProgressPanel panel;
    panel.start(false);
    CHECK_FALSE(panel.isBatch());
    CHECK(panel.batchLabel()->isHidden());

    panel.setStage(ProgressStage::Transcribing);
    CHECK(panel.bar()->isIndeterminate());
}

TEST_CASE("Stop is disabled once the job ends", "[widget]") {
    // from tests/test_upload_file_tab.py::TestTranscriptionProgressPanel::
    // test_stop_is_disabled_once_the_job_ends
    fv::TranscriptionProgressPanel panel;
    panel.start(false);
    CHECK(panel.stopButton()->isEnabled());

    panel.setStopping();
    CHECK_FALSE(panel.stopButton()->isEnabled());
    CHECK(panel.detailLabel()->text() == QStringLiteral(u"Canceling\u2026"));

    panel.start(false);
    panel.finish(true);
    CHECK_FALSE(panel.stopButton()->isEnabled());
}

TEST_CASE("the batch position is clamped but the label shows what was given", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    panel.start(true, 3);
    panel.setBatchPosition(9, 3, QStringLiteral("z.mp3"));
    CHECK(panel.batchLabel()->text() == QStringLiteral(u"File 9 of 3  \u00b7  z.mp3"));
    // Position 3 of 3: (3 - 1) / 3.
    CHECK(panel.bar()->fraction() == Catch::Approx(2.0 / 3.0));
    panel.setBatchPosition(0, 3, QStringLiteral("a.mp3"));
    CHECK(panel.bar()->fraction() == 0.0);
}

TEST_CASE("the elapsed clock reads the injected time and freezes at a terminal stage", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    double now = 100.0;
    panel.setClock([&now] { return now; });
    panel.start(false);
    CHECK(panel.elapsedLabel()->text() == QStringLiteral("0:00"));

    now = 142.0;
    panel.tick();
    CHECK(panel.elapsedLabel()->text() == QStringLiteral("0:42"));

    now = 190.0;
    panel.finish(true);
    CHECK(panel.elapsedLabel()->text() == QStringLiteral("1:30"));
    now = 400.0;
    panel.tick(); // the clock's own timer is stopped; a stray tick would still write the time
    CHECK(panel.elapsedLabel()->text() == QStringLiteral("5:00"));
}

TEST_CASE("the elapsed clock ticks each second, stops on hide and resumes on show", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    auto* clock = panel.findChild<QTimer*>(QString(), Qt::FindDirectChildrenOnly);
    REQUIRE(clock != nullptr);
    CHECK(clock->interval() == 1000);
    panel.start(false);
    CHECK(clock->isActive());
    panel.show();
    CHECK(clock->isActive());
    panel.hide();
    CHECK_FALSE(clock->isActive());
    panel.show();
    CHECK(clock->isActive());
    panel.finish(true);
    CHECK_FALSE(clock->isActive());
    panel.hide();
    panel.show();
    CHECK_FALSE(clock->isActive());
}

TEST_CASE("applyOverlayState reports whether the state is a stage", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    panel.start(true);
    CHECK(panel.applyOverlayState(OverlayState::Transcribing));
    CHECK(panel.stage() == ProgressStage::Transcribing);
    CHECK_FALSE(panel.applyOverlayState(OverlayState::Recording));
    CHECK(panel.stage() == ProgressStage::Transcribing);
}

TEST_CASE("the title carries the stage as its property", "[widget]") {
    fv::TranscriptionProgressPanel panel;
    panel.start(false);
    CHECK(panel.titleLabel()->text() == QStringLiteral("Preparing audio"));
    CHECK(panel.titleLabel()->property(fv::ui::prop::stage).toString() ==
          QStringLiteral("preparing"));
    panel.finish(false);
    CHECK(panel.titleLabel()->text() == QStringLiteral("Failed"));
    CHECK(panel.titleLabel()->property(fv::ui::prop::stage).toString() == QStringLiteral("failed"));
}

// ---------------------------------------------------------------------------------------------
// TestDecodeLabel (against fv::DecodeLabel)

namespace {

const fv::DecodeSegments kSegments{{QStringLiteral("Transcribed in "), false},
                                   {QStringLiteral("6.9s"), true}};

QString shownText(const QLabel& label) {
    return label.QLabel::text();
}

} // namespace

TEST_CASE("immediate decode text renders emphasis", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDecodeLabel::test_immediate_text_renders_emphasis
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(kSegments, false);
    CHECK(label.text().contains(QStringLiteral("Transcribed in ")));
    const QString emphasis = QStringLiteral("<span style='color:%1; font-weight:600'>6.9s</span>")
                                 .arg(fv::ui::currentPalette().qCss(u"text"));
    CHECK(label.text().contains(emphasis));
    CHECK_FALSE(label.isRevealing());
}

TEST_CASE("the decode reveal waits until the label is shown", "[widget]") {
    // from
    // tests/test_upload_file_tab.py::TestDecodeLabel::test_reveal_waits_until_the_label_is_shown A
    // result arriving behind the progress panel must not spend its animation unseen.
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(kSegments);
    CHECK_FALSE(label.isRevealing());
    CHECK(shownText(label).isEmpty());
    CHECK(label.text().contains(QStringLiteral("6.9s")));

    label.show();
    QApplication::processEvents();
    // Host load can drain the 720 ms timer during this single processEvents call; either the
    // reveal is in flight or it already landed on the finished text. The show-gated start is
    // what must not regress.
    CHECK((label.isRevealing() || shownText(label) == label.text()));
}

TEST_CASE("a partial decode frame scrambles ahead of the locked run", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDecodeLabel::
    // test_partial_frame_scrambles_ahead_of_the_locked_run
    // The port's scramble head is four characters (DecodeLabel::kHead); Python's test names
    // three through the private _render.
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(kSegments, false);
    const QString frame = label.frameAt(5.0 / 19.0);
    CHECK(frame.startsWith(QStringLiteral("Trans")));
    const QString scramble =
        QStringLiteral("color:%1").arg(fv::ui::currentPalette().qCss(u"accent"));
    CHECK(frame.count(scramble) == 4);
    CHECK_FALSE(frame.contains(QStringLiteral("6.9s")));
}

TEST_CASE("the finished decode frame matches the immediate text", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDecodeLabel::
    // test_finished_frame_matches_the_immediate_text
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(kSegments, false);
    CHECK(label.frameAt(1.0) == label.text());
}

TEST_CASE("hiding a decode mid-reveal lands on the final text", "[widget]") {
    // from tests/test_upload_file_tab.py::TestDecodeLabel::
    // test_hiding_mid_reveal_lands_on_the_final_text
    const fvtest::AppTheme theme(QStringLiteral("dark"));
    fv::DecodeLabel label;
    label.setSegments(kSegments);
    label.show();
    QApplication::processEvents();
    label.hide();
    CHECK_FALSE(label.isRevealing());
    CHECK(shownText(label) == label.text());
}

TEST_CASE("sample rates read in kHz", "[widget]") {
    // from tests/test_upload_file_tab.py::test_format_sample_rate
    CHECK(fv::format_sample_rate(44100) == "44.1 kHz");
    CHECK(fv::format_sample_rate(16000) == "16 kHz");
    CHECK(fv::format_sample_rate(48000) == "48 kHz");
    CHECK(fv::format_sample_rate(0).empty());
}

// ---------------------------------------------------------------------------------------------
// The queue and the relation picker (rows are ported by M2.21's tab tests; these pin the widgets)

namespace {

fv::QueueItem queued(const QString& path, bool withPreview = true) {
    fv::QueueItem item;
    item.path = path;
    if (withPreview) {
        item.preview = fvtest::samplePreview();
        item.state = QStringLiteral("pending");
    }
    return item;
}

} // namespace

TEST_CASE("a queue item's key folds the path the way ntpath.normcase does", "[widget]") {
#ifdef Q_OS_WIN
    CHECK(fv::QueueItem::keyFor(QStringLiteral("C:/Audio/A.MP3")) ==
          fv::QueueItem::keyFor(QStringLiteral("c:\\audio\\a.mp3")));
    CHECK(fv::QueueItem::keyFor(QStringLiteral("C:/Audio/A.MP3")) !=
          fv::QueueItem::keyFor(QStringLiteral("C:/Audio/B.MP3")));
    // os.path.abspath normalises the path as well: "." and ".." segments and doubled
    // separators name the same file, so it is queued once.
    CHECK(fv::QueueItem::keyFor(QStringLiteral("C:/Audio/./talks/../A.mp3")) ==
          fv::QueueItem::keyFor(QStringLiteral("C:\\Audio\\A.mp3")));
    CHECK(fv::QueueItem::keyFor(QStringLiteral("C:/Audio//A.mp3")) ==
          QStringLiteral("c:\\audio\\a.mp3"));
#else
    CHECK(fv::QueueItem::keyFor(QStringLiteral("/a/A.mp3")) !=
          fv::QueueItem::keyFor(QStringLiteral("/a/a.mp3")));
    CHECK(fv::QueueItem::keyFor(QStringLiteral("/a/./b/../A.mp3")) == QStringLiteral("/a/A.mp3"));
    CHECK(fv::QueueItem::keyFor(QStringLiteral("/a//A.mp3")) == QStringLiteral("/a/A.mp3"));
#endif
    fv::QueueItem item;
    item.path = QStringLiteral("C:\\interviews\\one.mp3");
    CHECK(item.name() == QStringLiteral("one.mp3"));
}

TEST_CASE("the queue lists rows in order and counts them", "[widget]") {
    fv::UploadQueueWidget queue;
    CHECK(queue.objectName() == QStringLiteral("uploadQueue"));
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    CHECK(queue.titleLabel()->text() == QStringLiteral("2 files"));
    const auto rows = queue.rows();
    REQUIRE(rows.size() == 2);
    CHECK(rows[0]->nameLabel()->text() == QStringLiteral("one.mp3"));
    CHECK(rows[1]->nameLabel()->text() == QStringLiteral("two.mp3"));
    CHECK_FALSE(rows[0]->upButton()->isEnabled());
    CHECK(rows[0]->downButton()->isEnabled());
    CHECK(rows[1]->upButton()->isEnabled());
    CHECK_FALSE(rows[1]->downButton()->isEnabled());
    CHECK(rows[0]->sizeChip()->text() == QStringLiteral("1.0 MB"));
    CHECK(rows[0]->durationChip()->text() == QStringLiteral("1m 0s"));
    CHECK(rows[0]->chunkChip()->text() == QStringLiteral("One pass"));
    CHECK(toneOf(rows[0]->chunkChip()) == QStringLiteral("ok"));
    CHECK(rows[0]->toolTip() == QStringLiteral("/a/one.mp3"));
    CHECK(rows[0]->stateChip()->text() == QStringLiteral("Queued"));

    queue.setItems({queued(QStringLiteral("/a/one.mp3"))});
    CHECK(queue.titleLabel()->text() == QStringLiteral("1 file"));
}

TEST_CASE("rows are rebuilt only when the order of keys changes", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    fv::UploadQueueRow* first = queue.rows()[0];

    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    CHECK(queue.rows()[0] == first);

    queue.setItems({queued(QStringLiteral("/a/two.mp3")), queued(QStringLiteral("/a/one.mp3"))});
    CHECK(queue.rows()[0] != first);
    CHECK(queue.rows()[0]->path() == QStringLiteral("/a/two.mp3"));
    CHECK(queue.rowFor(QStringLiteral("/a/one.mp3")) == queue.rows()[1]);
    CHECK(queue.rowFor(QStringLiteral("/a/none.mp3")) == nullptr);
}

TEST_CASE("an unread file reads Reading and an unreadable one Could not read", "[widget]") {
    fv::UploadQueueWidget queue;
    fv::QueueItem failed = queued(QStringLiteral("/a/bad.mp3"), false);
    failed.error = QStringLiteral("This doesn't look like an audio file.");
    queue.setItems({queued(QStringLiteral("/a/loading.mp3"), false), failed});
    const auto rows = queue.rows();
    CHECK(rows[0]->sizeChip()->text() == QStringLiteral(u"Reading\u2026"));
    CHECK(rows[0]->durationChip()->text() == QStringLiteral(u"\u2026"));
    CHECK(rows[0]->chunkChip()->isHidden());
    CHECK(rows[0]->state() == QStringLiteral("reading"));
    CHECK(rows[0]->stateChip()->text() == QStringLiteral(u"Reading\u2026"));

    CHECK(rows[1]->sizeChip()->text() == QStringLiteral("Could not read"));
    CHECK(rows[1]->state() == QStringLiteral("failed"));
    CHECK(rows[1]->stateChip()->text() == QStringLiteral("Failed"));
    CHECK(rows[1]->toolTip() == QStringLiteral("This doesn't look like an audio file."));
}

TEST_CASE("row state drives the state chip and both state properties", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    queue.setRowState(QStringLiteral("/a/one.mp3"), QStringLiteral("active"));
    fv::UploadQueueRow* row = queue.rowFor(QStringLiteral("/a/one.mp3"));
    CHECK(row->stateChip()->text() == QStringLiteral("Working"));
    CHECK(row->property(fv::ui::prop::state).toString() == QStringLiteral("active"));
    CHECK(row->stateChip()->property(fv::ui::prop::state).toString() == QStringLiteral("active"));
    const std::pair<const char*, const char*> states[] = {
        {"done", "Done"}, {"stopped", "Stopped"}, {"pending", "Queued"}};
    for (const auto& [state, text] : states) {
        queue.setRowState(QStringLiteral("/a/one.mp3"), QString::fromLatin1(state));
        CHECK(row->stateChip()->text() == QString::fromLatin1(text));
    }
    queue.setRowState(QStringLiteral("/a/none.mp3"), QStringLiteral("done")); // no such row
}

TEST_CASE("a row's copy button shows only while it has text", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.show();
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    // A row added to a shown parent is shown by a queued call.
    QApplication::processEvents();
    QSignalSpy copied(&queue, &fv::UploadQueueWidget::rowCopyRequested);
    fv::UploadQueueRow* row = queue.rowFor(QStringLiteral("/a/one.mp3"));
    CHECK(row->copyButton()->isHidden());

    queue.setRowTranscript(QStringLiteral("/a/one.mp3"), QStringLiteral("hello"));
    CHECK(row->copyButton()->isVisible());
    CHECK(row->transcript() == QStringLiteral("hello"));
    row->copyButton()->click();
    REQUIRE(copied.count() == 1);
    CHECK(copied.at(0).at(0).toString() == QStringLiteral("/a/one.mp3"));

    queue.setRowTranscript(QStringLiteral("/a/one.mp3"), QString());
    CHECK(row->copyButton()->isHidden());
}

TEST_CASE("the queue's buttons announce their intent with the row's path", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    QSignalSpy move(&queue, &fv::UploadQueueWidget::moveRequested);
    QSignalSpy remove(&queue, &fv::UploadQueueWidget::removeRequested);
    QSignalSpy add(&queue, &fv::UploadQueueWidget::addRequested);
    QSignalSpy clear(&queue, &fv::UploadQueueWidget::clearRequested);

    queue.rowFor(QStringLiteral("/a/one.mp3"))->downButton()->click();
    queue.rowFor(QStringLiteral("/a/two.mp3"))->upButton()->click();
    queue.rowFor(QStringLiteral("/a/two.mp3"))->removeButton()->click();
    queue.addButton()->click();
    queue.clearButton()->click();

    REQUIRE(move.count() == 2);
    CHECK(move.at(0).at(0).toString() == QStringLiteral("/a/one.mp3"));
    CHECK(move.at(0).at(1).toInt() == 1);
    CHECK(move.at(1).at(0).toString() == QStringLiteral("/a/two.mp3"));
    CHECK(move.at(1).at(1).toInt() == -1);
    REQUIRE(remove.count() == 1);
    CHECK(remove.at(0).at(0).toString() == QStringLiteral("/a/two.mp3"));
    CHECK(add.count() == 1);
    CHECK(clear.count() == 1);
}

TEST_CASE("locking the queue disables Add and Clear and hides move and remove", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.show();
    queue.setItems({queued(QStringLiteral("/a/one.mp3")), queued(QStringLiteral("/a/two.mp3"))});
    QApplication::processEvents();
    queue.setLocked(true);
    CHECK_FALSE(queue.addButton()->isEnabled());
    CHECK_FALSE(queue.clearButton()->isEnabled());
    CHECK_FALSE(queue.picker()->combo()->isEnabled());
    for (fv::UploadQueueRow* row : queue.rows()) {
        CHECK(row->upButton()->isHidden());
        CHECK(row->downButton()->isHidden());
        CHECK(row->removeButton()->isHidden());
    }
    queue.setLocked(false);
    CHECK(queue.addButton()->isEnabled());
    for (fv::UploadQueueRow* row : queue.rows()) {
        CHECK(row->removeButton()->isVisible());
    }
}

TEST_CASE("the queue note is dotted, warn-toned for a problem and tooltipped", "[widget]") {
    fv::UploadQueueWidget queue;
    queue.show();
    CHECK(queue.noteLabel()->isHidden());
    queue.setNote(QStringLiteral("1 already queued"));
    CHECK(queue.noteLabel()->isVisible());
    CHECK(queue.noteLabel()->text() == QStringLiteral(u"\u00b7  1 already queued"));
    CHECK(queue.noteLabel()->toolTip() == QStringLiteral("1 already queued"));
    CHECK(toneOf(queue.noteLabel()).isEmpty());
    queue.setNote(QStringLiteral("1 could not be read"), true);
    CHECK(toneOf(queue.noteLabel()) == QStringLiteral("warn"));
    queue.setNote(QString());
    CHECK(queue.noteLabel()->isHidden());
}

TEST_CASE("the relation picker offers three presets with their hints", "[widget]") {
    fv::RelationPicker picker;
    CHECK(picker.objectName() == QStringLiteral("uploadRelationRow"));
    CHECK(picker.combo()->count() == 3);
    CHECK(picker.combo()->itemText(0) == QStringLiteral("Separate recordings"));
    CHECK(picker.combo()->itemText(1) == QStringLiteral("Parts of one recording, in order"));
    CHECK(picker.combo()->itemText(2) == QStringLiteral(u"Custom\u2026"));
    CHECK(picker.relation() == fv::dictation::BatchRelation::Separate);
    CHECK(picker.hint()->text() ==
          QStringLiteral("Each file is cleaned on its own and saved as its own transcript."));
    CHECK(picker.editButton()->isHidden());
}

TEST_CASE("choosing a preset announces it and shows its hint", "[widget]") {
    fv::RelationPicker picker;
    QSignalSpy changed(&picker, &fv::RelationPicker::relationChanged);
    picker.combo()->setCurrentIndex(1);
    REQUIRE(changed.count() == 1);
    CHECK(changed.at(0).at(0).toString() == QStringLiteral("sequential"));
    CHECK(picker.hint()->text() ==
          QStringLiteral("Stitched into one transcript; overlap at the seams is removed."));
    CHECK(picker.editButton()->isHidden());

    picker.combo()->setCurrentIndex(2);
    CHECK(changed.at(1).at(0).toString() == QStringLiteral("custom"));
    CHECK(picker.hint()->text() ==
          QStringLiteral("Describe the files and how the cleanup should treat them."));
    CHECK_FALSE(picker.editButton()->isHidden());
}

TEST_CASE("setRelation shows a preset without announcing it", "[widget]") {
    fv::RelationPicker picker;
    QSignalSpy changed(&picker, &fv::RelationPicker::relationChanged);
    picker.setRelation(fv::dictation::BatchRelation::Sequential);
    CHECK(changed.count() == 0);
    CHECK(picker.relation() == fv::dictation::BatchRelation::Sequential);
    CHECK(picker.combo()->currentIndex() == 1);
    CHECK(picker.hint()->text() ==
          QStringLiteral("Stitched into one transcript; overlap at the seams is removed."));
}

TEST_CASE("the custom summary names the shape and truncates the description", "[widget]") {
    fv::RelationPicker picker;
    picker.setCustomSummary(QString(), true);
    CHECK(picker.hint()->text() == QStringLiteral("One combined transcript"));
    CHECK_FALSE(picker.editButton()->isHidden());

    picker.setCustomSummary(QStringLiteral("  Two   halves\n of one talk. "), false);
    CHECK(picker.hint()->text() ==
          QStringLiteral(u"One transcript per file  \u00b7  Two halves of one talk."));

    // Exactly 90 characters stay whole; 91 become the first 87 and an ellipsis.
    picker.setCustomSummary(QString(90, QLatin1Char('x')), true);
    CHECK(picker.hint()->text() ==
          QStringLiteral(u"One combined transcript  \u00b7  ") + QString(90, QLatin1Char('x')));
    picker.setCustomSummary(QString(91, QLatin1Char('x')), true);
    CHECK(picker.hint()->text() == QStringLiteral(u"One combined transcript  \u00b7  ") +
                                       QString(87, QLatin1Char('x')) + QStringLiteral(u"\u2026"));
    // A space at the cut is stripped before the ellipsis.
    picker.setCustomSummary(QString(86, QLatin1Char('x')) + QStringLiteral(" yyyyyyyy"), true);
    CHECK(picker.hint()->text() == QStringLiteral(u"One combined transcript  \u00b7  ") +
                                       QString(86, QLatin1Char('x')) + QStringLiteral(u"\u2026"));
}

TEST_CASE("the description is cut by code points, not UTF-16 units", "[widget]") {
    fv::RelationPicker picker;
    const QString smiley = QString::fromUcs4(U"\U0001F600");
    QString text;
    for (int i = 0; i < 91; ++i) {
        text += smiley;
    }
    picker.setCustomSummary(text, false);
    QString expected;
    for (int i = 0; i < 87; ++i) {
        expected += smiley;
    }
    CHECK(picker.hint()->text() == QStringLiteral(u"One transcript per file  \u00b7  ") + expected +
                                       QStringLiteral(u"\u2026"));
}

TEST_CASE("locking the picker disables the combo and the edit button", "[widget]") {
    fv::RelationPicker picker;
    picker.setLocked(true);
    CHECK_FALSE(picker.combo()->isEnabled());
    CHECK_FALSE(picker.editButton()->isEnabled());
    picker.setLocked(false);
    CHECK(picker.combo()->isEnabled());
}
