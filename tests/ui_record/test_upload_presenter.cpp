// The Upload File tab's side of the UI controller (ui_qt/ui_controller.py 934-970, 2450-2507):
// the tab's requests reach the runtime, the job's news lands on the tab's card while the job is
// the tab's, and copies go through the clipboard. The runtime is stood in for by the ports, the
// clipboard by focus::FakeClipboard; the real clipboard is never written.
#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <QApplication>
#include <QPointer>
#include <QPushButton>
#include <QWidget>

#include "appkit/registries.h"
#include "base/utf8.h"
#include "dictation/transcription_runtime.h"
#include "focus/testing/fake_clipboard.h"
#include "record_fixture.h"
#include "settings/testing/memory_store.h"
#include "testkit/testkit.h"
#include "ui_record/file_info_card.h"
#include "ui_record/local_engine_catalog.h"
#include "ui_record/transcription_progress.h"
#include "ui_record/upload_file_tab.h"
#include "ui_record/upload_presenter.h"
#include "ui_record/upload_queue.h"
#include "ui_shell/main_window.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/downloads_label.h"
#include "ui_widgets/eliding_label.h"
#include "upload_widgets_support.h"

namespace {

using fv::Json;
using fv::UploadFileTab;
using fv::UploadPresenter;
using fv::appcore::OverlayState;
using fv::dictation::ProgressStage;

// A registry with the upload screen, the presenter over recording ports, and a file to upload.
class PresenterRig {
public:
    explicit PresenterRig(bool quickRecordLoading = false) : loading(quickRecordLoading) {
        screens.define(fv::screen_key::upload, 200, QStringLiteral("Upload File"),
                       [this](QWidget* parent) -> QWidget* {
                           auto* tab =
                               new UploadFileTab(settings, fv::speechEngineCatalog(), parent);
                           tab->setPreviewRunner([](std::function<void()> worker) { worker(); });
                           tab->setPreviewReader([](const std::filesystem::path& path, bool) {
                               fv::audio::AudioFilePreview preview = fvtest::samplePreview();
                               preview.file_path = fv::base_utf8::path_to_utf8(path);
                               return preview;
                           });
                           return tab;
                       });
        UploadPresenter::Ports ports;
        ports.uploadFile = [this](const std::filesystem::path& path, std::optional<double> s) {
            uploads.emplace_back(QString::fromStdString(fv::base_utf8::path_to_utf8(path)), s);
        };
        ports.uploadFiles = [this](const fv::dictation::BatchUploadRequest& request) {
            batches.push_back(request);
        };
        ports.cancel = [this] {
            ++cancels;
        };
        ports.clipboard = &clipboard;
        ports.windowStatus = [this](const QString& status) {
            windowStatuses.push_back(status);
        };
        ports.quickRecordLoading = [this] {
            return loading;
        };
        presenter = std::make_unique<UploadPresenter>(screens, std::move(ports));

        file = QString::fromStdString(fv::base_utf8::path_to_utf8(root.path() / "talk.wav"));
        std::ofstream(fv::base_utf8::path_from_utf8(file.toStdString()), std::ios::binary)
            << std::string(64, '\0');
    }
    ~PresenterRig() {
        delete screens.existing(fv::screen_key::upload);
        QApplication::processEvents();
    }

    // Builds the tab as the window would on its first show, with the file queued.
    UploadFileTab& open() {
        auto* tab = qobject_cast<UploadFileTab*>(screens.ensure(fv::screen_key::upload, &host));
        tab->addFiles({file});
        QApplication::processEvents();
        return *tab;
    }

    fv::testkit::TempDataRoot root;
    fv::MemorySettingsStore settings;
    fv::ScreenRegistry screens;
    QWidget host;
    fv::focus::FakeClipboard clipboard;
    bool loading = false;
    std::vector<std::pair<QString, std::optional<double>>> uploads;
    std::vector<fv::dictation::BatchUploadRequest> batches;
    int cancels = 0;
    std::vector<QString> windowStatuses;
    QString file;
    std::unique_ptr<UploadPresenter> presenter;
};

} // namespace

// ui_controller.py 2450-2464 with main_window.py 749-751: a Transcribe on the tab reaches the
// runtime, and the job's results are the tab's from then on.
TEST_CASE("the tab's transcribe reaches the runtime and makes the job the tab's",
          "[upload_presenter][widget]") {
    PresenterRig rig;
    CHECK(rig.presenter->tab() == nullptr);
    UploadFileTab& tab = rig.open();
    CHECK(rig.presenter->tab() == &tab);
    CHECK_FALSE(rig.presenter->isUploadSource());

    tab.transcribe();

    REQUIRE(rig.uploads.size() == 1);
    CHECK(rig.uploads[0].first == rig.file);
    CHECK(rig.uploads[0].second == 60.0);
    CHECK(rig.presenter->isUploadSource());
    CHECK(rig.presenter->isUploadJobActive());

    // A recording started elsewhere takes the results back to Quick Record.
    rig.presenter->recordingStarted();
    CHECK_FALSE(rig.presenter->isUploadJobActive());
    CHECK_FALSE(rig.presenter->displayTranscript(QStringLiteral("dictated"), std::nullopt));
}

TEST_CASE("a queue's transcribe reaches the runtime as one batch", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    const QString second =
        QString::fromStdString(fv::base_utf8::path_to_utf8(rig.root.path() / "b.wav"));
    std::ofstream(fv::base_utf8::path_from_utf8(second.toStdString()), std::ios::binary) << "x";
    tab.addFiles({second});
    QApplication::processEvents();

    tab.transcribe();

    REQUIRE(rig.batches.size() == 1);
    CHECK(rig.batches[0].items.size() == 2);
    CHECK(rig.uploads.empty());
    CHECK(rig.presenter->isUploadJobActive());

    rig.presenter->setBatchProgress(1, 2, QStringLiteral("talk.wav"));
    CHECK(tab.fileInfoCard()->queue()->rows()[0]->state() == QStringLiteral("active"));
    rig.presenter->setBatchItemFinished(1, true, QStringLiteral("Hello"));
    CHECK(tab.fileInfoCard()->queue()->rows()[0]->transcript() == QStringLiteral("Hello"));
}

// _route_upload_progress (942-959) and show_large_file_state (961-970).
TEST_CASE("an upload job's stages go to the card, not the overlay", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    // Nothing runs yet: the overlay keeps every state.
    CHECK_FALSE(rig.presenter->routeProgress(OverlayState::Transcribing));
    CHECK_FALSE(rig.presenter->showLargeFileState(30.0));

    tab.transcribe();
    fv::TranscriptionProgressPanel& panel = *tab.fileInfoCard()->progressPanel();

    CHECK(rig.presenter->showLargeFileState(30.0));
    CHECK(panel.stage() == ProgressStage::Splitting);
    CHECK(rig.presenter->routeProgress(OverlayState::Transcribing));
    CHECK(panel.stage() == ProgressStage::Transcribing);
    // A state with no stage of the card stays the overlay's.
    CHECK_FALSE(rig.presenter->routeProgress(OverlayState::Recording));
    // None reaches the card too, but the shared teardown still runs.
    CHECK_FALSE(rig.presenter->routeProgress(OverlayState::None));
    CHECK(panel.stage() == ProgressStage::Failed);
    CHECK_FALSE(tab.isTranscribing());
}

// _display_transcript and _display_transcription_error (686-704) and set_transcription_stats.
TEST_CASE("an upload job's result lands on the tab", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    CHECK_FALSE(rig.presenter->displayTranscript(QStringLiteral("x"), std::nullopt));

    tab.transcribe();
    CHECK(rig.presenter->displayTranscript(QStringLiteral("Hello there"), std::nullopt));
    CHECK(tab.shownTranscript() == QStringLiteral("Hello there"));
    CHECK_FALSE(tab.isTranscriptionCollapsed());
    CHECK_FALSE(tab.isTranscribing());

    fv::dictation::TranscriptionStats stats;
    stats.transcription_time = 6.9;
    stats.audio_duration = 10.2;
    stats.file_size = 1024;
    CHECK(rig.presenter->setTranscriptionStats(stats));
    CHECK_FALSE(tab.fileInfoCard()->resultLabel()->isHidden());
    CHECK_FALSE(tab.fileInfoCard()->resultLabel()->text().contains(QStringLiteral(" network")));
    // A paired computer's pass names it, and the network's share (set_result's remote).
    stats.remote = fv::remote::RemoteTiming{"studio", 2, 0.5, 0.469};
    CHECK(rig.presenter->setTranscriptionStats(stats));
    const QString result = tab.fileInfoCard()->resultLabel()->text();
    CHECK(result.contains(QStringLiteral(" on studio")));
    CHECK(result.contains(QStringLiteral(">31 ms</span>")));

    tab.transcribe();
    CHECK(rig.presenter->displayTranscriptionError(
        QStringLiteral("Error: The speech model isn't downloaded yet.")));
    CHECK(tab.fileInfoCard()->progressPanel()->stage() == ProgressStage::Failed);
}

// _on_upload_cancel (2466-2476): only while the tab's job runs.
TEST_CASE("stop cancels only the tab's own running job", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    rig.presenter->cancelUpload();
    CHECK(rig.cancels == 0);

    tab.transcribe();
    tab.fileInfoCard()->progressPanel()->stopButton()->click();
    CHECK(rig.cancels == 1);
}

// _on_upload_copy (2501-2507).
TEST_CASE("a copy from the tab goes through the clipboard", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    int animations = 0;
    QObject::connect(rig.presenter.get(), &UploadPresenter::copiedAnimationRequested,
                     [&] { ++animations; });

    tab.setTranscript(QStringLiteral("Hello there"));
    tab.fileInfoCard()->copyButton()->click();
    CHECK(rig.clipboard.text() == "Hello there");
    CHECK(animations == 1);

    // A copy that fails says so on the panel while it is up.
    tab.transcribe();
    rig.clipboard.fail_writes = true;
    rig.presenter->copy(QStringLiteral("again"));
    CHECK(animations == 1);
    CHECK(tab.fileInfoCard()->progressPanel()->detailLabel()->text() ==
          QStringLiteral("Copy failed"));
}

// from tests/test_engine_status.py::test_engine_messages_after_an_upload_reach_the_visible_status
TEST_CASE("engine messages after an upload reach the visible status",
          "[test_engine_status][upload_presenter][widget]") {
    PresenterRig rig(true);
    UploadFileTab& tab = rig.open();
    tab.transcribe();
    REQUIRE(rig.presenter->isUploadSource());

    rig.presenter->applyStatus(QStringLiteral("Downloading model..."));

    CHECK(rig.windowStatuses == std::vector<QString>{QStringLiteral("Downloading model...")});
    CHECK(tab.resolvedLabel()->text() == QStringLiteral("Downloading model..."));
}

// from tests/test_engine_status.py::test_upload_job_messages_stay_in_the_upload_panel
TEST_CASE("upload job messages stay in the upload panel",
          "[test_engine_status][upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    tab.transcribe();

    rig.presenter->applyStatus(QStringLiteral("Transcribing chunk 2/3..."));

    CHECK(tab.fileInfoCard()->progressPanel()->detailLabel()->text() ==
          QStringLiteral("Transcribing chunk 2/3..."));
    CHECK(rig.windowStatuses.empty());

    // Another job's status is the window's.
    rig.presenter->recordingStarted();
    rig.presenter->applyStatus(QStringLiteral("Recording..."));
    CHECK(rig.windowStatuses == std::vector<QString>{QStringLiteral("Recording...")});
}

// The presenter attaches a tab once, however often it is told.
TEST_CASE("a tab is attached once", "[upload_presenter][widget]") {
    PresenterRig rig;
    UploadFileTab& tab = rig.open();
    rig.presenter->attach(&tab);
    tab.transcribe();
    CHECK(rig.uploads.size() == 1);
}
