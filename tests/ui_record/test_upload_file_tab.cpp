// The Upload File tab (ui_qt/widgets/upload_file_tab.py UploadFileTab and the engine card it
// inherits): the queue model, the previews read off the GUI thread, the job's lifecycle on the
// card, copying and the reading window, and the engine card's backends. The preview worker runs
// inline or is held back by the test (Python's _run_in_thread patch), and reads a preview the
// test gives unless a test reads real files.
#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <numbers>
#include <string>
#include <thread>
#include <vector>

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDropEvent>
#include <QLabel>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QStackedWidget>
#include <QTest>
#include <QTextBlock>
#include <QTextEdit>
#include <QTimer>
#include <QVBoxLayout>

#include "app_theme.h"
#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "audio/wav_writer.h"
#include "base/env.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/paths.h"
#include "base/testing/fake_clock.h"
#include "base/testing/manual_executor.h"
#include "base/utf8.h"
#include "http/testing/fake_http_client.h"
#include "settings/testing/memory_store.h"
#include "speech/dictation_language.h"
#include "testkit/testkit.h"
#include "ui_record/batch_relation_dialog.h"
#include "ui_record/drop_zone.h"
#include "ui_record/engine_selection.h"
#include "ui_record/file_info_card.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/quick_record_tab.h"
#include "ui_record/record_feature.h"
#include "ui_record/relation_picker.h"
#include "ui_record/stats_display.h"
#include "ui_record/transcript_pane.h"
#include "ui_record/transcript_viewer_dialog.h"
#include "ui_record/transcription_progress.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_queue.h"
#include "ui_shell/main_window.h"
#include "ui_shell/shell_feature.h"
#include "ui_widgets/animated_progress_bar.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/engine_field.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/no_wheel.h"
#include "ui_widgets/speech_server_controls.h"
#include "ui_widgets/tabbed_content.h"
#include "ui_widgets/wrapped_label.h"
#include "uimath/window_geometry.h"
#include "upload_widgets_support.h"

namespace {

using Catch::Approx;
using fv::Json;
using fv::UploadFileTab;
using fv::appcore::OverlayState;
using fv::audio::AudioFilePreview;
using fv::dictation::BatchRelation;
using fv::dictation::ProgressStage;

namespace fs = std::filesystem;

QString name(const QString& path) {
    fv::QueueItem item;
    item.path = path;
    return item.name();
}

// _preview_for: the sample preview, named after the path.
AudioFilePreview previewFor(const QString& path, double seconds = 60.0) {
    AudioFilePreview preview = fvtest::samplePreview();
    preview.file_path = path.toStdString();
    preview.file_name = name(path).toStdString();
    preview.duration_seconds = seconds;
    return preview;
}

// A tab over settings in memory whose preview worker runs inline (or is held back while
// `deferred`) and reads `reader`. Files the test writes live in a folder of its own.
class UploadRig {
public:
    explicit UploadRig(const Json& saved = Json::object()) : settings(saved) {
        tab = std::make_unique<UploadFileTab>(settings, fv::speechEngineCatalog());
        tab->setPreviewRunner([this](std::function<void()> worker) {
            if (deferred) {
                pending.push_back(std::move(worker));
            } else {
                worker();
            }
        });
        tab->setPreviewReader([this](const fs::path& path, bool engineSplits) {
            return reader(QString::fromStdString(fv::base_utf8::path_to_utf8(path)), engineSplits);
        });
        tab->setFileBrowser([](QWidget*) { return QStringList(); });
    }
    ~UploadRig() {
        tab.reset();
        QApplication::processEvents();
    }
    UploadRig(const UploadRig&) = delete;
    UploadRig& operator=(const UploadRig&) = delete;

    // A 64-byte file in the rig's folder.
    QString file(const char* fileName) {
        const fs::path path = root.path() / fileName;
        std::ofstream(path, std::ios::binary) << std::string(64, '\0');
        return QString::fromStdString(fv::base_utf8::path_to_utf8(path));
    }
    QStringList files(std::initializer_list<const char*> names) {
        QStringList paths;
        for (const char* fileName : names) {
            paths.append(file(fileName));
        }
        return paths;
    }
    // Queues paths and delivers the previews the workers posted.
    void add(const QStringList& paths, int skipped = 0) {
        tab->addFiles(paths, skipped);
        deliver();
    }
    // Runs the held-back workers, then delivers.
    void runPending() {
        auto workers = std::move(pending);
        pending.clear();
        for (auto& worker : workers) {
            worker();
        }
        deliver();
    }
    static void deliver() { QApplication::processEvents(); }

    UploadFileTab& operator*() const { return *tab; }
    UploadFileTab* operator->() const { return tab.get(); }
    fv::FileInfoCard& card() const { return *tab->fileInfoCard(); }
    fv::UploadQueueWidget& queue() const { return *tab->fileInfoCard()->queue(); }
    fv::TranscriptionProgressPanel& panel() const { return *tab->fileInfoCard()->progressPanel(); }

    fv::testkit::TempDataRoot root;
    fv::MemorySettingsStore settings;
    std::function<AudioFilePreview(const QString&, bool)> reader = [](const QString& path, bool) {
        return previewFor(path);
    };
    bool deferred = false;
    std::vector<std::function<void()>> pending;
    std::unique_ptr<UploadFileTab> tab;
};

// _tab_with_file: one 64-byte recording.wav, read.
struct OneFile : UploadRig {
    OneFile() {
        path = file("recording.wav");
        add({path});
    }
    QString path;
};

// _tab_with_files: a.wav and b.wav (or the names given), read.
struct Files : UploadRig {
    explicit Files(std::initializer_list<const char*> names = {"a.wav", "b.wav"}) {
        paths = files(names);
        add(paths);
    }
    QStringList paths;
};

std::vector<QString> rowStates(const fv::UploadQueueWidget& queue) {
    std::vector<QString> states;
    for (const fv::UploadQueueRow* row : queue.rows()) {
        states.push_back(row->state());
    }
    return states;
}

std::vector<QString> strings(std::initializer_list<const char*> values) {
    std::vector<QString> made;
    for (const char* value : values) {
        made.push_back(QString::fromUtf8(value));
    }
    return made;
}

void choose(QComboBox* combo, const QString& data) {
    combo->setCurrentIndex(combo->findData(data));
}

} // namespace

// ---- TestUploadFileTab ----

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_init_structure
TEST_CASE("init structure", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    CHECK(rig->scrollArea()->widgetResizable());
    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->fileInfoCard()->isHidden());
    CHECK(rig->findChild<QLabel*>(QStringLiteral("statusLabel")) == nullptr);
    CHECK(rig->dropZone()->noticeLabel()->isHidden());
    CHECK(rig->isTranscriptionCollapsed());
    // Map a08 A08-58: the Upload tab's own knobs.
    CHECK(rig->transcriptText()->placeholderText() ==
          QStringLiteral("Transcription will appear here...\nUpload an audio file to begin."));
    CHECK(rig->findChild<QWidget*>(QStringLiteral("uploadFileContent")) != nullptr);
    CHECK(rig->resolvedLabel()->text() == QStringLiteral("Ready to transcribe"));
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_file_selected_flow
TEST_CASE("file selected flow", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.reader = [](const QString&, bool) {
        AudioFilePreview preview;
        preview.file_path = "recording.wav";
        preview.file_name = "recording.wav";
        preview.file_size_mb = 0.002;
        preview.duration_seconds = 5.0;
        preview.sample_rate = 16000;
        preview.channels = 1;
        return preview;
    };
    rig.add({QStringLiteral("recording.wav")});

    CHECK(rig->dropZone()->isHidden());
    CHECK_FALSE(rig->fileInfoCard()->isHidden());
    CHECK(rig.card().transcribeButton()->isEnabled());
    CHECK(rig->audioPath() == QStringLiteral("recording.wav"));
    CHECK(rig.card().mode() == fv::FileInfoCard::Mode::Single);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_single_file_card_appears_before_its_preview_lands
TEST_CASE("single file card appears before its preview lands", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.deferred = true;
    rig.add({QStringLiteral("recording.wav")});

    CHECK_FALSE(rig->fileInfoCard()->isHidden());
    CHECK(rig.card().filenameLabel()->text() == QStringLiteral("recording.wav"));
    CHECK_FALSE(rig.card().transcribeButton()->isEnabled());

    rig.runPending();
    CHECK(rig.card().transcribeButton()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_unreadable_single_file_returns_to_the_drop_zone_with_the_reason
TEST_CASE("unreadable single file returns to the drop zone with the reason",
          "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.reader = [](const QString&, bool) -> AudioFilePreview {
        throw fv::audio::UnreadableAudio("This file is empty.");
    };
    rig.add({QStringLiteral("somewhere/broken.wav")});

    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->fileInfoCard()->isHidden());
    CHECK_FALSE(rig->dropZone()->noticeLabel()->isHidden());
    CHECK(rig->dropZone()->noticeLabel()->text() ==
          QStringLiteral("broken.wav: This file is empty."));
    CHECK(rig->audioPath().isEmpty());

    rig.reader = [](const QString& path, bool) {
        return previewFor(path);
    };
    rig.add({QStringLiteral("good.wav")});
    CHECK(rig->dropZone()->noticeLabel()->isHidden());
}

// A file gone before its preview, and a failure of another kind: the worker's other two
// sentences (_start_previews).
TEST_CASE("a preview worker says why a file could not be read", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.reader = [](const QString&, bool) -> AudioFilePreview {
        throw fv::audio::AudioFileNotFound("Audio file not found: gone.wav");
    };
    rig.add({QStringLiteral("gone.wav")});
    CHECK(rig->dropZone()->noticeLabel()->text() ==
          QStringLiteral("gone.wav: This file no longer exists."));

    rig.reader = [](const QString&, bool) -> AudioFilePreview {
        throw std::runtime_error("decoder exploded");
    };
    rig.add({QStringLiteral("odd.wav")});
    CHECK(rig->dropZone()->noticeLabel()->text() ==
          QStringLiteral("odd.wav: %1 couldn't read this file (RuntimeError).")
              .arg(QString::fromUtf8(fv::identity().product)));
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_clear_file_flow
TEST_CASE("clear file flow", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->clearFile();
    CHECK(rig->audioPath().isEmpty());
    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->fileInfoCard()->isHidden());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_transcript_display_and_copy_signal
TEST_CASE("transcript display and copy signal", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    std::vector<QString> copied;
    QObject::connect(rig.tab.get(), &UploadFileTab::copyRequested,
                     [&](const QString& text) { copied.push_back(text); });

    rig->setTranscript(QStringLiteral("Hello world transcription"));
    CHECK(rig->transcriptText()->toPlainText() == QStringLiteral("Hello world transcription"));
    CHECK(rig.card().copyButton()->isActive());

    rig.card().copyButton()->click();
    CHECK(copied == strings({"Hello world transcription"}));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcript_renders_markdown_but_copies_the_source
TEST_CASE("transcript renders markdown but copies the source", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    std::vector<QString> copied;
    QObject::connect(rig.tab.get(), &UploadFileTab::copyRequested,
                     [&](const QString& text) { copied.push_back(text); });

    rig->setTranscript(QStringLiteral("## a.mp3\n\nHello **world**"));
    const QTextBlock first = rig->transcriptText()->document()->begin();
    CHECK(first.blockFormat().headingLevel() == 2);
    CHECK(rig->transcriptText()->toPlainText() == QStringLiteral("a.mp3\nHello world"));

    rig.card().copyButton()->click();
    CHECK(copied == strings({"## a.mp3\n\nHello **world**"}));
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_raw_view_renders_the_same_way
TEST_CASE("raw view renders the same way", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setTranscript(QStringLiteral("## a.mp3\n\nclean"), QStringLiteral("## a.mp3\n\nraw um"));
    rig->rawButton()->click();
    CHECK(rig->transcriptText()->toPlainText() == QStringLiteral("a.mp3\nraw um"));
    CHECK(rig->transcriptText()->document()->begin().blockFormat().headingLevel() == 2);
    CHECK(rig->shownTranscript() == QStringLiteral("## a.mp3\n\nraw um"));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_expand_button_sits_in_the_pane_corner_only_while_there_is_a_transcript
TEST_CASE("expand button sits in the pane corner only while there is a transcript",
          "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->resize(700, 600);
    rig->show();
    CHECK(rig->expandButton()->isHidden());

    rig->setTranscript(QStringLiteral("Hello"));
    CHECK_FALSE(rig->expandButton()->isHidden());
    fv::TranscriptPane* pane = rig->transcriptPane();
    CHECK(rig->expandButton()->parent() == pane);
    CHECK(rig->expandButton()->geometry().right() ==
          pane->width() - 1 - fv::TranscriptPane::kCornerInsetX);
    CHECK(rig->expandButton()->y() == fv::TranscriptPane::kCornerInsetY);

    rig->setTranscript(QStringLiteral("Error: Transcription canceled"));
    CHECK(rig->expandButton()->isHidden());
    rig->setTranscript(QStringLiteral("Hello again"));
    rig->clearTranscription();
    CHECK(rig->expandButton()->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_viewer_opens_on_the_current_transcript_and_follows_changes
TEST_CASE("viewer opens on the current transcript and follows changes",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->setTranscript(QStringLiteral("## recording.wav\n\nfirst"), QStringLiteral("first raw"));
    rig->openTranscriptViewer();
    fv::TranscriptViewerDialog* viewer = rig->viewer();
    REQUIRE(viewer != nullptr);
    CHECK(viewer->isVisible());
    CHECK(viewer->shownText() == QStringLiteral("## recording.wav\n\nfirst"));
    CHECK(viewer->titleLabel()->text() == QStringLiteral("recording.wav"));
    CHECK_FALSE(viewer->versionToggle()->isHidden());

    rig->setTranscript(QStringLiteral("second"));
    CHECK(viewer->shownText() == QStringLiteral("second"));
    CHECK(viewer->versionToggle()->isHidden());

    rig->openTranscriptViewer();
    CHECK(rig->viewer() == viewer);

    rig->clearTranscription();
    CHECK(viewer->shownText().isEmpty());
    viewer->hide();
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_viewer_receives_completed_batch_structure
TEST_CASE("viewer receives completed batch structure", "[test_upload_file_tab][widget]") {
    Files rig;
    fv::dictation::BatchUploadRequest request;
    for (const QString& path : rig.paths) {
        request.items.push_back({path.toStdString(), std::nullopt});
    }
    request.relation = BatchRelation::Separate;
    fv::dictation::BatchResult result;
    result.request = request;
    int position = 1;
    for (const auto& item : request.items) {
        fv::dictation::BatchItemResult done;
        done.item = item;
        done.text = "Text " + std::to_string(position++);
        result.items.push_back(done);
    }
    rig->setTranscript(QStringLiteral("## a.wav\n\nText 1\n\n## b.wav\n\nText 2"));
    rig->setBatchResult(result);
    rig->openTranscriptViewer();

    fv::TranscriptViewerDialog* viewer = rig->viewer();
    std::vector<QString> tabs;
    for (int index = 0; index < viewer->pageTabs()->count(); ++index) {
        tabs.push_back(viewer->pageTabs()->tabText(index));
    }
    CHECK(tabs == strings({"Overview", "Trans. 1", "Trans. 2"}));
    // Map a08 A08-43: two or more files are titled by their count.
    CHECK(viewer->titleLabel()->text() == QStringLiteral("2 files"));
    CHECK(viewer->windowTitle() == QStringLiteral(u"Transcript — 2 files"));
    viewer->pageTabs()->setCurrentIndex(2);
    CHECK(viewer->shownText() == QStringLiteral("## b.wav\n\nText 2"));
    viewer->hide();
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_viewer_copy_goes_through_the_tab
TEST_CASE("viewer copy goes through the tab", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    std::vector<QString> copied;
    QObject::connect(rig.tab.get(), &UploadFileTab::copyRequested,
                     [&](const QString& text) { copied.push_back(text); });
    rig->setTranscript(QStringLiteral("body text"));
    rig->openTranscriptViewer();
    rig->viewer()->copyButton()->click();
    CHECK(copied == strings({"body text"}));
    rig->viewer()->hide();
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_two_files_show_the_queue_with_separate_recordings_selected
TEST_CASE("two files show the queue with separate recordings selected",
          "[test_upload_file_tab][widget]") {
    Files rig;
    fv::FileInfoCard& card = rig.card();

    CHECK(card.mode() == fv::FileInfoCard::Mode::Queue);
    CHECK(card.headerRow()->isHidden());
    CHECK_FALSE(card.queue()->isHidden());
    CHECK(card.queue()->picker()->relation() == BatchRelation::Separate);
    std::vector<QString> names;
    for (const fv::UploadQueueRow* row : card.queue()->rows()) {
        names.push_back(row->nameLabel()->text());
    }
    CHECK(names == strings({"a.wav", "b.wav"}));
    CHECK(card.queue()->titleLabel()->text() == QStringLiteral("2 files"));
    CHECK(card.queue()->noteLabel()->isHidden());
    CHECK(card.copyButton()->text() == QStringLiteral("Copy all"));
    CHECK(card.transcribeButton()->isEnabled());
    CHECK(rig->audioPath().isEmpty());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_two_files_emit_one_batch_request_in_order
TEST_CASE("two files emit one batch request in order", "[test_upload_file_tab][widget]") {
    Files rig;
    std::vector<QString> single;
    std::vector<fv::dictation::BatchUploadRequest> batch;
    QObject::connect(rig.tab.get(), &UploadFileTab::uploadRequested,
                     [&](const QString& path, double) { single.push_back(path); });
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { batch.push_back(request); });

    rig->transcribe();

    CHECK(single.empty());
    REQUIRE(batch.size() == 1);
    const auto& request = batch[0];
    REQUIRE(request.items.size() == 2);
    CHECK(QString::fromStdString(request.items[0].audio_path) == rig.paths[0]);
    CHECK(QString::fromStdString(request.items[1].audio_path) == rig.paths[1]);
    for (const auto& item : request.items) {
        CHECK(item.duration_seconds == 60.0);
    }
    CHECK(request.relation == BatchRelation::Separate);
    CHECK_FALSE(request.combine());
    CHECK(rig->isTranscribing());
    CHECK(rig.panel().isBatch());
    CHECK(rowStates(rig.queue()) == strings({"pending", "pending"}));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcribe_waits_until_every_preview_has_arrived
TEST_CASE("transcribe waits until every preview has arrived", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.deferred = true;
    rig.add(rig.files({"a.wav", "b.wav"}));

    CHECK_FALSE(rig.card().transcribeButton()->isEnabled());
    CHECK(rowStates(rig.queue()) == strings({"reading", "reading"}));

    rig.runPending();
    CHECK(rig.card().transcribeButton()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_previews_arriving_out_of_order_land_on_the_right_row
TEST_CASE("previews arriving out of order land on the right row",
          "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.deferred = true;
    const QStringList paths = rig.files({"a.wav", "b.wav"});
    rig.add(paths);

    const std::uint64_t token = rig->previewToken();
    rig->onPreviewReady({token, paths[1], previewFor(paths[1], 120.0), {}});
    rig->onPreviewReady({token, paths[0], previewFor(paths[0], 60.0), {}});

    fv::UploadQueueWidget& queue = rig.queue();
    CHECK(queue.rowFor(paths[0])->durationChip()->text() == QStringLiteral("1m 0s"));
    CHECK(queue.rowFor(paths[1])->durationChip()->text() == QStringLiteral("2m 0s"));
    std::vector<QString> names;
    for (const fv::UploadQueueRow* row : queue.rows()) {
        names.push_back(row->nameLabel()->text());
    }
    CHECK(names == strings({"a.wav", "b.wav"}));
    // Another tab's preview is not this tab's.
    rig->onPreviewReady({token + 1000, paths[0], previewFor(paths[0], 30.0), {}});
    CHECK(queue.rowFor(paths[0])->durationChip()->text() == QStringLiteral("1m 0s"));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_failed_preview_marks_its_row_and_removing_it_unblocks_transcribe
TEST_CASE("a failed preview marks its row and removing it unblocks transcribe",
          "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.deferred = true;
    const QStringList paths = rig.files({"a.wav", "b.wav"});
    rig.add(paths);

    const std::uint64_t token = rig->previewToken();
    rig->onPreviewReady({token, paths[0], previewFor(paths[0]), {}});
    rig->onPreviewReady(
        {token, paths[1], std::nullopt, QStringLiteral("This doesn't look like an audio file.")});

    fv::UploadQueueWidget& queue = rig.queue();
    CHECK(queue.rowFor(paths[1])->state() == QStringLiteral("failed"));
    CHECK_FALSE(rig.card().transcribeButton()->isEnabled());
    CHECK(queue.noteLabel()->text().contains(QStringLiteral("1 could not be read")));
    CHECK(queue.noteLabel()->property("tone").toString() == QStringLiteral("warn"));

    rig->removeFile(paths[1]);

    CHECK(rig.card().mode() == fv::FileInfoCard::Mode::Single);
    CHECK(rig->audioPath() == paths[0]);
    CHECK(rig.card().transcribeButton()->isEnabled());
    CHECK(queue.noteLabel()->isHidden());
}

// Map a08 A08-63: the queue note's full wording, "it" for one file and "them" for more.
TEST_CASE("the unreadable note asks to remove it or them", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig.deferred = true;
    const QStringList paths = rig.files({"a.wav", "b.wav", "c.wav"});
    rig.add(paths);
    const std::uint64_t token = rig->previewToken();
    rig->onPreviewReady({token, paths[0], previewFor(paths[0]), {}});
    rig->onPreviewReady({token, paths[1], std::nullopt, QStringLiteral("Unreadable.")});
    fv::UploadQueueWidget& queue = rig.queue();
    CHECK(queue.noteLabel()->toolTip().contains(
        QStringLiteral(u"1 could not be read — remove it to continue")));
    rig->onPreviewReady({token, paths[2], std::nullopt, QStringLiteral("Unreadable.")});
    CHECK(queue.noteLabel()->toolTip().contains(
        QStringLiteral(u"2 could not be read — remove them to continue")));
    CHECK(queue.noteLabel()->property("tone").toString() == QStringLiteral("warn"));
    CHECK_FALSE(rig.card().transcribeButton()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_tab_deleted_mid_preview_is_not_held_by_the_worker
TEST_CASE("a tab deleted mid preview is not held by the worker", "[test_upload_file_tab][widget]") {
    // The worker used to capture the tab: it kept a deleted tab alive, then deleted it off the
    // GUI thread or posted to it from its own thread, which can crash the process.
    fv::MemorySettingsStore settings;
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool open = false;
    std::vector<std::thread> workers;
    {
        auto* tab = new UploadFileTab(settings, fv::speechEngineCatalog());
        tab->setPreviewReader([&](const fs::path& path, bool) {
            std::unique_lock lock(gateMutex);
            gateChanged.wait_for(lock, std::chrono::seconds(5), [&] { return open; });
            return previewFor(QString::fromStdString(fv::base_utf8::path_to_utf8(path)));
        });
        tab->setPreviewRunner(
            [&](std::function<void()> worker) { workers.emplace_back(std::move(worker)); });
        tab->addFiles({QStringLiteral("a.wav")});
        QPointer<UploadFileTab> ref(tab);
        tab->deleteLater();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QApplication::processEvents();
        CHECK(ref == nullptr);
    }
    {
        std::lock_guard lock(gateMutex);
        open = true;
    }
    gateChanged.notify_all();
    for (std::thread& worker : workers) {
        worker.join();
    }
    // The posted result reaches the relay, which no tab hears any more.
    QApplication::processEvents();
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_reorder_buttons_swap_rows_and_the_request_order
TEST_CASE("reorder buttons swap rows and the request order", "[test_upload_file_tab][widget]") {
    Files rig({"a.wav", "b.wav", "c.wav"});
    const QStringList& paths = rig.paths;
    fv::UploadQueueWidget& queue = rig.queue();
    CHECK_FALSE(queue.rows().front()->upButton()->isEnabled());
    CHECK_FALSE(queue.rows().back()->downButton()->isEnabled());

    queue.rowFor(paths[0])->downButton()->click();
    CHECK(rig->queuedPaths() == QStringList{paths[1], paths[0], paths[2]});

    queue.rowFor(paths[2])->upButton()->click();
    CHECK(rig->queuedPaths() == QStringList{paths[1], paths[2], paths[0]});

    std::vector<fv::dictation::BatchUploadRequest> requests;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { requests.push_back(request); });
    rig->transcribe();
    REQUIRE(requests.size() == 1);
    QStringList order;
    for (const auto& item : requests[0].items) {
        order.append(QString::fromStdString(item.audio_path));
    }
    CHECK(order == QStringList{paths[1], paths[2], paths[0]});
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_removing_down_to_one_file_collapses_back_to_the_single_card
TEST_CASE("removing down to one file collapses back to the single card",
          "[test_upload_file_tab][widget]") {
    Files rig;
    rig->removeFile(rig.paths[1]);

    fv::FileInfoCard& card = rig.card();
    CHECK(card.mode() == fv::FileInfoCard::Mode::Single);
    CHECK_FALSE(card.headerRow()->isHidden());
    CHECK(card.filenameLabel()->text() == QStringLiteral("a.wav"));
    CHECK(card.copyButton()->text() == QStringLiteral("Copy"));
    CHECK(rig->audioPath() == rig.paths[0]);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_removing_the_last_file_returns_to_the_drop_zone
TEST_CASE("removing the last file returns to the drop zone", "[test_upload_file_tab][widget]") {
    Files rig;
    rig->removeFile(rig.paths[0]);
    rig->removeFile(rig.paths[1]);

    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->fileInfoCard()->isHidden());
    CHECK(rig->queuedPaths().isEmpty());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_dropping_on_the_card_appends_to_the_queue
TEST_CASE("dropping on the card appends to the queue", "[test_upload_file_tab][widget]") {
    Files rig;
    const QString extra = rig.file("c.wav");
    const auto mime = fvtest::mimeOfPaths(
        {extra,
         QString::fromStdString(fv::base_utf8::path_to_utf8(rig.root.path() / "notes.txt"))});
    QDropEvent event(QPointF(10, 10), Qt::CopyAction, mime.get(), Qt::NoButton, Qt::NoModifier);
    fvtest::deliver(rig.card(), &event);
    rig.deliver();

    CHECK(event.isAccepted());
    QStringList names;
    for (const QString& path : rig->queuedPaths()) {
        names.append(name(path));
    }
    CHECK(names ==
          QStringList{QStringLiteral("a.wav"), QStringLiteral("b.wav"), QStringLiteral("c.wav")});
    fv::ElidingLabel* note = rig.queue().noteLabel();
    CHECK_FALSE(note->isHidden());
    CHECK(note->text() == QStringLiteral(u"·  1 skipped (not audio)"));
    CHECK(note->property("tone").toString().isEmpty());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_duplicate_paths_are_added_once
TEST_CASE("duplicate paths are added once", "[test_upload_file_tab][widget]") {
    Files rig;
    rig.add({rig.paths[0]});

    CHECK(rig->queuedPaths() == rig.paths);
    CHECK(rig.queue().noteLabel()->text().contains(QStringLiteral("1 already queued")));
}

// The queue key is normcase(abspath(path)): on Windows a path in another case or with the other
// slash is the same file.
TEST_CASE("a path in another case or spelling is the same queued file",
          "[test_upload_file_tab][widget]") {
#ifdef _WIN32
    Files rig;
    QString other = rig.paths[0].toUpper();
    other.replace(QLatin1Char('/'), QLatin1Char('\\'));
    rig.add({other});
    CHECK(rig->queuedPaths() == rig.paths);
#else
    SKIP("normcase folds case only on Windows");
#endif
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_drop_with_no_audio_leaves_the_drop_zone_up
TEST_CASE("a drop with no audio leaves the drop zone up", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->addFiles({}, 3);

    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->dropZone()->noticeLabel()->text() == fv::noAudioDroppedMessage());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_dropping_only_non_audio_on_the_zone_says_so
TEST_CASE("dropping only non audio on the zone says so", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    const auto mime = fvtest::mimeOfPaths(
        {QString::fromStdString(fv::base_utf8::path_to_utf8(rig.root.path() / "notes.txt"))});
    QDropEvent event(QPointF(10, 10), Qt::CopyAction, mime.get(), Qt::LeftButton, Qt::NoModifier);
    fvtest::deliver(*rig->dropZone(), &event);

    CHECK_FALSE(rig->dropZone()->noticeLabel()->isHidden());
    CHECK(rig->dropZone()->noticeLabel()->text() == fv::noAudioDroppedMessage());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_adding_files_is_refused_while_a_job_runs
TEST_CASE("adding files is refused while a job runs", "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();

    rig.add({rig.file("c.wav")});

    CHECK(rig->queuedPaths() == rig.paths);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_sequential_preset_implies_combine_and_is_remembered
TEST_CASE("sequential preset implies combine and is remembered", "[test_upload_file_tab][widget]") {
    Files rig;
    fv::RelationPicker* picker = rig.queue().picker();
    std::vector<fv::dictation::BatchUploadRequest> requests;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { requests.push_back(request); });

    choose(picker->combo(), QStringLiteral("sequential"));
    rig->transcribe();

    CHECK(rig->relation() == BatchRelation::Sequential);
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].combine());
    CHECK(rig.settings.snapshot()->at("transcript_batch_relation") == "sequential");
    CHECK(picker->hint()->text().contains(QStringLiteral("Stitched")));
}

// Map a08 A08-64: a relation that cannot be saved is still the tab's choice.
TEST_CASE("a relation that cannot be saved is still used", "[test_upload_file_tab][widget]") {
    Files rig;
    std::vector<fv::dictation::BatchUploadRequest> requests;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { requests.push_back(request); });
    rig.settings.file().fail_next_replace("disk full");
    choose(rig.queue().picker()->combo(), QStringLiteral("sequential"));
    CHECK(rig->relation() == BatchRelation::Sequential);
    CHECK_FALSE(rig.settings.snapshot()->contains("transcript_batch_relation"));
    rig->transcribe();
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].relation == BatchRelation::Sequential);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_custom_relation_opens_the_dialog_and_stores_the_description
TEST_CASE("custom relation opens the dialog and stores the description",
          "[test_upload_file_tab][widget]") {
    Files rig;
    fv::RelationPicker* picker = rig.queue().picker();
    std::vector<fv::dictation::BatchUploadRequest> requests;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { requests.push_back(request); });
    QStringList listed;
    rig->setRelationDialogRunner([&](fv::BatchRelationDialog& dialog) {
        dialog.instructionsEdit()->setPlainText(QStringLiteral("Two halves of one call."));
        dialog.combineCheck()->setChecked(true);
        return true;
    });

    choose(picker->combo(), QStringLiteral("custom"));
    rig->transcribe();

    CHECK(rig->relation() == BatchRelation::Custom);
    REQUIRE(requests.size() == 1);
    const auto& request = requests[0];
    CHECK(request.custom_instructions == "Two halves of one call.");
    CHECK(request.custom_combine);
    CHECK(request.combine());
    const Json& saved = *rig.settings.snapshot();
    CHECK(saved.at("transcript_batch_relation") == "custom");
    CHECK(saved.at("transcript_batch_custom_instructions") == "Two halves of one call.");
    CHECK(saved.at("transcript_batch_custom_combine") == true);
    CHECK(picker->hint()->text().startsWith(QStringLiteral("One combined transcript")));
    CHECK_FALSE(picker->editButton()->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_rejecting_the_custom_dialog_reverts_the_combo
TEST_CASE("rejecting the custom dialog reverts the combo", "[test_upload_file_tab][widget]") {
    Files rig;
    fv::RelationPicker* picker = rig.queue().picker();
    rig->setRelationDialogRunner([](fv::BatchRelationDialog&) { return false; });

    choose(picker->combo(), QStringLiteral("custom"));

    CHECK(rig->relation() == BatchRelation::Separate);
    CHECK(picker->relation() == BatchRelation::Separate);
    CHECK_FALSE(rig.settings.snapshot()->contains("transcript_batch_relation"));
    CHECK_FALSE(rig.settings.snapshot()->contains("transcript_batch_custom_instructions"));
    CHECK(picker->editButton()->isHidden());
}

// A saved preset is the tab's from the start (resolve_transcript_batch_*).
TEST_CASE("the saved relation and description are the tab's from the start",
          "[test_upload_file_tab][widget]") {
    UploadRig saved(Json{{"transcript_batch_relation", "custom"},
                         {"transcript_batch_custom_instructions", "Three parts of a lecture."},
                         {"transcript_batch_custom_combine", true}});
    saved.add(saved.files({"a.wav", "b.wav"}));
    CHECK(saved->relation() == BatchRelation::Custom);
    CHECK(saved->customInstructions() == QStringLiteral("Three parts of a lecture."));
    CHECK(saved->customCombine());
    CHECK(saved.queue().picker()->relation() == BatchRelation::Custom);
    CHECK(saved.queue().picker()->hint()->text().startsWith(QStringLiteral("One combined")));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_batch_progress_marks_rows_and_the_panel
TEST_CASE("batch progress marks rows and the panel", "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();

    rig->setBatchProgress(1, 2, QStringLiteral("a.wav"));
    CHECK(rig.queue().rows()[0]->state() == QStringLiteral("active"));
    CHECK(rig.panel().batchLabel()->text() == QStringLiteral(u"File 1 of 2  ·  a.wav"));

    rig->setBatchItemFinished(1, true);
    rig->setBatchProgress(2, 2, QStringLiteral("b.wav"));
    CHECK(rowStates(rig.queue()) == strings({"done", "active"}));
    CHECK(rig.panel().bar()->fraction() == Approx(0.5));

    rig->setBatchItemFinished(2, false);
    CHECK(rig.queue().rows()[1]->state() == QStringLiteral("failed"));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_finished_files_get_their_own_copy_button
TEST_CASE("finished files get their own copy button", "[test_upload_file_tab][widget]") {
    Files rig;
    std::vector<QString> copied;
    QObject::connect(rig.tab.get(), &UploadFileTab::copyRequested,
                     [&](const QString& text) { copied.push_back(text); });
    fv::UploadQueueWidget& queue = rig.queue();
    rig->transcribe();
    for (const fv::UploadQueueRow* row : queue.rows()) {
        CHECK(row->copyButton()->isHidden());
    }

    rig->setBatchItemFinished(1, true, QStringLiteral("Hello from a"));
    CHECK_FALSE(queue.rowFor(rig.paths[0])->copyButton()->isHidden());
    CHECK(queue.rowFor(rig.paths[1])->copyButton()->isHidden());

    queue.rowFor(rig.paths[0])->copyButton()->click();
    CHECK(copied == strings({"Hello from a"}));

    rig->setBatchItemFinished(2, false, QString());
    CHECK(queue.rowFor(rig.paths[1])->copyButton()->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_row_copy_survives_the_job_ending_and_clears_when_the_next_starts
TEST_CASE("row copy survives the job ending and clears when the next starts",
          "[test_upload_file_tab][widget]") {
    Files rig;
    fv::UploadQueueWidget& queue = rig.queue();
    rig->transcribe();
    rig->setBatchItemFinished(1, true, QStringLiteral("Hello from a"));
    rig->setTranscript(QStringLiteral("## a.wav\n\nHello from a\n\n## b.wav\n\nThere"));
    CHECK_FALSE(queue.rowFor(rig.paths[0])->copyButton()->isHidden());
    CHECK_FALSE(queue.rowFor(rig.paths[0])->removeButton()->isHidden());

    rig->transcribe();
    CHECK(queue.rowFor(rig.paths[0])->copyButton()->isHidden());
    // A combined job reports no per-file text, so the row stays plain.
    rig->setBatchItemFinished(1, true, QString());
    CHECK(queue.rowFor(rig.paths[0])->copyButton()->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_missing_files_at_transcribe_time_are_noted_in_the_queue_header
TEST_CASE("missing files at transcribe time are noted in the queue header",
          "[test_upload_file_tab][widget]") {
    Files rig({"a.wav", "b.wav", "c.wav"});
    fs::remove(fv::base_utf8::path_from_utf8(rig.paths[1].toStdString()));
    std::vector<fv::dictation::BatchUploadRequest> requests;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadFilesRequested,
        [&](const fv::dictation::BatchUploadRequest& request) { requests.push_back(request); });

    rig->transcribe();

    CHECK(requests.empty());
    CHECK(rig->queuedPaths() == QStringList{rig.paths[0], rig.paths[2]});
    fv::ElidingLabel* note = rig.queue().noteLabel();
    CHECK(note->text().contains(QStringLiteral("1 file no longer exists and was removed")));

    rig->transcribe();
    CHECK(requests.size() == 1);
    CHECK(note->isHidden());
}

// The wording for more than one missing file.
TEST_CASE("several missing files are counted in the plural", "[test_upload_file_tab][widget]") {
    Files rig({"a.wav", "b.wav", "c.wav", "d.wav"});
    rig->setFileExists(
        [&](const QString& path) { return path == rig.paths[0] || path == rig.paths[3]; });
    rig->transcribe();
    CHECK(rig->queuedPaths() == QStringList{rig.paths[0], rig.paths[3]});
    CHECK(rig.queue().noteLabel()->text().contains(
        QStringLiteral("2 files no longer exist and were removed")));
}

// The single-file path checks the file too.
TEST_CASE("a single file gone at transcribe time returns to the drop zone",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    std::vector<QString> requested;
    QObject::connect(rig.tab.get(), &UploadFileTab::uploadRequested,
                     [&](const QString& path, double) { requested.push_back(path); });
    fs::remove(fv::base_utf8::path_from_utf8(rig.path.toStdString()));

    rig->transcribe();

    CHECK(requested.empty());
    CHECK_FALSE(rig->dropZone()->isHidden());
    CHECK(rig->dropZone()->noticeLabel()->text() ==
          QStringLiteral(u"That file no longer exists — drop it again"));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_fixed_raw_switch_lives_inside_the_transcript_pane
TEST_CASE("fixed raw switch lives inside the transcript pane", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    CHECK(rig->versionToggle()->parent() == rig->transcriptPane());
    CHECK(rig->transcriptText()->parent() == rig->transcriptPane());
    CHECK(rig->versionToggle()->isHidden());

    rig->setTranscript(QStringLiteral("Fixed text"), QStringLiteral("raw text"));
    CHECK_FALSE(rig->versionToggle()->isHidden());
    CHECK(rig->transcriptText()->property("headed").toBool());

    rig->setTranscript(QStringLiteral("Only text"));
    CHECK(rig->versionToggle()->isHidden());
    CHECK_FALSE(rig->transcriptText()->property("headed").toBool());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_stop_emits_cancel_requested_and_keeps_the_tab_locked
TEST_CASE("stop emits cancel requested and keeps the tab locked",
          "[test_upload_file_tab][widget]") {
    Files rig;
    int canceled = 0;
    QObject::connect(rig.tab.get(), &UploadFileTab::cancelRequested, [&] { ++canceled; });
    rig->transcribe();

    rig.panel().stopButton()->click();

    CHECK(canceled == 1);
    CHECK(rig->isTranscribing());
    CHECK_FALSE(rig.panel().stopButton()->isEnabled());
    CHECK(rig.panel().detailLabel()->text() == QStringLiteral(u"Canceling…"));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_canceled_job_stays_canceled_when_the_error_transcript_lands
TEST_CASE("a canceled job stays canceled when the error transcript lands",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->setProgressState(OverlayState::Canceling);
    rig->setTranscript(QStringLiteral("Error: Transcription canceled"));

    CHECK(rig.panel().stage() == ProgressStage::Canceled);
    CHECK_FALSE(rig->isTranscribing());
    CHECK(rig->modelCombo()->isEnabled());
    CHECK(rig.card().settleTimer()->isActive());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_stop_on_a_queue_marks_the_working_file_stopped
TEST_CASE("stop on a queue marks the working file stopped", "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();
    rig->setBatchProgress(1, 2, QStringLiteral("a.wav"));
    CHECK(rowStates(rig.queue()) == strings({"active", "pending"}));

    rig.panel().stopButton()->click();
    rig->setProgressState(OverlayState::Canceling);
    rig->showJobError(QStringLiteral("Error: Transcription canceled"));

    CHECK(rowStates(rig.queue()) == strings({"stopped", "pending"}));
    CHECK(rig.queue().rows()[0]->stateChip()->text() == QStringLiteral("Stopped"));
    CHECK_FALSE(rig->isTranscribing());
    CHECK(rig.card().transcribeButton()->isEnabled());

    rig->transcribe();
    CHECK(rowStates(rig.queue()) == strings({"pending", "pending"}));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_stop_after_the_first_file_keeps_done_and_marks_the_working_file_stopped
TEST_CASE("stop after the first file keeps done and marks the working file stopped",
          "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();
    rig->setBatchItemFinished(1, true, QStringLiteral("Hello from a"));
    rig->setBatchProgress(2, 2, QStringLiteral("b.wav"));

    rig->setProgressState(OverlayState::Canceling);
    rig->setTranscript(QStringLiteral("## a.wav\n\nHello from a"));

    CHECK(rowStates(rig.queue()) == strings({"done", "stopped"}));
    CHECK(rig.queue().rows()[1]->stateChip()->text() == QStringLiteral("Stopped"));
    CHECK_FALSE(rig->isTranscribing());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_failed_queue_job_marks_the_working_file_failed
TEST_CASE("a failed queue job marks the working file failed", "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();
    rig->setBatchProgress(1, 2, QStringLiteral("a.wav"));

    rig->showJobError(QStringLiteral("Error: The speech model isn't downloaded yet. Approve the "
                                     "download, then try again."));

    CHECK(rowStates(rig.queue()) == strings({"failed", "pending"}));
    CHECK_FALSE(rig->isTranscribing());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_none_while_a_queue_job_runs_clears_the_working_badge
TEST_CASE("none while a queue job runs clears the working badge",
          "[test_upload_file_tab][widget]") {
    Files rig;
    rig->transcribe();
    rig->setBatchProgress(1, 2, QStringLiteral("a.wav"));

    rig->setProgressState(OverlayState::None);

    CHECK(rowStates(rig.queue()) == strings({"failed", "pending"}));
    CHECK_FALSE(rig->isTranscribing());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_queue_controls_hide_while_a_job_runs
TEST_CASE("queue controls hide while a job runs", "[test_upload_file_tab][widget]") {
    Files rig;
    fv::UploadQueueWidget& queue = rig.queue();

    rig->transcribe();
    CHECK_FALSE(queue.addButton()->isEnabled());
    CHECK_FALSE(queue.picker()->combo()->isEnabled());
    CHECK(queue.rows()[0]->removeButton()->isHidden());

    rig->setTranscript(QStringLiteral("## a.wav\n\nHello\n\n## b.wav\n\nThere"));
    CHECK(queue.addButton()->isEnabled());
    CHECK(queue.picker()->combo()->isEnabled());
    CHECK_FALSE(queue.rows()[0]->removeButton()->isHidden());
    CHECK(rig.card().copyButton()->isActive());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcribe_locks_the_tab_and_shows_progress_in_the_card
TEST_CASE("transcribe locks the tab and shows progress in the card",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    std::vector<std::pair<QString, double>> requested;
    QObject::connect(
        rig.tab.get(), &UploadFileTab::uploadRequested,
        [&](const QString& path, double duration) { requested.emplace_back(path, duration); });

    rig->transcribe();

    REQUIRE(requested.size() == 1);
    CHECK(requested[0].first == rig.path);
    CHECK(requested[0].second == 60.0);
    CHECK(rig->isTranscribing());
    CHECK(rig.card().isProgressShown());
    CHECK(rig->findChild<QLabel*>(QStringLiteral("statusLabel")) == nullptr);
    CHECK_FALSE(rig->modelCombo()->isEnabled());
    CHECK_FALSE(rig->localEngine()->modelCombo()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_files_can_be_queued_during_a_recording_and_transcribed_afterward
TEST_CASE("files can be queued during a recording and transcribed afterward",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    const QString path = rig->queuedPaths().front();
    rig->setRecordingActive(true);
    rig->clearFile();
    Q_EMIT rig->dropZone()->filesSelected({path}, 0);
    rig.deliver();

    CHECK(rig->queuedPaths() == QStringList{path});
    CHECK_FALSE(rig.card().transcribeButton()->isEnabled());
    CHECK_FALSE(rig->modelCombo()->isEnabled());
    CHECK_FALSE(rig->isTranscribing());

    rig->setRecordingActive(false);
    CHECK(rig.card().transcribeButton()->isEnabled());
    rig.card().transcribeButton()->click();
    CHECK(rig->isTranscribing());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_overlay_states_and_status_text_land_in_the_panel
TEST_CASE("overlay states and status text land in the panel", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->setLargeFileStage(30.0);
    CHECK(rig.panel().stage() == ProgressStage::Splitting);

    rig->setProgressState(OverlayState::Transcribing);
    CHECK(rig.panel().stage() == ProgressStage::Transcribing);

    rig->setStatus(QStringLiteral("Transcribing chunk 1/3..."));
    CHECK(rig.panel().detailLabel()->text() == QStringLiteral("Transcribing chunk 1/3..."));
}

// set_status outside a job is dropped: neither the panel nor the engine readout takes it.
TEST_CASE("a status outside a job is dropped", "[test_upload_file_tab][widget]") {
    OneFile rig;
    const QString readout = rig->resolvedLabel()->text();
    rig->setStatus(QStringLiteral("Copied to clipboard"));
    CHECK(rig->resolvedLabel()->text() == readout);
    CHECK(rig.panel().detailLabel()->text().isEmpty());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcript_finishes_the_job_and_returns_the_actions
TEST_CASE("transcript finishes the job and returns the actions", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->setTranscript(QStringLiteral("Hello there"));
    CHECK_FALSE(rig->isTranscribing());
    CHECK(rig.panel().stage() == ProgressStage::Done);
    CHECK(rig->modelCombo()->isEnabled());
    CHECK(rig.card().copyButton()->isActive());

    // A None routed after the result is a no-op: the job is already over.
    rig->setProgressState(OverlayState::None);
    CHECK(rig.panel().stage() == ProgressStage::Done);

    fvtest::expire(rig.card().settleTimer());
    CHECK_FALSE(rig.card().isProgressShown());
    CHECK(rig->findChild<QLabel*>(QStringLiteral("statusLabel")) == nullptr);
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_stats_land_in_the_card_not_the_strip
TEST_CASE("stats land in the card not the strip", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("Hello there"));

    rig->setTranscriptionStats(6.9, 10.2, 875 * 1024);

    fv::DecodeLabel* note = rig.card().resultLabel();
    CHECK_FALSE(note->isHidden());
    CHECK(note->text().contains(QStringLiteral("6.9s")));
    CHECK(note->text().contains(QStringLiteral(u"1.5×")));
    CHECK(rig->statsWidget()->isHidden());

    rig->clearTranscriptionStats();
    CHECK(note->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcription_stats_includes_cleanup_time_when_provided
TEST_CASE("transcription stats includes cleanup time when provided",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("Hello there"));

    rig->setTranscriptionStats(24.7, 73.3, 875 * 1024, 4.2);

    fv::DecodeLabel* note = rig.card().resultLabel();
    CHECK_FALSE(note->isHidden());
    CHECK(note->text().contains(QStringLiteral("24.7s")));
    CHECK(note->text().contains(QStringLiteral(u"3.0×")));
    CHECK(note->text().contains(QStringLiteral("Cleaned in")));
    CHECK(note->text().contains(QStringLiteral("4.2s")));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_transcription_stats_omits_cleanup_time_when_none_or_zero
TEST_CASE("transcription stats omits cleanup time when none or zero",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("Hello there"));

    // None and 0.0 are one value here: the base's cleanupTime defaults to 0.
    rig->setTranscriptionStats(24.7, 73.3, 875 * 1024);
    fv::DecodeLabel* note = rig.card().resultLabel();
    CHECK_FALSE(note->isHidden());
    CHECK_FALSE(note->text().contains(QStringLiteral("Cleaned in")));

    rig->setTranscriptionStats(24.7, 73.3, 875 * 1024, 0.0);
    CHECK_FALSE(note->text().contains(QStringLiteral("Cleaned in")));
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_starting_a_job_clears_the_previous_result_note
TEST_CASE("starting a job clears the previous result note", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->setTranscriptionStats(6.9, 10.2, 1024);

    rig->transcribe();
    CHECK(rig.card().resultLabel()->isHidden());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_finished_result_clears_when_other_files_come_up
TEST_CASE("a finished result clears when other files come up", "[test_upload_file_tab][widget]") {
    const std::string nextStep = GENERATE("add", "remove");
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("First file's words"));
    rig->setTranscriptionStats(6.9, 10.2, 1024);
    CHECK_FALSE(rig->transcriptText()->toPlainText().isEmpty());

    if (nextStep == "remove") {
        rig->clearFile();
    } else {
        rig.add({rig.file("second.wav")});
    }

    CHECK(rig->transcriptText()->toPlainText().isEmpty());
    CHECK(rig.card().resultLabel()->isHidden());
    CHECK_FALSE(rig.card().copyButton()->isActive());
    CHECK(rig->expandButton()->isHidden());
}

// A drop of files already queued keeps the result: nothing new came up.
TEST_CASE("a drop of queued files only keeps the result", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("First file's words"));
    rig.add({rig.path});
    CHECK_FALSE(rig->transcriptText()->toPlainText().isEmpty());
    CHECK(rig.card().copyButton()->isActive());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_starting_a_job_clears_the_previous_transcript
TEST_CASE("starting a job clears the previous transcript", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("First pass"));
    rig.card().finishTranscribing(true);

    rig->transcribe();
    CHECK(rig->transcriptText()->toPlainText().isEmpty());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_a_failed_job_says_why_on_the_card_not_in_the_transcript
TEST_CASE("a failed job says why on the card not in the transcript",
          "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->showJobError(QStringLiteral("Error: The speech model isn't downloaded yet. Approve the "
                                     "download, then try again."));

    fv::FileInfoCard& card = rig.card();
    CHECK(card.progressPanel()->stage() == ProgressStage::Failed);
    CHECK(rig->transcriptText()->toPlainText().isEmpty());
    CHECK_FALSE(card.copyButton()->isActive());
    CHECK_FALSE(rig->isTranscribing());
    CHECK(card.resultLabel()->property("tone").toString() == QStringLiteral("danger"));
    CHECK(card.resultLabel()->text().startsWith(QStringLiteral("The speech model isn")));
    // The next job's timing replaces the reason in the normal tone.
    rig->transcribe();
    rig->setTranscript(QStringLiteral("Words"));
    rig->setTranscriptionStats(1.0, 2.0, 1024);
    CHECK(card.resultLabel()->property("tone").toString().isEmpty());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_a_canceled_job_leaves_no_error_note
TEST_CASE("a canceled job leaves no error note", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setProgressState(OverlayState::Canceling);

    rig->showJobError(QStringLiteral("Error: Transcription canceled"));

    CHECK(rig.panel().stage() == ProgressStage::Canceled);
    CHECK(rig.card().resultLabel()->isHidden());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_error_transcript_marks_the_job_failed
TEST_CASE("error transcript marks the job failed", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->setTranscript(QStringLiteral("Error: model not downloaded"));
    CHECK(rig.panel().stage() == ProgressStage::Failed);
    CHECK_FALSE(rig.card().copyButton()->isActive());
}

// An empty decode is a finished job with nothing to copy (EMPTY_ASR_MESSAGE).
TEST_CASE("no speech finishes the job with nothing to copy", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();
    rig->setTranscript(QStringLiteral("No speech detected (empty after VAD)"));
    CHECK(rig.panel().stage() == ProgressStage::Done);
    CHECK_FALSE(rig.card().copyButton()->isActive());
    CHECK(rig->expandButton()->isHidden());
}

// from tests/test_upload_file_tab.py::TestUploadFileTab::test_none_while_running_releases_the_card
TEST_CASE("none while running releases the card", "[test_upload_file_tab][widget]") {
    OneFile rig;
    rig->transcribe();

    rig->setProgressState(OverlayState::None);
    CHECK_FALSE(rig->isTranscribing());
    CHECK(rig->modelCombo()->isEnabled());
    CHECK(rig.panel().stage() == ProgressStage::Failed);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_progress_state_is_ignored_when_nothing_is_running
TEST_CASE("progress state is ignored when nothing is running", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setProgressState(OverlayState::Transcribing);
    CHECK_FALSE(rig.panel().stage().has_value());
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_scroll_area_handles_constrained_height
TEST_CASE("scroll area handles constrained height", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->resize(500, 350);
    rig->show();
    QApplication::processEvents();

    CHECK(rig->scrollArea()->isVisible());
    CHECK(rig->contentLayout()->count() > 0);
    CHECK(rig->dropZone()->minimumHeight() == 150);
}

// from
// tests/test_upload_file_tab.py::TestUploadFileTab::test_expand_and_collapse_transcription_with_scroll_area
TEST_CASE("expand and collapse transcription with scroll area", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->resize(605, 580);
    rig->show();
    QApplication::processEvents();

    const int engineHeight = rig->engineCard()->height();

    rig->setTranscriptionCollapsed(false);
    QApplication::processEvents();
    CHECK_FALSE(rig->isTranscriptionCollapsed());

    rig->setTranscriptionCollapsed(true);
    QApplication::processEvents();
    CHECK(rig->isTranscriptionCollapsed());
    CHECK(rig->engineCard()->height() == engineHeight);
}

// The browser (the drop zone's click and the queue's Add files) queues what was chosen.
TEST_CASE("the file browser queues what was chosen", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    const QStringList chosen = rig.files({"a.wav", "b.wav"});
    int asked = 0;
    rig->setFileBrowser([&](QWidget*) {
        ++asked;
        return chosen;
    });
    Q_EMIT rig->dropZone()->browseRequested();
    rig.deliver();
    CHECK(asked == 1);
    CHECK(rig->queuedPaths() == chosen);

    rig->setFileBrowser([&](QWidget*) {
        ++asked;
        return QStringList();
    });
    rig.queue().addButton()->click();
    CHECK(asked == 2);
    CHECK(rig->queuedPaths() == chosen);
}

// ---- TestEngineCard ----

// from tests/test_upload_file_tab.py::TestEngineCard::test_card_height_is_stable_across_backends
TEST_CASE("card height is stable across backends", "[test_upload_file_tab][widget]") {
    // Under the theme, which sizes the fields: unstyled, every combo is the platform default and
    // a field that missed the theme's rule would pass.
    fvtest::AppTheme theme;
    UploadRig rig;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    rig->setApiServer(new fv::SpeechServerControls(rig.settings, client, io));
    rig->resize(605, 580);
    rig->show();
    QApplication::processEvents();

    const int localHeight = rig->engineCard()->height();

    rig->setLocalEngineVisible(false);
    QApplication::processEvents();
    CHECK(rig->engineCard()->height() == localHeight);
    CHECK_FALSE(rig->localEngine()->isVisible());

    rig->setLocalEngineVisible(true);
    QApplication::processEvents();
    CHECK(rig->engineCard()->height() == localHeight);
    CHECK(rig->localEngine()->isVisible());
    rig.tab.reset();
    fv::SpeechServerControls::resetCatalogs();
}

// from
// tests/test_upload_file_tab.py::TestEngineCard::test_cleanup_toggle_stays_reachable_on_api_backend
TEST_CASE("cleanup toggle stays reachable on api backend", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->show();
    rig->setLocalEngineVisible(false);
    QApplication::processEvents();

    CHECK(rig->cleanupCheck()->isVisible());
}

// from
// tests/test_upload_file_tab.py::TestEngineCard::test_card_does_not_raise_the_content_width_floor
TEST_CASE("card does not raise the content width floor", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    QWidget* column = rig->engineCard()->parentWidget();
    CHECK(rig->engineCard()->minimumSizeHint().width() <= column->minimumWidth());
}

// from tests/test_upload_file_tab.py::TestEngineCard::test_resolved_readout_clears_for_api_backends
TEST_CASE("resolved readout clears for api backends", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | cuda"), true);
    CHECK(rig->resolvedLabel()->text() == QStringLiteral("Parakeet TDT 0.6B v3 | cuda"));

    rig->setDeviceInfo(QString());
    CHECK(rig->resolvedLabel()->text() == QStringLiteral("Ready to transcribe"));
}

// from
// tests/test_upload_file_tab.py::TestEngineCard::test_backend_reflects_selection_without_emitting
TEST_CASE("backend reflects selection without emitting", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    std::vector<QString> announced;
    QObject::connect(rig.tab.get(), &UploadFileTab::modelChanged,
                     [&](const QString& name) { announced.push_back(name); });

    rig->setBackend(QStringLiteral("API"));
    CHECK(rig->currentBackend() == QStringLiteral("API"));
    CHECK(rig->modelCombo()->currentText() == QStringLiteral("API"));
    CHECK(announced.empty());

    rig->chooseBackend(QStringLiteral("Parakeet"));
    CHECK(announced == strings({"Parakeet"}));
}

// from tests/test_upload_file_tab.py::TestEngineCard::test_choosing_the_active_backend_is_a_no_op
TEST_CASE("choosing the active backend is a no op", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setBackend(QStringLiteral("Parakeet"));
    std::vector<QString> announced;
    QObject::connect(rig.tab.get(), &UploadFileTab::modelChanged,
                     [&](const QString& name) { announced.push_back(name); });

    rig->chooseBackend(QStringLiteral("Parakeet"));
    CHECK(announced.empty());
}

// from tests/test_upload_file_tab.py::TestEngineCard::test_backend_locks_while_transcribing
TEST_CASE("backend locks while transcribing", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setBackendEnabled(false);
    CHECK_FALSE(rig->modelCombo()->isEnabled());

    rig->setBackendEnabled(true);
    CHECK(rig->modelCombo()->isEnabled());
}

// ---- TestEngineStatusDots ----

// from tests/test_upload_file_tab.py::TestEngineStatusDots::test_ready_engine_reads_green
TEST_CASE("ready engine reads green", "[test_upload_file_tab][widget]") {
    UploadRig rig;
    rig->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | cuda"), true);
    CHECK(rig->statusDot()->status() == fv::EngineStatus::Ready);
}

// from tests/test_upload_file_tab.py::TestEngineStatusDots::test_unusable_engine_reads_as_attention
TEST_CASE("unusable engine reads as attention", "[test_upload_file_tab][widget]") {
    // A missing model is the case the dot exists for.
    UploadRig rig;
    rig->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | not downloaded"), false);
    CHECK(rig->statusDot()->status() == fv::EngineStatus::Attention);
}

// from tests/test_upload_file_tab.py::TestEngineStatusDots::test_unreported_engine_stays_neutral
TEST_CASE("unreported engine stays neutral", "[test_upload_file_tab][widget]") {
    // Nothing has loaded yet at startup; green would be a guess.
    UploadRig rig;
    rig->setDeviceInfo(QStringLiteral("Parakeet TDT 0.6B v3 | cuda"), true);
    rig->setDeviceInfo(QStringLiteral("Not initialized"));
    CHECK(rig->statusDot()->status() == fv::EngineStatus::Unknown);
}

// from tests/test_upload_file_tab.py::TestEngineStatusDots::test_dots_go_with_the_local_engine
TEST_CASE("dots go with the local engine", "[test_upload_file_tab][widget]") {
    // An API backend has no local engine for the dots to report on.
    UploadRig rig;
    rig->show();

    rig->setLocalEngineVisible(false);
    QApplication::processEvents();
    CHECK_FALSE(rig->statusDot()->isVisible());

    rig->setLocalEngineVisible(true);
    QApplication::processEvents();
    CHECK(rig->statusDot()->isVisible());
}

// ---- TestLocalEngineFields (Parakeet's Model, Device and Language; Python's are Whisper's
// Model, Device and Quant) ----

namespace {

struct LocalFields {
    LocalFields()
        : settings(Json{{"selected_model", "parakeet"},
                        {"local_asr_models", {{"parakeet", "parakeet-v3"}}},
                        {"local_asr_devices", {{"parakeet", "auto"}}}}),
          controls(settings, fv::speechEngineCatalog(), QStringLiteral("parakeet")) {
        QObject::connect(&controls, &fv::LocalEngineControls::engineSettingsChanged,
                         [this] { ++reloads; });
    }
    fv::MemorySettingsStore settings;
    fv::LocalEngineControls controls;
    int reloads = 0;
};

// An item of combo other than the current one.
int otherIndex(const QComboBox* combo) {
    return combo->currentIndex() == 0 ? 1 : 0;
}

} // namespace

// from
// tests/test_upload_file_tab.py::TestLocalEngineFields::test_changing_a_field_persists_it_and_asks_for_a_reload
TEST_CASE("changing a field persists it and asks for a reload", "[test_upload_file_tab][widget]") {
    LocalFields fields;
    const int which = GENERATE(0, 1, 2);
    QComboBox* combo = which == 0   ? fields.controls.modelCombo()
                       : which == 1 ? fields.controls.deviceCombo()
                                    : fields.controls.languageCombo();
    REQUIRE(combo->count() > 1);
    combo->setCurrentIndex(otherIndex(combo));
    const std::string value = combo->currentData().toString().toStdString();

    const Json& saved = *fields.settings.snapshot();
    if (which == 0) {
        CHECK(saved.at("local_asr_models").at("parakeet") == value);
    } else if (which == 1) {
        CHECK(saved.at("local_asr_devices").at("parakeet") == value);
    } else {
        CHECK(saved.at("local_asr_language") == value);
    }
    CHECK(fields.reloads == 1);
}

// from
// tests/test_upload_file_tab.py::TestLocalEngineFields::test_a_field_change_persists_all_three_values
TEST_CASE("a field change persists all three values", "[test_upload_file_tab][widget]") {
    // The reload reads settings, so a partial write would revert the rest.
    LocalFields fields;
    QComboBox* device = fields.controls.deviceCombo();
    device->blockSignals(true);
    device->setCurrentIndex(device->findData(QStringLiteral("cpu")));
    device->blockSignals(false);

    QComboBox* model = fields.controls.modelCombo();
    model->setCurrentIndex(otherIndex(model));

    const Json& saved = *fields.settings.snapshot();
    CHECK(saved.at("local_asr_models").at("parakeet") ==
          model->currentData().toString().toStdString());
    CHECK(saved.at("local_asr_devices").at("parakeet") == "cpu");
    CHECK(saved.contains("local_asr_language"));
}

// from tests/test_upload_file_tab.py::TestLocalEngineFields::test_set_values_is_silent
TEST_CASE("set values is silent", "[test_upload_file_tab][widget]") {
    // set_values is setBackend here: it shows the saved fields without emitting.
    LocalFields fields;
    fields.settings.update(Json{{"local_asr_devices", {{"parakeet", "cpu"}}}});
    fields.controls.setBackend(QStringLiteral("parakeet"));
    CHECK(fields.reloads == 0);
    CHECK(fields.controls.deviceCombo()->currentData().toString() == QStringLiteral("cpu"));
}

// from tests/test_upload_file_tab.py::TestLocalEngineFields::test_busy_locks_every_field
TEST_CASE("busy locks every field", "[test_upload_file_tab][widget]") {
    LocalFields fields;
    const std::vector<QComboBox*> all{fields.controls.modelCombo(), fields.controls.deviceCombo(),
                                      fields.controls.languageCombo()};

    fields.controls.setBusy(true);
    for (QComboBox* field : all) {
        CHECK_FALSE(field->isEnabled());
    }
    fields.controls.setBusy(false);
    for (QComboBox* field : all) {
        CHECK(field->isEnabled());
    }
}

// ---- TestMainWindowUploadTabIntegration ----

namespace {

// The shell and the record feature as Bootstrap builds them, with the window completed.
class WindowFixture {
public:
    WindowFixture()
        : paths_(fv::Paths::resolve(fv::identity(), fv::system_env())), ui_(qApp),
          context_(services()) {
        shell_.provide(context_);
        record_.provide(context_);
        record_.install(context_);
        shell_.install(context_);
        window().availableScreenRect = [] {
            return QRect(0, 0, 1920, 1080);
        };
        window().maxUsableHeight = [] {
            return 1040;
        };
        window().completeSetup();
    }

    fv::MainWindow& window() { return context_.service<fv::MainWindow>(); }
    fv::AppContext& context() { return context_; }

private:
    fv::AppServices services() {
        fv::AppServices made;
        made.identity = &fv::identity();
        made.paths = &paths_;
        made.settings = &settings_;
        made.ui = &ui_;
        return made;
    }

    fv::testkit::TempDataRoot root_;
    fv::Paths paths_;
    fv::MemorySettingsStore settings_;
    fv::qt::QtUiExecutor ui_;
    fv::AppContext context_;
    fv::ShellFeature shell_;
    fv::RecordFeature record_;
};

} // namespace

// from
// tests/test_upload_file_tab.py::TestMainWindowUploadTabIntegration::test_transcription_tabs_do_not_scroll_at_minimum_height
TEST_CASE("transcription tabs do not scroll at minimum height", "[test_upload_file_tab][widget]") {
    fvtest::AppTheme theme;
    WindowFixture fixture;
    fv::MainWindow& window = fixture.window();
    window.resize(fv::window_size::kDefaultWidth, fv::window_size::kMinHeight);
    window.show();
    QApplication::processEvents();

    for (const auto& [index, key] :
         {std::pair{fv::TabbedContentWidget::TabQuickRecord, fv::RecordFeature::kQuickRecordScreen},
          std::pair{fv::TabbedContentWidget::TabUploadFile, fv::RecordFeature::kUploadScreen}}) {
        window.tabbedContent()->setCurrentIndex(index);
        auto* tab =
            qobject_cast<fv::TranscriptionTabBase*>(fixture.context().screens().existing(key));
        REQUIRE(tab != nullptr);
        // The window grows into each page; its scrollbar stays off meanwhile.
        QApplication::processEvents();
        CHECK_FALSE(tab->scrollArea()->verticalScrollBar()->isVisible());
        QTest::qWait(400);
        CHECK(tab->isVisibleTo(&window));
        CHECK(tab->scrollArea()->verticalScrollBar()->maximum() == 0);
    }
    window.hide();
}

// ---- TestApiModelField ----

namespace {

// The API backend's fields over a fake network and a manual io executor.
struct ApiRig {
    ApiRig() { fv::SpeechServerControls::resetCatalogs(); }
    ~ApiRig() { fv::SpeechServerControls::resetCatalogs(); }

    std::unique_ptr<UploadFileTab> tab() {
        auto made = std::make_unique<UploadFileTab>(settings, fv::speechEngineCatalog());
        auto* controls = new fv::SpeechServerControls(settings, client, io);
        controls->setClock(clock);
        made->setApiServer(controls);
        return made;
    }

    fv::MemorySettingsStore settings;
    fv::http::FakeHttpClient client;
    fv::ManualExecutor io;
    fv::FakeClock clock;
};

} // namespace

// from
// tests/test_upload_file_tab.py::TestApiModelField::test_backend_switch_preserves_each_model_and_locks_api_choice
TEST_CASE("backend switch preserves each model and locks api choice",
          "[test_upload_file_tab][widget]") {
    ApiRig rig;
    auto tab = rig.tab();
    const QString localModel = tab->localEngine()->modelCombo()->currentText();
    // MODEL_CHOICES that FioriVoce has, sorted by casefold.
    std::vector<QString> backends;
    for (int i = 0; i < tab->modelCombo()->count(); ++i) {
        backends.push_back(tab->modelCombo()->itemData(i).toString());
    }
    CHECK(backends == strings({"api", "parakeet", "remote"}));
    tab->chooseBackend(QStringLiteral("API"));
    CHECK(tab->localEngine()->isHidden());
    REQUIRE(tab->apiServer() != nullptr);
    CHECK_FALSE(tab->apiServer()->isHidden());
    CHECK(tab->apiServer()->serverCombo()->currentData().toString() == QStringLiteral("openai"));
    QComboBox* combo = tab->apiServer()->modelCombo();
    choose(combo, QStringLiteral("whisper-1"));
    CHECK(rig.settings.snapshot()->at("api_transcription_model") == "whisper-1");
    tab->chooseBackend(QStringLiteral("Parakeet"));
    CHECK(tab->apiServer()->isHidden());
    CHECK(tab->localEngine()->modelCombo()->currentText() == localModel);
    tab->chooseBackend(QStringLiteral("API"));
    CHECK(combo->currentData().toString() == QStringLiteral("whisper-1"));
    CHECK(combo->currentText() == QStringLiteral("whisper-1 (retiring Feb 26, 2027)"));
    tab->setBackendEnabled(false);
    CHECK_FALSE(tab->apiServer()->modelCombo()->isEnabled());
    tab->setBackendEnabled(true);
    CHECK(tab->apiServer()->modelCombo()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestApiModelField::test_model_change_syncs_other_tab_without_feedback
TEST_CASE("model change syncs other tab without feedback", "[test_upload_file_tab][widget]") {
    ApiRig rig;
    auto first = rig.tab();
    auto second = rig.tab();
    first->setBackend(QStringLiteral("API"));
    second->setBackend(QStringLiteral("API"));
    std::vector<QString> announcements;
    QObject::connect(second.get(), &UploadFileTab::modelChanged,
                     [&](const QString& name) { announcements.push_back(name); });
    QObject::connect(first.get(), &UploadFileTab::modelChanged, second.get(),
                     &UploadFileTab::setBackend);
    QComboBox* firstModels = first->apiServer()->modelCombo();
    QComboBox* secondModels = second->apiServer()->modelCombo();
    choose(firstModels, QStringLiteral("gpt-4o-mini-transcribe"));
    CHECK(secondModels->currentData().toString() == QStringLiteral("gpt-4o-mini-transcribe"));
    CHECK(announcements.empty());
    rig.settings.save_setting("api_transcription_model", "whisper-1");
    first->chooseBackend(QStringLiteral("API"));
    CHECK(firstModels->currentData().toString() == QStringLiteral("whisper-1"));
    CHECK(secondModels->currentData().toString() == QStringLiteral("whisper-1"));
}

// ---- TestChunkCountFollowsTheEngine ----

namespace {

// write_wav: a 440 Hz tone as a real 16-bit mono WAV, written a second at a time.
QString writeWav(const fs::path& path, double seconds, int rate = 44100) {
    fv::audio::WavWriter writer(path, {.rate = rate, .channels = 1, .sample_width = 2});
    const auto frames = static_cast<std::int64_t>(seconds * rate);
    std::vector<std::int16_t> block;
    for (std::int64_t start = 0; start < frames; start += rate) {
        const std::int64_t count = std::min<std::int64_t>(rate, frames - start);
        block.clear();
        for (std::int64_t i = start; i < start + count; ++i) {
            block.push_back(static_cast<std::int16_t>(
                std::sin(2 * std::numbers::pi * 440.0 * static_cast<double>(i) / rate) * 8000));
        }
        writer.write(std::span<const std::int16_t>(block));
    }
    writer.commit(std::nullopt);
    return QString::fromStdString(fv::base_utf8::path_to_utf8(path));
}

// Python shrinks MAX_FILE_SIZE_MB to 0.05 for a 3 s file; the limit is a constant here, so the
// long file is over the real 23 MiB: 300 s of 44.1 kHz mono is 25.2 MiB.
constexpr double kLongSeconds = 300.0;

// Whether audio_processor counted decoded samples (the full decode the preview avoids).
class DecodeLog : public fv::log::Sink {
public:
    void write(const fv::log::Record& record) override {
        if (record.logger == "services.audio_processor" &&
            record.message.find("counting decoded samples") != std::string::npos) {
            const std::scoped_lock lock(mutex_);
            decoded_ = true;
        }
    }
    bool decoded() const {
        const std::scoped_lock lock(mutex_);
        return decoded_;
    }

private:
    mutable std::mutex mutex_;
    bool decoded_ = false;
};

// A tab reading real files with the real preview, on the backend given.
struct ChunkRig : UploadRig {
    explicit ChunkRig(const char* backend) : log(std::make_shared<DecodeLog>()) {
        fv::log::add_sink(log);
        reader = [](const QString& path, bool engineSplits) {
            return UploadFileTab::readPreview(fv::base_utf8::path_from_utf8(path.toStdString()),
                                              engineSplits);
        };
        tab->setBackend(QString::fromUtf8(backend));
    }
    ~ChunkRig() { fv::log::remove_sink(log); }

    std::shared_ptr<DecodeLog> log;
};

} // namespace

// from
// tests/test_upload_file_tab.py::TestChunkCountFollowsTheEngine::test_local_engine_takes_a_large_file_in_one_pass_without_decoding
TEST_CASE("local engine takes a large file in one pass without decoding",
          "[test_upload_file_tab][widget]") {
    ChunkRig rig("Parakeet");
    rig.add({writeWav(rig.root.path() / "long.wav", kLongSeconds)});
    QLabel* label = rig.card().chunkLabel();

    CHECK(label->text() == QStringLiteral("One pass"));
    CHECK(label->property("tone").toString() == QStringLiteral("ok"));
    CHECK_FALSE(rig.log->decoded());
    CHECK(rig.card().transcribeButton()->isEnabled());
}

// from
// tests/test_upload_file_tab.py::TestChunkCountFollowsTheEngine::test_api_counts_the_chunks_and_an_engine_switch_rereads
TEST_CASE("api counts the chunks and an engine switch rereads", "[test_upload_file_tab][widget]") {
    ChunkRig rig("API");
    rig.add({writeWav(rig.root.path() / "long.wav", kLongSeconds)});
    QLabel* label = rig.card().chunkLabel();

    CHECK(label->text().endsWith(QStringLiteral(" chunks")));
    CHECK(label->property("tone").toString() == QStringLiteral("warn"));
    CHECK_FALSE(rig.log->decoded());

    rig->setBackend(QStringLiteral("Parakeet"));
    rig.deliver();
    CHECK(label->text() == QStringLiteral("One pass"));
    CHECK_FALSE(rig.log->decoded());

    rig->chooseBackend(QStringLiteral("API"));
    rig.deliver();
    CHECK(label->text().endsWith(QStringLiteral(" chunks")));
    CHECK_FALSE(rig.log->decoded());
}

// from
// tests/test_upload_file_tab.py::TestChunkCountFollowsTheEngine::test_a_reread_keeps_finished_rows_and_skips_small_files
TEST_CASE("a reread keeps finished rows and skips small files", "[test_upload_file_tab][widget]") {
    ChunkRig rig("Parakeet");
    const QString longWav = writeWav(rig.root.path() / "long.wav", kLongSeconds);
    const QString shortWav = writeWav(rig.root.path() / "short.wav", 0.2);
    int reads = 0;
    rig.reader = [&reads](const QString& path, bool engineSplits) {
        ++reads;
        return UploadFileTab::readPreview(fv::base_utf8::path_from_utf8(path.toStdString()),
                                          engineSplits);
    };
    rig.add({longWav, shortWav});
    REQUIRE(reads == 2);
    // Both files finished a job: the tab's rows say so.
    rig->transcribe();
    rig->setBatchProgress(1, 2, QStringLiteral("long.wav"));
    rig->setBatchItemFinished(1, true);
    rig->setBatchProgress(2, 2, QStringLiteral("short.wav"));
    rig->setBatchItemFinished(2, true);
    rig->setTranscript(QStringLiteral("## long.wav\n\nA\n\n## short.wav\n\nB"));
    REQUIRE(rowStates(rig.queue()) == strings({"done", "done"}));

    rig->setBackend(QStringLiteral("API"));
    rig.deliver();

    const auto rows = rig.queue().rows();
    CHECK(rows[0]->chunkChip()->text().endsWith(QStringLiteral(" chunks")));
    CHECK(rows[1]->chunkChip()->text() == QStringLiteral("One pass"));
    CHECK(rowStates(rig.queue()) == strings({"done", "done"}));
    // A small file is not re-read.
    CHECK(reads == 3);
    CHECK_FALSE(rig.log->decoded());
}

// ---- tests/test_personalize_fix3_h2_voice_languages.py ----

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestOneLanguageControl::test_upload_keeps_its_language_and_quick_record_follows_the_chips
TEST_CASE("upload keeps its language and quick record follows the chips",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"},
                                          {"dictation_languages", Json::array({"en", "es"})},
                                          {"dictation_active_language", "en"}});
    fv::LocalEngineControls files(settings, fv::speechEngineCatalog(), QStringLiteral("parakeet"));
    fv::LocalEngineControls dictation(settings, fv::speechEngineCatalog(),
                                      QStringLiteral("parakeet"), true);
    for (fv::LocalEngineControls* controls : {&files, &dictation}) {
        controls->setBackend(QStringLiteral("parakeet"));
    }
    CHECK_FALSE(files.languageField()->isHidden());
    CHECK(dictation.languageField()->isHidden());

    fv::QuickRecordTab record(settings, fv::speechEngineCatalog());
    record.setModelSelection(QStringLiteral("parakeet"));
    CHECK(record.localEngine()->languageField()->isHidden());
    UploadFileTab upload(settings, fv::speechEngineCatalog());
    upload.setModelSelection(QStringLiteral("parakeet"));
    CHECK_FALSE(upload.localEngine()->languageField()->isHidden());
}

// from
// tests/test_personalize_fix3_h2_voice_languages.py::TestOneLanguageControl::test_a_languages_change_reaches_the_main_window_fields
TEST_CASE("a languages change reaches the main window fields",
          "[test_personalize_fix3_h2_voice_languages][widget]") {
    fv::MemorySettingsStore settings(Json{{"selected_model", "parakeet"}});
    QObject context;
    fv::qt::QtUiExecutor ui(&context);
    fv::EngineSelection selection(settings, ui);
    fv::QuickRecordTab record(settings, fv::speechEngineCatalog());
    UploadFileTab upload(settings, fv::speechEngineCatalog());
    selection.add(&record);
    selection.add(&upload);
    record.syncEngineSelection();
    upload.syncEngineSelection();
    CHECK_FALSE(record.localEngine()->languageField()->isHidden());

    // Saving the languages is all Settings does; the tab hears it.
    Json chosen = *settings.snapshot();
    fv::speech::set_chosen(chosen, {"en", "es"});
    Json patch = Json::object();
    for (const char* key : {"dictation_languages", "dictation_active_language"}) {
        if (chosen.contains(key)) {
            patch[key] = chosen.at(key);
        }
    }
    settings.update(patch);
    QApplication::processEvents();
    CHECK(record.localEngine()->languageField()->isHidden());
    // The Upload tab keeps the engine's Language field: files use the engine's language.
    CHECK_FALSE(upload.localEngine()->languageField()->isHidden());
}
