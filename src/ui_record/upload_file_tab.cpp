#include "ui_record/upload_file_tab.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <format>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <QPushButton>
#include <QSize>
#include <QVBoxLayout>

#include "appkit/app_context.h"
#include "appkit/qt_bridge.h"
#include "base/identity.h"
#include "base/log.h"
#include "base/utf8.h"
#include "dictation/transcript_text.h"
#include "settings/keys.h"
#include "settings/store.h"
#include "speech/upload_splitter.h"
#include "ui_record/batch_relation_dialog.h"
#include "ui_record/drop_zone.h"
#include "ui_record/file_info_card.h"
#include "ui_record/python_exception_name.h"
#include "ui_record/relation_picker.h"
#include "ui_record/stats_display.h"
#include "ui_record/transcript_pane.h"
#include "ui_record/transcript_viewer_dialog.h"
#include "ui_record/transcription_progress.h"
#include "ui_theme/icons.h"
#include "ui_widgets/local_engine_controls.h"
#include "ui_widgets/speech_backend_picker.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.upload_file_tab"};

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

TranscriptionTabKnobs uploadKnobs() {
    TranscriptionTabKnobs knobs;
    knobs.contentObjectName = QStringLiteral("uploadFileContent");
    knobs.initialStatus = QStringLiteral("Ready to transcribe");
    knobs.transcriptPlaceholder =
        QStringLiteral("Transcription will appear here...\nUpload an audio file to begin.");
    // A multi-file job heads each file's text with its name.
    knobs.transcriptMarkdown = true;
    // Files are transcribed whole; there is no while-you-speak preview here.
    knobs.livePreviewControl = false;
    // Files always use the engine's language, so its Language field stays.
    knobs.dictationLanguages = false;
    return knobs;
}

// type(exc).__name__: the Python class OpenWhisper would have named.
QString exceptionName(const std::exception& error) {
    return fromStd(pythonExceptionName(error));
}

// The worker's sentence for a file it could not read.
QString previewError(const std::exception_ptr& failure, const QString& path) {
    try {
        std::rethrow_exception(failure);
    } catch (const audio::AudioFileNotFound&) {
        return QStringLiteral("This file no longer exists.");
    } catch (const audio::UnreadableAudio& error) {
        return QString::fromUtf8(error.what());
    } catch (const std::exception& error) {
        kLog.warning("Reading {} failed: {}", path.toStdString(),
                     exceptionName(error).toStdString());
        return QStringLiteral("%1 couldn't read this file (%2).")
            .arg(fromStd(std::string(identity().product)), exceptionName(error));
    } catch (...) {
        kLog.warning("Reading {} failed", path.toStdString());
        return QStringLiteral("%1 couldn't read this file (Exception).")
            .arg(fromStd(std::string(identity().product)));
    }
}

void persist(SettingsStore& settings, std::string_view key, const Json& value) {
    try {
        settings.save_setting(key, value);
    } catch (const std::exception& error) {
        kLog.warning("Could not save {}: {}", key, error.what());
    }
}

} // namespace

// ---- UploadPreviewRelay ----

UploadPreviewRelay& UploadPreviewRelay::instance() {
    // Never deleted: a worker may still post to it while the application winds down.
    static UploadPreviewRelay* relay = new UploadPreviewRelay;
    return *relay;
}

void UploadPreviewRelay::post(UploadPreviewResult result) {
    qt::post(this, [this, result = std::move(result)] { Q_EMIT ready(result); });
}

std::uint64_t UploadPreviewRelay::nextToken() {
    static std::atomic<std::uint64_t> tokens{0};
    return ++tokens;
}

// ---- UploadFileTab ----

audio::AudioFilePreview UploadFileTab::readPreview(const std::filesystem::path& path,
                                                   bool engineSplits) {
    const audio::ChunkEstimator estimator = [](std::int64_t totalSamples, int sampleRate) {
        std::vector<std::int64_t> points;
        for (const std::size_t point :
             speech::time_based_splits(static_cast<std::size_t>(totalSamples), sampleRate)) {
            points.push_back(static_cast<std::int64_t>(point));
        }
        return points;
    };
    return audio::preview_file(path, engineSplits, &estimator);
}

UploadFileTab::UploadFileTab(SettingsStore& settings, LocalEngineCatalog catalog, QWidget* parent)
    : TranscriptionTabBase(settings, std::move(catalog), uploadKnobs(), parent),
      reader_(&UploadFileTab::readPreview),
      runner_([](std::function<void()> worker) { std::thread(std::move(worker)).detach(); }),
      browser_(&browseForAudioFiles), exists_([](const QString& path) {
          std::error_code error;
          return std::filesystem::exists(base_utf8::path_from_utf8(path.toStdString()), error);
      }) {
    initialize();

    expandButton_ = new QPushButton;
    expandButton_->setObjectName(QStringLiteral("transcriptExpandButton"));
    expandButton_->setIcon(ui::tablerIcon(QStringLiteral("arrows-diagonal-gray.svg")));
    expandButton_->setIconSize(QSize(16, 16));
    expandButton_->setFixedSize(26, 26);
    expandButton_->setCursor(Qt::PointingHandCursor);
    expandButton_->setFocusPolicy(Qt::NoFocus);
    expandButton_->setToolTip(QStringLiteral("Open in a reading window"));
    connect(expandButton_, &QPushButton::clicked, this, &UploadFileTab::openTranscriptViewer);
    expandButton_->hide();
    transcriptPane()->setCornerWidget(expandButton_);

    const Json snapshot = *settings.snapshot();
    relation_ = dictation::batch_relation_from(keys::transcript_batch_relation.read(snapshot))
                    .value_or(dictation::BatchRelation::Separate);
    customInstructions_ = fromStd(keys::transcript_batch_custom_instructions.read(snapshot));
    customCombine_ = keys::transcript_batch_custom_combine.read(snapshot);
    applyRelationToPicker();
}

UploadFileTab::~UploadFileTab() = default;

void UploadFileTab::buildContentBeforeStatus(QVBoxLayout* layout) {
    dropZone_ = new DropZoneWidget;
    layout->addWidget(dropZone_);

    fileInfoCard_ = new FileInfoCard;
    fileInfoCard_->hide();
    layout->addWidget(fileInfoCard_);
}

void UploadFileTab::connectSignals() {
    TranscriptionTabBase::connectSignals();
    connect(dropZone_, &DropZoneWidget::filesSelected, this, &UploadFileTab::addFiles);
    connect(dropZone_, &DropZoneWidget::browseRequested, this, &UploadFileTab::openFileBrowser);
    FileInfoCard* card = fileInfoCard_;
    connect(card, &FileInfoCard::transcribeClicked, this, &UploadFileTab::transcribe);
    connect(card, &FileInfoCard::removeClicked, this, &UploadFileTab::clearFile);
    connect(card, &FileInfoCard::copyClicked, this, &UploadFileTab::copyFixed);
    connect(card, &FileInfoCard::stopClicked, this, &UploadFileTab::stop);
    connect(card, &FileInfoCard::filesDropped, this, &UploadFileTab::addFiles);
    UploadQueueWidget* queue = card->queue();
    connect(queue, &UploadQueueWidget::addRequested, this, &UploadFileTab::openFileBrowser);
    connect(queue, &UploadQueueWidget::clearRequested, this, &UploadFileTab::clearFile);
    connect(queue, &UploadQueueWidget::removeRequested, this, &UploadFileTab::removeFile);
    connect(queue, &UploadQueueWidget::moveRequested, this, &UploadFileTab::moveFile);
    connect(queue, &UploadQueueWidget::rowCopyRequested, this, &UploadFileTab::copyRow);
    connect(queue->picker(), &RelationPicker::relationChanged, this,
            &UploadFileTab::onRelationChanged);
    connect(queue->picker(), &RelationPicker::editRequested, this, &UploadFileTab::onEditCustom);
    connect(statsWidget(), &TranscriptionStatsWidget::visibilityChanged, this,
            &UploadFileTab::statsVisibilityChanged);
    // Marks this tab's previews on the relay every tab shares.
    previewToken_ = UploadPreviewRelay::nextToken();
    connect(&UploadPreviewRelay::instance(), &UploadPreviewRelay::ready, this,
            &UploadFileTab::onPreviewReady);
}

bool UploadFileTab::isTranscribing() const {
    return fileInfoCard_->isTranscribing();
}

QStringList UploadFileTab::queuedPaths() const {
    QStringList paths;
    for (const QueueItem& item : items_) {
        paths.append(item.path);
    }
    return paths;
}

void UploadFileTab::setRecordingActive(bool active) {
    recordingActive_ = active;
    refreshReady();
    updateEngineLock();
}

void UploadFileTab::setStatus(const QString& statusText) {
    if (fileInfoCard_->isProgressShown()) {
        fileInfoCard_->progressPanel()->setDetail(statusText);
    }
}

void UploadFileTab::setProgressState(appcore::OverlayState state) {
    if (!isTranscribing()) {
        return;
    }
    if (state == appcore::OverlayState::None) {
        releaseWorkingQueueRows(false);
        fileInfoCard_->finishTranscribing(false);
        updateEngineLock();
        return;
    }
    fileInfoCard_->progressPanel()->applyOverlayState(state);
}

void UploadFileTab::setLargeFileStage(double fileSizeMb) {
    if (isTranscribing()) {
        fileInfoCard_->progressPanel()->setLargeFile(fileSizeMb);
    }
}

void UploadFileTab::setBatchProgress(int position, int total, const QString& sourceName) {
    if (!isTranscribing()) {
        return;
    }
    const int index = position - 1;
    if (index >= 0 && index < items_.size()) {
        items_[index].state = QString::fromLatin1(kRowActive);
        pushRows();
    }
    fileInfoCard_->progressPanel()->setBatchPosition(position, total, sourceName);
}

void UploadFileTab::setBatchItemFinished(int position, bool success, const QString& transcript) {
    if (!isTranscribing()) {
        return;
    }
    const int index = position - 1;
    if (index >= 0 && index < items_.size()) {
        QueueItem& item = items_[index];
        item.state = QString::fromLatin1(success ? kRowDone : kRowFailed);
        item.transcript = success ? transcript.trimmed() : QString();
        pushRows();
    }
}

// ---- queue ----

void UploadFileTab::addFiles(const QStringList& paths, int skipped) {
    if (isTranscribing()) {
        // Every way in is disabled while a job runs; a late drop is ignored.
        return;
    }
    QStringList existing;
    for (const QueueItem& item : items_) {
        existing.append(item.key());
    }
    QStringList added;
    int duplicates = 0;
    for (const QString& path : paths) {
        QueueItem item;
        item.path = path;
        const QString key = item.key();
        if (existing.contains(key)) {
            ++duplicates;
            continue;
        }
        existing.append(key);
        items_.append(item);
        added.append(item.path);
    }

    if (items_.isEmpty()) {
        if (skipped > 0) {
            dropZone_->setNotice(noAudioDroppedMessage());
        }
        return;
    }

    QStringList parts;
    if (duplicates > 0) {
        parts.append(QStringLiteral("%1 already queued").arg(duplicates));
    }
    if (skipped > 0) {
        parts.append(QStringLiteral("%1 skipped (not audio)").arg(skipped));
    }
    queueNote_ = parts.join(QStringLiteral(u"  ·  "));
    dropZone_->setNotice(QString());
    dropZone_->hide();
    fileInfoCard_->show();
    if (!added.isEmpty()) {
        resetResult();
    }
    render();
    if (!added.isEmpty()) {
        startPreviews(added);
    }
}

// Reads file facts off the GUI thread; results come back by path. preview_file decodes the
// whole of a file the engine will split, to count its chunks, so a run of long files would
// otherwise freeze the window once per file. One worker per drop keeps a ten-file drop from
// decoding ten files at once.
void UploadFileTab::startPreviews(const QStringList& paths) {
    const bool engineSplits = engineSplitsFiles();
    // Bound here: the worker must not reach back into the tab.
    const std::uint64_t token = previewToken_;
    UploadPreviewRelay* relay = &UploadPreviewRelay::instance();
    PreviewReader reader = reader_;
    runner_([paths, engineSplits, token, relay, reader] {
        for (const QString& path : paths) {
            UploadPreviewResult result;
            result.token = token;
            result.path = path;
            try {
                result.preview =
                    reader(base_utf8::path_from_utf8(path.toStdString()), engineSplits);
            } catch (...) {
                result.error = previewError(std::current_exception(), path);
            }
            relay->post(std::move(result));
        }
    });
}

void UploadFileTab::onPreviewReady(const UploadPreviewResult& result) {
    if (result.token != previewToken_) {
        return;
    }
    QueueItem* item = itemFor(result.path);
    if (item == nullptr) {
        return;
    }
    if (result.preview) {
        item->preview = result.preview;
        item->error.clear();
        if (item->state == QLatin1String(kRowReading)) {
            // A re-read for another engine keeps a finished job's state.
            item->state = QString::fromLatin1(kRowPending);
        }
        kLog.info("File loaded: {}", result.preview->file_name);
    } else {
        kLog.error("Could not read {}: {}", result.path.toStdString(), result.error.toStdString());
        if (items_.size() == 1) {
            // Alone, an unreadable file goes back to the drop zone, which says why.
            const QString name = item->name();
            clearFile();
            dropZone_->setNotice(QStringLiteral("%1: %2").arg(name, result.error));
            return;
        }
        item->preview.reset();
        item->error = result.error;
        item->state = QString::fromLatin1(kRowFailed);
    }
    render();
    // The engine may have changed while this file was being read.
    refreshChunkEstimates();
}

// Whether the selected engine uploads a file over the limit in chunks. Only the API backend
// does; every other engine takes a file of any size in one pass, so its preview needs no chunk
// count and no decode.
bool UploadFileTab::engineSplitsFiles() const {
    return currentBackend() == backendDisplayName("api");
}

// Re-reads the queued files whose chunk count the engine changes: a file over the upload limit
// read for an engine that splits it, now queued for one that does not, or the other way round.
void UploadFileTab::refreshChunkEstimates() {
    if (isTranscribing()) {
        return;
    }
    const bool engineSplits = engineSplitsFiles();
    QStringList stale;
    for (QueueItem& item : items_) {
        if (item.preview && item.preview->over_upload_limit() &&
            item.preview->needs_splitting != engineSplits) {
            stale.append(item.path);
            item.preview.reset();
        }
    }
    if (stale.isEmpty()) {
        return;
    }
    render();
    startPreviews(stale);
}

void UploadFileTab::onBackendChanged() {
    TranscriptionTabBase::onBackendChanged();
    refreshChunkEstimates();
}

void UploadFileTab::setBackend(const QString& displayName) {
    TranscriptionTabBase::setBackend(displayName);
    refreshChunkEstimates();
}

QueueItem* UploadFileTab::itemFor(const QString& path) {
    const QString key = QueueItem::keyFor(path);
    for (QueueItem& item : items_) {
        if (item.key() == key) {
            return &item;
        }
    }
    return nullptr;
}

void UploadFileTab::pushRows() {
    fileInfoCard_->queue()->setItems(items_);
}

void UploadFileTab::render() {
    FileInfoCard* card = fileInfoCard_;
    const qsizetype count = items_.size();
    if (count == 0) {
        clearFile();
        return;
    }
    if (count == 1) {
        const QueueItem& item = items_.front();
        card->setMode(FileInfoCard::Mode::Single);
        audioPath_ = item.path;
        preview_ = item.preview;
        if (item.preview) {
            card->setPreview(*item.preview);
        } else {
            card->setLoading(item.name());
        }
    } else {
        card->setMode(FileInfoCard::Mode::Queue);
        audioPath_.clear();
        preview_.reset();
        pushRows();
        applyRelationToPicker();
    }
    refreshReady();
}

void UploadFileTab::refreshReady() {
    int failed = 0;
    bool allRead = !items_.isEmpty();
    for (const QueueItem& item : items_) {
        if (!item.error.isEmpty()) {
            ++failed;
        }
        if (!item.preview) {
            allRead = false;
        }
    }
    const bool ready = allRead && failed == 0;
    fileInfoCard_->setReady(ready && !recordingActive_);
    QStringList notes;
    if (failed > 0) {
        notes.append(QStringLiteral(u"%1 could not be read — remove %2 to continue")
                         .arg(failed)
                         .arg(failed != 1 ? QStringLiteral("them") : QStringLiteral("it")));
    }
    if (!queueNote_.isEmpty()) {
        notes.append(queueNote_);
    }
    fileInfoCard_->queue()->setNote(notes.join(QStringLiteral(u"  ·  ")), failed > 0);
}

void UploadFileTab::moveFile(const QString& path, int delta) {
    if (isTranscribing()) {
        return;
    }
    QueueItem* item = itemFor(path);
    if (item == nullptr) {
        return;
    }
    const qsizetype index = item - items_.data();
    const qsizetype target = index + delta;
    if (target >= 0 && target < items_.size()) {
        items_.swapItemsAt(index, target);
        render();
    }
}

void UploadFileTab::removeFile(const QString& path) {
    if (isTranscribing()) {
        return;
    }
    QueueItem* item = itemFor(path);
    if (item == nullptr) {
        return;
    }
    items_.removeAt(item - items_.data());
    render();
}

// ---- relation preset ----

void UploadFileTab::applyRelationToPicker() {
    RelationPicker* picker = fileInfoCard_->queue()->picker();
    picker->setRelation(relation_);
    if (relation_ == dictation::BatchRelation::Custom) {
        picker->setCustomSummary(customInstructions_, customCombine_);
    }
}

void UploadFileTab::onRelationChanged(const QString& value) {
    const auto chosen = dictation::batch_relation_from(value.toStdString());
    if (!chosen) {
        return;
    }
    const dictation::BatchRelation previous = relation_;
    if (*chosen == dictation::BatchRelation::Custom && !editCustomDescription()) {
        fileInfoCard_->queue()->picker()->setRelation(previous);
        return;
    }
    relation_ = *chosen;
    applyRelationToPicker();
    persist(settings(), keys::transcript_batch_relation.key(), value.toStdString());
}

void UploadFileTab::onEditCustom() {
    if (editCustomDescription()) {
        applyRelationToPicker();
    }
}

// Opens the Custom dialog; true when the user kept a description.
bool UploadFileTab::editCustomDescription() {
    QStringList names;
    for (const QueueItem& item : items_) {
        names.append(item.name());
    }
    BatchRelationDialog dialog(names, customInstructions_, customCombine_, window());
    const bool accepted =
        relationDialogRunner_ ? relationDialogRunner_(dialog) : dialog.exec() != 0;
    if (!accepted) {
        return false;
    }
    customInstructions_ = dialog.instructionsText();
    customCombine_ = dialog.combineChecked();
    persist(settings(), keys::transcript_batch_custom_instructions.key(),
            customInstructions_.toStdString());
    persist(settings(), keys::transcript_batch_custom_combine.key(), customCombine_);
    return true;
}

// ---- job lifecycle ----

void UploadFileTab::transcribe() {
    if (recordingActive_ || isTranscribing() || items_.isEmpty()) {
        return;
    }
    resetResult();
    const bool withCleanup = cleanupCheck()->isChecked();
    if (items_.size() == 1) {
        if (audioPath_.isEmpty() || !exists_(audioPath_)) {
            clearFile();
            dropZone_->setNotice(QStringLiteral(u"That file no longer exists — drop it again"));
            return;
        }
        fileInfoCard_->setTranscribing(true, withCleanup);
        updateEngineLock();
        const double duration = preview_ ? preview_->duration_seconds : 0.0;
        Q_EMIT uploadRequested(audioPath_, duration);
        return;
    }

    int missing = 0;
    for (qsizetype index = items_.size() - 1; index >= 0; --index) {
        if (!exists_(items_[index].path)) {
            items_.removeAt(index);
            ++missing;
        }
    }
    if (missing > 0) {
        queueNote_ = QStringLiteral("%1 file%2 no longer exist%3 and %4 removed")
                         .arg(missing)
                         .arg(missing != 1 ? QStringLiteral("s") : QString())
                         .arg(missing == 1 ? QStringLiteral("s") : QString())
                         .arg(missing == 1 ? QStringLiteral("was") : QStringLiteral("were"));
        render();
        return;
    }

    queueNote_.clear();
    stopRequested_ = false;
    for (QueueItem& item : items_) {
        item.state = QString::fromLatin1(kRowPending);
        item.transcript.clear();
    }
    const int total = static_cast<int>(items_.size());
    fileInfoCard_->setTranscribing(true, withCleanup, total);
    pushRows();
    fileInfoCard_->queue()->setNote(QString());
    updateEngineLock();
    dictation::BatchUploadRequest request;
    for (const QueueItem& item : items_) {
        request.items.push_back(dictation::BatchItem{
            item.path.toStdString(),
            item.preview ? std::optional<double>(item.preview->duration_seconds) : std::nullopt});
    }
    request.relation = relation_;
    request.custom_instructions = customInstructions_.toStdString();
    request.custom_combine = customCombine_;
    Q_EMIT uploadFilesRequested(request);
}

void UploadFileTab::stop() {
    if (!isTranscribing()) {
        return;
    }
    // The tab stays locked until the runtime's terminal signal arrives; unlocking here would let
    // a second job start under the first.
    stopRequested_ = true;
    fileInfoCard_->progressPanel()->setStopping();
    Q_EMIT cancelRequested();
}

// Clears any Working badge once the job has ended. Batch progress marks a row active when that
// file starts, and done or failed when it finishes; Stop, and any other end that skips that
// second update, would leave the row highlighted as if work were still running.
void UploadFileTab::releaseWorkingQueueRows(bool success, bool canceled) {
    const auto stage = fileInfoCard_->progressPanel()->stage();
    const char* outcome = kRowFailed;
    if (canceled || stopRequested_ || stage == dictation::ProgressStage::Canceled) {
        outcome = kRowStopped;
    } else if (success) {
        outcome = kRowDone;
    }
    stopRequested_ = false;
    bool changed = false;
    for (QueueItem& item : items_) {
        if (item.state == QLatin1String(kRowActive)) {
            item.state = QString::fromLatin1(outcome);
            changed = true;
        }
    }
    if (changed) {
        pushRows();
    }
}

void UploadFileTab::setTranscript(const QString& text, const std::optional<QString>& raw) {
    batchResult_.reset();
    TranscriptionTabBase::setTranscript(text, raw);
    fixedText_ = text;
    rawText_ = (raw && !raw->isEmpty() && *raw != text) ? raw : std::nullopt;
    const std::string utf8 = text.toStdString();
    const bool failed = dictation::is_failed(utf8);
    const bool canceled = dictation::is_canceled(utf8);
    const bool copyable = dictation::is_copyable(utf8);
    releaseWorkingQueueRows(!failed, canceled);
    fileInfoCard_->finishTranscribing(!failed);
    updateEngineLock();
    fileInfoCard_->setCopyEnabled(copyable);
    expandButton_->setVisible(copyable);
    syncViewer();
}

void UploadFileTab::showJobError(const QString& message) {
    clearTranscription();
    const std::string utf8 = message.toStdString();
    const bool canceled = dictation::is_canceled_error(utf8);
    releaseWorkingQueueRows(false, canceled);
    fileInfoCard_->finishTranscribing(false);
    updateEngineLock();
    fileInfoCard_->setCopyEnabled(false);
    const QString reason = fromStd(dictation::job_error_reason(utf8));
    if (!reason.isEmpty() && !canceled) {
        fileInfoCard_->setErrorNote(reason);
    }
}

void UploadFileTab::setBatchResult(const dictation::BatchResult& result) {
    batchResult_ = result;
    syncViewer();
}

void UploadFileTab::clearTranscription() {
    batchResult_.reset();
    TranscriptionTabBase::clearTranscription();
    fixedText_.clear();
    rawText_.reset();
    if (expandButton_ != nullptr) {
        expandButton_->hide();
    }
    syncViewer();
}

void UploadFileTab::openTranscriptViewer() {
    if (viewer_ == nullptr) {
        viewer_ = new TranscriptViewerDialog(window());
        connect(viewer_, &TranscriptViewerDialog::copyRequested, this,
                &UploadFileTab::copyRequested);
    }
    syncViewer();
    viewer_->show();
    viewer_->raise();
    viewer_->activateWindow();
}

TranscriptViewerDialog* UploadFileTab::viewer() const {
    return viewer_;
}

void UploadFileTab::syncViewer() {
    if (viewer_ == nullptr) {
        return;
    }
    const QString title = viewerTitle();
    const QString raw = rawText_.value_or(QString());
    if (batchResult_) {
        viewer_->setBatchResult(*batchResult_, fixedText_, raw, title);
    } else {
        viewer_->setTranscript(fixedText_, raw, title);
    }
}

QString UploadFileTab::viewerTitle() const {
    if (items_.size() == 1) {
        return items_.front().name();
    }
    if (!items_.isEmpty()) {
        return QStringLiteral("%1 files").arg(items_.size());
    }
    return {};
}

void UploadFileTab::updateEngineLock() {
    const bool busy = isTranscribing() || recordingActive_;
    setBackendEnabled(!busy);
    localEngine()->setBusy(busy);
}

void UploadFileTab::setTranscriptionStats(double transcriptionTime, double audioDuration,
                                          std::optional<std::int64_t> /*fileSize*/,
                                          double cleanupTime,
                                          const std::optional<remote::RemoteTiming>& remote) {
    // Duration and size are chips already; only the time is new. A cleanup time of 0 is none.
    // A remote engine names the computer that did it and, when the host timed every request,
    // the network's share.
    std::optional<RemoteResultTiming> where;
    if (remote && !remote->host.empty()) {
        where = RemoteResultTiming{QString::fromStdString(remote->host),
                                   remote->requests > 0 ? remote->network_s() : std::nullopt};
    }
    fileInfoCard_->setResult(transcriptionTime, audioDuration,
                             cleanupTime > 0 ? std::optional<double>(cleanupTime) : std::nullopt,
                             where);
}

void UploadFileTab::clearTranscriptionStats() {
    fileInfoCard_->clearResult();
}

// The card's Copy: the Fixed text, stripped, never the Raw one.
void UploadFileTab::copyFixed() {
    const QString text = fixedText_.trimmed();
    if (!text.isEmpty()) {
        Q_EMIT copyRequested(text);
    }
}

void UploadFileTab::copyRow(const QString& path) {
    const QueueItem* item = itemFor(path);
    if (item != nullptr && !item->transcript.isEmpty()) {
        Q_EMIT copyRequested(item->transcript);
    }
}

// Drops the last job's transcript and timing once other files are up. Left showing, they read
// as the new file's result and Copy copied them. History keeps every finished transcript.
void UploadFileTab::resetResult() {
    fileInfoCard_->clearResult();
    fileInfoCard_->setCopyEnabled(false);
    clearTranscription();
}

void UploadFileTab::clearFile() {
    items_.clear();
    audioPath_.clear();
    preview_.reset();
    queueNote_.clear();
    FileInfoCard* card = fileInfoCard_;
    card->hide();
    card->setTranscribing(false);
    resetResult();
    pushRows();
    card->queue()->setNote(QString());
    card->setMode(FileInfoCard::Mode::Single);
    card->setReady(!recordingActive_);
    dropZone_->show();
    updateEngineLock();
}

void UploadFileTab::openFileBrowser() {
    const QStringList paths = browser_(dropZone_);
    if (!paths.isEmpty()) {
        addFiles(paths, 0);
    }
}

} // namespace fv
