#include "ui_record/file_info_card.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

#include "history/format_utils.h"
#include "ui_record/drop_zone.h"
#include "ui_record/transcription_progress.h"
#include "ui_record/upload_queue.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/buttons.h"
#include "ui_widgets/decode_label.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

QLabel* makeChip(const char* objectName = "uploadMetaChip") {
    auto* chip = new QLabel;
    chip->setObjectName(QString::fromLatin1(objectName));
    chip->setAlignment(Qt::AlignCenter);
    return chip;
}

QString fromStd(const std::string& text) {
    return QString::fromStdString(text);
}

} // namespace

FileInfoCard::FileInfoCard(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("uploadFileCard"));
    setAcceptDrops(true);

    settleTimer_ = new QTimer(this);
    settleTimer_->setSingleShot(true);
    settleTimer_->setInterval(kResultHoldMs);
    connect(settleTimer_, &QTimer::timeout, this, [this] { showActions(); });

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(18, 16, 18, 16);
    layout->setSpacing(14);

    headerRow_ = new QWidget;
    headerRow_->setObjectName(QStringLiteral("uploadFileHeader"));
    auto* header = new QHBoxLayout(headerRow_);
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(14);

    iconLabel_ = new QLabel;
    iconLabel_->setObjectName(QStringLiteral("uploadFileIcon"));
    iconLabel_->setFixedSize(44, 44);
    iconLabel_->setAlignment(Qt::AlignCenter);
    iconLabel_->setPixmap(ui::tablerPixmap(QStringLiteral("file-music-blue.svg"), 24));
    header->addWidget(iconLabel_, 0, Qt::AlignTop);

    auto* textColumn = new QVBoxLayout;
    textColumn->setContentsMargins(0, 2, 0, 0);
    textColumn->setSpacing(7);

    filenameLabel_ = new ElidingLabel;
    filenameLabel_->setObjectName(QStringLiteral("uploadFileName"));
    textColumn->addWidget(filenameLabel_);

    auto* chips = new QHBoxLayout;
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(6);
    sizeChip_ = makeChip();
    durationChip_ = makeChip();
    rateChip_ = makeChip();
    channelsChip_ = makeChip();
    chunkLabel_ = makeChip("uploadChunkChip");
    for (QLabel* chip : {sizeChip_, durationChip_, rateChip_, channelsChip_, chunkLabel_}) {
        chips->addWidget(chip);
    }
    chips->addStretch();
    textColumn->addLayout(chips);
    header->addLayout(textColumn, 1);

    removeButton_ = new QPushButton(QStringLiteral("Remove"));
    removeButton_->setObjectName(QStringLiteral("uploadRemoveButton"));
    removeButton_->setFlat(true);
    removeButton_->setCursor(Qt::PointingHandCursor);
    connect(removeButton_, &QPushButton::clicked, this, &FileInfoCard::removeClicked);
    header->addWidget(removeButton_, 0, Qt::AlignTop);
    layout->addWidget(headerRow_);

    queue_ = new UploadQueueWidget;
    queue_->hide();
    layout->addWidget(queue_);

    auto* divider = new QFrame;
    divider->setObjectName(QStringLiteral("uploadCardDivider"));
    divider->setFixedHeight(1);
    layout->addWidget(divider);

    footer_ = new QStackedWidget;
    footer_->setObjectName(QStringLiteral("uploadFooter"));

    actionsPage_ = new QWidget;
    actionsPage_->setObjectName(QStringLiteral("uploadActions"));
    auto* actions = new QHBoxLayout(actionsPage_);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(12);

    copyButton_ = new Button(QStringLiteral("Copy"));
    copyButton_->setObjectName(QStringLiteral("uploadCopyButton"));
    copyButton_->setBaseMinimumSize(96, 42);
    copyButton_->setActive(false);
    connect(copyButton_, &QPushButton::clicked, this, &FileInfoCard::copyClicked);
    actions->addWidget(copyButton_);

    actions->addStretch();
    // Duration and size already sit in the chips, so the only new fact a finished job brings is
    // how long it took; it lives between the actions rather than in the stats strip the Quick
    // Record tab uses.
    resultLabel_ = new DecodeLabel;
    resultLabel_->setObjectName(QStringLiteral("uploadResultNote"));
    resultLabel_->setAlignment(Qt::AlignCenter);
    resultLabel_->setWordWrap(true);
    resultLabel_->hide();
    actions->addWidget(resultLabel_);
    actions->addStretch();

    transcribeButton_ = new PrimaryButton(QStringLiteral("Transcribe"));
    transcribeButton_->setBaseMinimumSize(150, 44);
    connect(transcribeButton_, &QPushButton::clicked, this, &FileInfoCard::transcribeClicked);
    actions->addWidget(transcribeButton_);
    footer_->addWidget(actionsPage_);

    progress_ = new TranscriptionProgressPanel;
    connect(progress_, &TranscriptionProgressPanel::stopClicked, this, &FileInfoCard::stopClicked);
    footer_->addWidget(progress_);

    layout->addWidget(footer_);
}

bool FileInfoCard::isProgressShown() const {
    return footer_->currentWidget() == progress_;
}

void FileInfoCard::dragEnterEvent(QDragEnterEvent* event) {
    if (transcribing_ || !event->mimeData()->hasUrls()) {
        event->ignore();
        return;
    }
    if (!audioPathsFromMime(event->mimeData()).paths.isEmpty()) {
        event->acceptProposedAction();
    } else {
        event->ignore();
    }
}

void FileInfoCard::dropEvent(QDropEvent* event) {
    const DroppedAudio dropped = audioPathsFromMime(event->mimeData());
    if (!dropped.paths.isEmpty() && !transcribing_) {
        event->acceptProposedAction();
        Q_EMIT filesDropped(dropped.paths, dropped.skipped);
        return;
    }
    event->ignore();
}

void FileInfoCard::setMode(Mode mode) {
    mode_ = mode;
    headerRow_->setVisible(mode == Mode::Single);
    queue_->setVisible(mode == Mode::Queue);
    copyButton_->setText(mode == Mode::Queue ? QStringLiteral("Copy all") : QStringLiteral("Copy"));
}

void FileInfoCard::setPreview(const audio::AudioFilePreview& preview) {
    filenameLabel_->setText(fromStd(preview.file_name));
    sizeChip_->setText(fromStd(preview.file_size_formatted()));
    durationChip_->setText(fromStd(preview.duration_formatted()));
    rateChip_->setText(fromStd(format_sample_rate(preview.sample_rate)));
    if (preview.channels == 1) {
        channelsChip_->setText(QStringLiteral("Mono"));
    } else if (preview.channels == 2) {
        channelsChip_->setText(QStringLiteral("Stereo"));
    } else {
        channelsChip_->setText(QStringLiteral("%1 ch").arg(preview.channels));
    }

    if (preview.needs_splitting) {
        chunkLabel_->setText(QStringLiteral("About %1 chunks").arg(preview.estimated_chunks));
        ui::setStyleProperty(chunkLabel_, ui::prop::tone, QStringLiteral("warn"));
    } else {
        chunkLabel_->setText(QStringLiteral("One pass"));
        ui::setStyleProperty(chunkLabel_, ui::prop::tone, QStringLiteral("ok"));
    }
}

void FileInfoCard::setLoading(const QString& fileName) {
    filenameLabel_->setText(fileName);
    for (QLabel* chip : {sizeChip_, durationChip_, rateChip_, channelsChip_}) {
        chip->setText(QStringLiteral(u"…"));
    }
    chunkLabel_->setText(QStringLiteral(u"Reading…"));
    ui::setStyleProperty(chunkLabel_, ui::prop::tone, QString());
}

void FileInfoCard::setReady(bool ready) {
    ready_ = ready;
    applyTranscribeEnabled();
}

void FileInfoCard::applyTranscribeEnabled() {
    transcribeButton_->setEnabled(ready_ && !transcribing_);
}

void FileInfoCard::setTranscribing(bool active, bool withCleanup, int totalFiles) {
    settleTimer_->stop();
    transcribing_ = active;
    applyTranscribeEnabled();
    removeButton_->setEnabled(!active);
    queue_->setLocked(active);
    if (active) {
        copyButton_->setActive(false);
        clearResult();
        progress_->start(withCleanup, totalFiles);
        showProgress();
    } else {
        showActions();
    }
}

void FileInfoCard::finishTranscribing(bool success) {
    const bool wasRunning = transcribing_;
    transcribing_ = false;
    applyTranscribeEnabled();
    removeButton_->setEnabled(true);
    queue_->setLocked(false);
    if (wasRunning) {
        if (progress_->stage() == dictation::ProgressStage::Canceled) {
            progress_->setStopEnabled(false);
        } else {
            progress_->finish(success);
        }
        settleTimer_->start();
    } else if (isProgressShown()) {
        showActions();
    }
}

void FileInfoCard::setCopyEnabled(bool enabled) {
    copyButton_->setActive(enabled);
}

void FileInfoCard::setResult(double transcriptionTime, double audioDuration,
                             std::optional<double> cleanupTime,
                             const std::optional<RemoteResultTiming>& remote, bool animate) {
    const QString separator = QStringLiteral(u"  ·  ");
    DecodeSegments segments{{QStringLiteral("Transcribed in "), false},
                            {fromStd(format_audio_duration(transcriptionTime)), true}};
    if (remote && !remote->host.isEmpty()) {
        segments.append({QStringLiteral(" on ") + remote->host, false});
    }
    if (transcriptionTime > 0 && audioDuration > 0) {
        const double speed = audioDuration / transcriptionTime;
        segments.append({separator, false});
        segments.append({QString::number(speed, 'f', 1) + QStringLiteral(u"×"), true});
        segments.append({QStringLiteral(" realtime"), false});
    }
    if (remote && remote->networkSeconds) {
        segments.append({separator, false});
        segments.append({fromStd(format_short_duration(*remote->networkSeconds)), true});
        segments.append({QStringLiteral(" network"), false});
    }
    if (cleanupTime && *cleanupTime > 0) {
        segments.append({separator, false});
        segments.append({QStringLiteral("Cleaned in "), false});
        segments.append({fromStd(format_audio_duration(*cleanupTime)), true});
    }
    // The reveal runs when the action row next comes into view, which is after the progress
    // panel has held its Done state.
    ui::setStyleProperty(resultLabel_, ui::prop::tone, QString());
    resultLabel_->setToolTip(QString());
    resultLabel_->setSegments(segments, animate);
    resultLabel_->show();
}

void FileInfoCard::setErrorNote(const QString& message) {
    ui::setStyleProperty(resultLabel_, ui::prop::tone, QStringLiteral("danger"));
    resultLabel_->setToolTip(message);
    resultLabel_->setSegments({{message, false}}, false);
    resultLabel_->show();
}

void FileInfoCard::clearResult() {
    ui::setStyleProperty(resultLabel_, ui::prop::tone, QString());
    resultLabel_->setToolTip(QString());
    resultLabel_->clear();
    resultLabel_->hide();
}

void FileInfoCard::showProgress() {
    if (!isProgressShown()) {
        footer_->setCurrentWidget(progress_);
        Q_EMIT progressShown(true);
    }
}

void FileInfoCard::showActions() {
    if (isProgressShown()) {
        footer_->setCurrentWidget(actionsPage_);
        Q_EMIT progressShown(false);
    }
}

} // namespace fv
