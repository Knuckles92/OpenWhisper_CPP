#include "ui_record/upload_queue.h"

#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

#include "dictation/batch.h"
#include "ui_record/relation_picker.h"
#include "ui_theme/icons.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/eliding_label.h"

namespace fv {
namespace {

QLabel* makeChip(const char* objectName = "uploadMetaChip") {
    auto* chip = new QLabel;
    chip->setObjectName(QString::fromLatin1(objectName));
    chip->setAlignment(Qt::AlignCenter);
    return chip;
}

QPushButton* makeIconButton(const char* icon, const char* objectName, const QString& tooltip) {
    auto* button = new QPushButton;
    button->setObjectName(QString::fromLatin1(objectName));
    button->setIcon(ui::tablerIcon(QString::fromLatin1(icon)));
    button->setIconSize(QSize(14, 14));
    button->setFixedSize(26, 26);
    button->setFlat(true);
    button->setToolTip(tooltip);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}

} // namespace

QString rowStateText(const QString& state) {
    if (state == QLatin1String(kRowReading)) {
        return QStringLiteral(u"Reading…");
    }
    if (state == QLatin1String(kRowPending)) {
        return QStringLiteral("Queued");
    }
    if (state == QLatin1String(kRowActive)) {
        return QStringLiteral("Working");
    }
    if (state == QLatin1String(kRowDone)) {
        return QStringLiteral("Done");
    }
    if (state == QLatin1String(kRowFailed)) {
        return QStringLiteral("Failed");
    }
    if (state == QLatin1String(kRowStopped)) {
        return QStringLiteral("Stopped");
    }
    return state;
}

QString QueueItem::keyFor(const QString& path) {
    // os.path.abspath also normalises: "." and ".." segments and doubled separators go, which
    // absoluteFilePath() leaves in place.
    QString absolute = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#ifdef Q_OS_WIN
    // ntpath.normcase: lower case, backslashes.
    absolute = QDir::toNativeSeparators(absolute).toLower();
#endif
    return absolute;
}

QString QueueItem::name() const {
    dictation::BatchItem item;
    item.audio_path = path.toStdString();
    return QString::fromStdString(item.source_name());
}

UploadQueueRow::UploadQueueRow(const QString& path, QWidget* parent) : QFrame(parent), path_(path) {
    setObjectName(QStringLiteral("uploadQueueRow"));

    auto* layout = new QHBoxLayout(this);
    layout->setContentsMargins(10, 8, 8, 8);
    layout->setSpacing(10);

    iconLabel_ = new QLabel;
    iconLabel_->setObjectName(QStringLiteral("uploadQueueRowIcon"));
    iconLabel_->setFixedSize(30, 30);
    iconLabel_->setAlignment(Qt::AlignCenter);
    iconLabel_->setPixmap(ui::tablerPixmap(QStringLiteral("file-music-blue.svg"), 16));
    layout->addWidget(iconLabel_);

    auto* textColumn = new QVBoxLayout;
    textColumn->setContentsMargins(0, 0, 0, 0);
    textColumn->setSpacing(4);
    QueueItem named;
    named.path = path;
    nameLabel_ = new ElidingLabel(named.name());
    nameLabel_->setObjectName(QStringLiteral("uploadQueueRowName"));
    textColumn->addWidget(nameLabel_);

    auto* chips = new QHBoxLayout;
    chips->setContentsMargins(0, 0, 0, 0);
    chips->setSpacing(6);
    sizeChip_ = makeChip();
    durationChip_ = makeChip();
    chunkChip_ = makeChip("uploadChunkChip");
    for (QLabel* chip : {sizeChip_, durationChip_, chunkChip_}) {
        chips->addWidget(chip);
    }
    chips->addStretch();
    textColumn->addLayout(chips);
    layout->addLayout(textColumn, 1);

    // Copies this file's transcript alone; the card's Copy all takes the whole result. Hidden
    // until the file has finished text of its own.
    copyButton_ = makeIconButton("copy-gray.svg", "uploadRowCopyButton",
                                 QStringLiteral("Copy this file's transcript"));
    connect(copyButton_, &QPushButton::clicked, this, [this] { Q_EMIT copyClicked(path_); });
    copyButton_->hide();
    layout->addWidget(copyButton_);

    stateChip_ = new QLabel;
    stateChip_->setObjectName(QStringLiteral("uploadRowState"));
    stateChip_->setAlignment(Qt::AlignCenter);
    layout->addWidget(stateChip_);

    upButton_ =
        makeIconButton("chevron-up-gray.svg", "uploadRowMoveButton", QStringLiteral("Move up"));
    connect(upButton_, &QPushButton::clicked, this, [this] { Q_EMIT moveUpClicked(path_); });
    layout->addWidget(upButton_);

    downButton_ =
        makeIconButton("chevron-down-gray.svg", "uploadRowMoveButton", QStringLiteral("Move down"));
    connect(downButton_, &QPushButton::clicked, this, [this] { Q_EMIT moveDownClicked(path_); });
    layout->addWidget(downButton_);

    removeButton_ =
        makeIconButton("x-gray.svg", "uploadRowRemoveButton", QStringLiteral("Remove from queue"));
    connect(removeButton_, &QPushButton::clicked, this, [this] { Q_EMIT removeClicked(path_); });
    layout->addWidget(removeButton_);

    setLoading();
}

void UploadQueueRow::setTranscript(const QString& text) {
    transcript_ = text;
    copyButton_->setVisible(!transcript_.isEmpty());
}

void UploadQueueRow::setLoading() {
    sizeChip_->setText(QStringLiteral(u"Reading…"));
    durationChip_->setText(QStringLiteral(u"…"));
    chunkChip_->hide();
    setState(QString::fromLatin1(kRowReading));
}

void UploadQueueRow::setPreview(const audio::AudioFilePreview& preview) {
    sizeChip_->setText(QString::fromStdString(preview.file_size_formatted()));
    durationChip_->setText(QString::fromStdString(preview.duration_formatted()));
    chunkChip_->show();
    if (preview.needs_splitting) {
        chunkChip_->setText(QStringLiteral("About %1 chunks").arg(preview.estimated_chunks));
        ui::setStyleProperty(chunkChip_, ui::prop::tone, QStringLiteral("warn"));
    } else {
        chunkChip_->setText(QStringLiteral("One pass"));
        ui::setStyleProperty(chunkChip_, ui::prop::tone, QStringLiteral("ok"));
    }
    setToolTip(path_);
}

void UploadQueueRow::setError(const QString& message) {
    sizeChip_->setText(QStringLiteral("Could not read"));
    durationChip_->setText(QStringLiteral(u"…"));
    chunkChip_->hide();
    setToolTip(message);
    setState(QString::fromLatin1(kRowFailed));
}

void UploadQueueRow::setState(const QString& state) {
    if (state == state_) {
        return;
    }
    state_ = state;
    stateChip_->setText(rowStateText(state));
    ui::setStyleProperty(stateChip_, ui::prop::state, state);
    ui::setStyleProperty(this, ui::prop::state, state);
}

void UploadQueueRow::setPosition(int index, int count) {
    upButton_->setEnabled(index > 0);
    downButton_->setEnabled(index < count - 1);
}

void UploadQueueRow::setLocked(bool locked) {
    for (QPushButton* button : {upButton_, downButton_, removeButton_}) {
        button->setVisible(!locked);
    }
}

UploadQueueWidget::UploadQueueWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("uploadQueue"));

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(10);

    auto* header = new QHBoxLayout;
    header->setContentsMargins(0, 0, 0, 0);
    header->setSpacing(8);
    titleLabel_ = new QLabel;
    titleLabel_->setObjectName(QStringLiteral("uploadQueueTitle"));
    header->addWidget(titleLabel_);

    // What the set still needs or what the last drop left out, beside the count; there is no
    // status line under the card to say it.
    noteLabel_ = new ElidingLabel;
    noteLabel_->setObjectName(QStringLiteral("uploadQueueNote"));
    noteLabel_->hide();
    header->addWidget(noteLabel_, 1);

    addButton_ = new QPushButton(QStringLiteral(u"Add files…"));
    addButton_->setObjectName(QStringLiteral("uploadAddFilesButton"));
    addButton_->setIcon(ui::tablerIcon(QStringLiteral("plus-blue.svg")));
    addButton_->setIconSize(QSize(14, 14));
    addButton_->setFlat(true);
    addButton_->setCursor(Qt::PointingHandCursor);
    connect(addButton_, &QPushButton::clicked, this, &UploadQueueWidget::addRequested);
    header->addWidget(addButton_);

    clearButton_ = new QPushButton(QStringLiteral("Clear all"));
    clearButton_->setObjectName(QStringLiteral("uploadRemoveButton"));
    clearButton_->setFlat(true);
    clearButton_->setCursor(Qt::PointingHandCursor);
    connect(clearButton_, &QPushButton::clicked, this, &UploadQueueWidget::clearRequested);
    header->addWidget(clearButton_);
    layout->addLayout(header);

    rowsLayout_ = new QVBoxLayout;
    rowsLayout_->setContentsMargins(0, 0, 0, 0);
    rowsLayout_->setSpacing(6);
    layout->addLayout(rowsLayout_);

    picker_ = new RelationPicker;
    layout->addWidget(picker_);
}

void UploadQueueWidget::setItems(const QList<QueueItem>& items) {
    QStringList keys;
    for (const QueueItem& item : items) {
        keys.append(item.key());
    }
    if (keys != order_) {
        rebuild(items);
    }
    for (qsizetype index = 0; index < items.size(); ++index) {
        const QueueItem& item = items[index];
        UploadQueueRow* row = rows_.value(item.key());
        if (!item.error.isEmpty()) {
            row->setError(item.error);
        } else if (!item.preview) {
            row->setLoading();
        } else {
            row->setPreview(*item.preview);
            row->setState(item.state);
        }
        row->setTranscript(item.transcript);
        row->setPosition(static_cast<int>(index), static_cast<int>(items.size()));
    }
    const qsizetype count = items.size();
    titleLabel_->setText(
        QStringLiteral("%1 file%2").arg(count).arg(count != 1 ? QStringLiteral("s") : QString()));
}

void UploadQueueWidget::setRowState(const QString& path, const QString& state) {
    if (UploadQueueRow* row = rowFor(path)) {
        row->setState(state);
    }
}

void UploadQueueWidget::setRowTranscript(const QString& path, const QString& text) {
    if (UploadQueueRow* row = rowFor(path)) {
        row->setTranscript(text);
    }
}

void UploadQueueWidget::setNote(const QString& text, bool warn) {
    noteLabel_->setText(text.isEmpty() ? QString() : QStringLiteral(u"·  ") + text);
    noteLabel_->setToolTip(text);
    noteLabel_->setVisible(!text.isEmpty());
    ui::setStyleProperty(noteLabel_, ui::prop::tone, warn ? QStringLiteral("warn") : QString());
}

void UploadQueueWidget::rebuild(const QList<QueueItem>& items) {
    for (UploadQueueRow* row : std::as_const(rows_)) {
        rowsLayout_->removeWidget(row);
        row->hide();
        row->deleteLater();
    }
    rows_.clear();
    order_.clear();
    for (const QueueItem& item : items) {
        auto* row = new UploadQueueRow(item.path);
        connect(row, &UploadQueueRow::removeClicked, this, &UploadQueueWidget::removeRequested);
        connect(row, &UploadQueueRow::moveUpClicked, this,
                [this](const QString& path) { Q_EMIT moveRequested(path, -1); });
        connect(row, &UploadQueueRow::moveDownClicked, this,
                [this](const QString& path) { Q_EMIT moveRequested(path, 1); });
        connect(row, &UploadQueueRow::copyClicked, this, &UploadQueueWidget::rowCopyRequested);
        rowsLayout_->addWidget(row);
        rows_.insert(item.key(), row);
        order_.append(item.key());
    }
}

UploadQueueRow* UploadQueueWidget::rowFor(const QString& path) const {
    return rows_.value(QueueItem::keyFor(path), nullptr);
}

QList<UploadQueueRow*> UploadQueueWidget::rows() const {
    QList<UploadQueueRow*> result;
    for (const QString& key : order_) {
        result.append(rows_.value(key));
    }
    return result;
}

void UploadQueueWidget::setLocked(bool locked) {
    addButton_->setEnabled(!locked);
    clearButton_->setEnabled(!locked);
    picker_->setLocked(locked);
    for (UploadQueueRow* row : std::as_const(rows_)) {
        row->setLocked(locked);
    }
}

} // namespace fv
