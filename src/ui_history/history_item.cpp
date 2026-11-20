#include "ui_history/history_item.h"

#include <QFont>
#include <QFontMetrics>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPushButton>
#include <QSizePolicy>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "base/utf8.h"
#include "history/format_utils.h"
#include "history/manager.h"
#include "pyport/text.h"
#include "ui_history/chip_flow.h"
#include "ui_history/history_playback.h"
#include "ui_history/history_store.h"
#include "ui_history/history_text.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/eliding_label.h"
#include "ui_widgets/file_reveal.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.history_sidebar"};

// HistoryItemWidget._apply_style.
const QString kItemStyle = QStringLiteral(R"qss(
            QFrame#historyItem {
                background-color: rgba(@surface-rgb, 0.5);
                border-radius: 12px;
                border: 1px solid rgba(@overlay-rgb, 0.05);
            }
            QFrame#historyItem:hover {
                background-color: rgba(@surface-hover-rgb, 0.6);
                border: 1px solid rgba(@accent-rgb, 0.35);
            }
            QLabel#historyTimestamp {
                color: @text-secondary-strong;
                background-color: transparent;
            }
            QLabel#historyAudioChip {
                background-color: rgba(@overlay-rgb, 0.06);
                color: @text-tertiary;
                border: 1px solid rgba(@overlay-rgb, 0.08);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 500;
            }
            QLabel#historyLocationChip {
                background-color: rgba(@success-rgb, 0.10);
                color: @success-text;
                border: 1px solid rgba(@success-rgb, 0.24);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 600;
            }
            QLabel#historyModelBadge {
                background-color: rgba(@accent-rgb, 0.14);
                color: @accent-soft;
                border: 1px solid rgba(@accent-rgb, 0.25);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 600;
            }
            QLabel#historyCleanupChip {
                background-color: rgba(@purple-rgb, 0.12);
                color: @purple-text;
                border: 1px solid rgba(@purple-rgb, 0.25);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 600;
            }
            QLabel#historyOriginalChip,
            QLabel#historyAppChip {
                background-color: rgba(@overlay-rgb, 0.06);
                color: @text-secondary-strong;
                border: 1px solid rgba(@overlay-rgb, 0.10);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 600;
            }
            QLabel#historyKindChip {
                background-color: rgba(@warning-rgb, 0.12);
                color: @warning-text;
                border: 1px solid rgba(@warning-rgb, 0.28);
                border-radius: 6px;
                padding: 0px 8px;
                font-size: 10px;
                font-weight: 600;
            }
            QPushButton#historyPlayBtn {
                background-color: rgba(@overlay-rgb, 0.06);
                color: @text-body;
                border: 1px solid rgba(@overlay-rgb, 0.10);
                border-radius: 7px;
                padding: 4px 12px 4px 8px;
                font-size: 11px;
                font-weight: 600;
            }
            QPushButton#historyPlayBtn:hover {
                background-color: rgba(@accent-rgb, 0.14);
                border: 1px solid rgba(@accent-rgb, 0.35);
            }
            QPushButton#historyPlayBtn:disabled {
                color: @text-muted;
            }
            QLabel#historyPlayTime {
                color: @text-tertiary;
                background-color: transparent;
                font-size: 10px;
            }
            QLabel#historyTitle {
                color: @text;
                background-color: transparent;
            }
            QLabel#historyPreview {
                color: @text-body;
                background-color: transparent;
            }
            QPushButton#retranscribeBtn {
                background-color: rgba(@success-rgb, 0.12);
                color: @success-text-strong;
                border: 1px solid rgba(@success-rgb, 0.28);
                border-radius: 7px;
                padding: 4px 12px;
                font-size: 11px;
                font-weight: 600;
            }
            QPushButton#retranscribeBtn:hover {
                background-color: rgba(@success-rgb, 0.22);
                border: 1px solid rgba(@success-rgb, 0.45);
            }
            QPushButton#retranscribeBtn:pressed {
                background-color: rgba(@success-rgb, 0.32);
            }
        )qss");

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

QString pathText(const std::filesystem::path& path) {
    return qt::toQString(path_to_utf8(path));
}

QFont segoe(int pointSize, QFont::Weight weight = QFont::Normal) {
    return QFont(QStringLiteral("Segoe UI"), pointSize, weight);
}

} // namespace

HistoryItemWidget::HistoryItemWidget(HistoryEntry entry, HistoryStore& store, QWidget* parent)
    : QFrame(parent), reveal(&revealInFileManager), entry_(std::move(entry)) {
    failedRecording_ = entry_.entry_kind == FAILED_RECORDING_KIND;
    if (entry_was_cleaned(entry_)) {
        cleanupInfo_ = text(format_cleanup_info(entry_));
    }
    storedOnHost_ = entry_.stored_on.has_value() && !entry_.stored_on->empty();
    if (entry_.audio_file && !entry_.audio_file->empty() && !storedOnHost_) {
        audioPath_ = store.get_recording_path(*entry_.audio_file);
    }
    setObjectName(QStringLiteral("historyItem"));
    setCursor(Qt::PointingHandCursor);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &HistoryItemWidget::showContextMenu);

    setupUi();
    applyStyle();
}

HistoryItemWidget::~HistoryItemWidget() = default;

bool HistoryItemWidget::hasAudio() const {
    if (storedOnHost_) {
        return entry_.remote_audio;
    }
    return audioPath_.has_value();
}

QString HistoryItemWidget::retranscribeLabel() const {
    return failedRecording_ ? QStringLiteral("Retry transcription")
                            : QStringLiteral("Transcribe again");
}

void HistoryItemWidget::setupUi() {
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);

    auto* topRow = new QHBoxLayout;
    topRow->setSpacing(8);
    topRow->setContentsMargins(0, 0, 0, 0);

    timestampLabel_ = new QLabel(text(entry_.formatted_timestamp()));
    timestampLabel_->setObjectName(QStringLiteral("historyTimestamp"));
    timestampLabel_->setFont(segoe(10));
    timestampLabel_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    topRow->addWidget(timestampLabel_, 0, Qt::AlignVCenter);

    if (hasAudio()) {
        const bool sized = entry_.file_size.has_value() && *entry_.file_size != 0;
        audioChip_ =
            new QLabel(sized ? text(format_file_size(static_cast<double>(*entry_.file_size)))
                             : QStringLiteral("Audio"));
        audioChip_->setObjectName(QStringLiteral("historyAudioChip"));
        audioChip_->setFont(segoe(9));
        audioChip_->setAlignment(Qt::AlignCenter);
        audioChip_->setToolTip(
            QStringLiteral("Recording available — play it or transcribe it again"));
        audioChip_->setFixedHeight(20);
        topRow->addWidget(audioChip_, 0, Qt::AlignVCenter);
    }

    topRow->addStretch();

    modelBadge_ = new QLabel;
    modelBadge_->setObjectName(QStringLiteral("historyModelBadge"));
    modelBadge_->setToolTip(text(entry_.model));
    modelBadge_->setAlignment(Qt::AlignCenter);
    modelBadge_->setFixedHeight(20);
    // Elided so an unexpected model string can never widen the card beyond the sidebar's
    // fixed-width viewport.
    modelBadge_->setText(modelBadge_->fontMetrics().elidedText(
        text(format_model_name(entry_.model)), Qt::ElideRight, 120));
    modelBadge_->setMaximumWidth(140);
    topRow->addWidget(modelBadge_, 0, Qt::AlignVCenter);

    layout->addLayout(topRow);

    // What kind of entry, how AI cleanup changed it, the app it went into and where it is kept
    // share their own row (the top row is already full), wrapping when they do not fit.
    const QString kindText = text(kind_label(entry_));
    std::vector<QWidget*> chips;
    makeChips(kindText, chips);
    if (!chips.empty()) {
        chips_ = new ChipFlow(std::move(chips));
        layout->addWidget(chips_);
    }

    const std::string sourceName = py::strip(entry_.source_name.value_or(""));
    std::string title = py::strip(entry_.title.value_or(""));
    if (kindText.isEmpty() && title.empty()) {
        // A rewrite's source is its transform or "Quick Record": the kind chip says it better.
        title = sourceName;
    }
    if (!title.empty()) {
        titleLabel_ = new WrappedLabel(text(title));
        titleLabel_->setObjectName(QStringLiteral("historyTitle"));
        titleLabel_->setFont(segoe(12, QFont::DemiBold));
        titleLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
        layout->addWidget(titleLabel_);
    }

    // The preview is already cut short by preview_text, so it sizes naturally: a hard maximum
    // height clipped glyphs mid-line.
    const QString preview =
        failedRecording_ ? text(failed_recording_hint(hasAudio())) : text(entry_.preview_text());
    previewLabel_ = new WrappedLabel(preview);
    previewLabel_->setObjectName(QStringLiteral("historyPreview"));
    previewLabel_->setFont(segoe(11));
    previewLabel_->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    previewLabel_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    layout->addWidget(previewLabel_);

    if (hasAudio()) {
        addAudioFooter(layout);
    }
}

void HistoryItemWidget::makeChips(const QString& kindText, std::vector<QWidget*>& chips) {
    if (!kindText.isEmpty()) {
        kindChip_ = chip("historyKindChip", kindText);
        chips.push_back(kindChip_);
    }
    if (entry_was_cleaned(entry_)) {
        cleanupChip_ = chip("historyCleanupChip", QStringLiteral("✦ ") + cleanupInfo_);
        cleanupChip_->setToolTip(text(cleanup_tooltip(entry_)));
        chips.push_back(cleanupChip_);
    } else if (entry_version(entry_) == ORIGINAL_VERSION) {
        cleanupChip_ = chip("historyOriginalChip", QStringLiteral("Original"));
        cleanupChip_->setToolTip(QStringLiteral("AI edit undone: this is what you said"));
        chips.push_back(cleanupChip_);
    }
    const QString appName = text(py::strip(entry_.app_name.value_or("")));
    if (!appName.isEmpty()) {
        appChip_ = chip("historyAppChip", appName);
        appChip_->setToolTip(QStringLiteral("Dictated into %1").arg(appName));
        chips.push_back(appChip_);
    }
    const LocationChip location = location_chip(entry_);
    if (!location.text.empty()) {
        locationChip_ = chip("historyLocationChip", text(location.text));
        locationChip_->setToolTip(text(location.tooltip));
        chips.push_back(locationChip_);
    }
}

void HistoryItemWidget::addAudioFooter(QVBoxLayout* layout) {
    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 2, 0, 0);
    footer->setSpacing(8);

    auto* playButton = new QPushButton(QStringLiteral("Play"));
    playButton->setObjectName(QStringLiteral("historyPlayBtn"));
    playButton->setCursor(Qt::PointingHandCursor);
    playButton->setFixedHeight(28);
    playback_ = new PlaybackControl(entry_, audioPath_, playButton, this);
    footer->addWidget(playButton);
    footer->addWidget(playback_->timeLabel());
    footer->addStretch();

    retranscribeButton_ = new QPushButton(retranscribeLabel());
    retranscribeButton_->setObjectName(QStringLiteral("retranscribeBtn"));
    retranscribeButton_->setCursor(Qt::PointingHandCursor);
    retranscribeButton_->setFixedHeight(28);
    retranscribeButton_->setToolTip(QStringLiteral(
        "Run this recording through the current model using the current AI cleanup setting"));
    connect(retranscribeButton_, &QPushButton::clicked, this,
            &HistoryItemWidget::requestRetranscribe);
    footer->addWidget(retranscribeButton_);
    layout->addLayout(footer);
    layout->addWidget(playback_->progress());
}

ElidingLabel* HistoryItemWidget::chip(const char* name, const QString& chipText) {
    auto* label = new ElidingLabel(chipText);
    label->setObjectName(QString::fromLatin1(name));
    label->setFont(segoe(9));
    label->setFixedHeight(20);
    return label;
}

void HistoryItemWidget::requestRetranscribe() {
    if (!storedOnHost_) {
        if (audioPath_) {
            Q_EMIT retranscribeRequested(pathText(*audioPath_));
        }
        return;
    }
    if (!fetch_ || io_ == nullptr || retranscribeButton_ == nullptr ||
        !retranscribeButton_->isEnabled()) {
        return;
    }
    retranscribeButton_->setEnabled(false);
    retranscribeButton_->setText(
        QStringLiteral("Getting it from %1…").arg(text(entry_.stored_on.value_or(""))));
    if (!ui_) {
        ui_ = std::make_shared<qt::QtUiExecutor>(this);
    }
    // Bound here: the worker must not reach back into the card; the card's executor drops the
    // answer once the card is gone.
    io_->post([ui = ui_, fetch = fetch_, this] {
        std::filesystem::path path;
        QString error;
        try {
            path = fetch();
        } catch (const std::exception& problem) {
            kLog.warning("Could not fetch a recording from the host: {}", problem.what());
            const std::string reason = problem.what();
            error = text(reason.empty() ? "error" : reason);
        }
        ui->post([this, path = std::move(path), error = std::move(error)] {
            retranscribeButton_->setEnabled(true);
            retranscribeButton_->setText(retranscribeLabel());
            if (!error.isEmpty()) {
                retranscribeButton_->setToolTip(
                    QStringLiteral("Couldn't get the recording: %1").arg(error));
            }
            if (!path.empty()) {
                Q_EMIT retranscribeRequested(pathText(path));
            }
        });
    });
}

void HistoryItemWidget::setRemoteAudio(Executor& io, std::function<std::filesystem::path()> fetch) {
    io_ = &io;
    fetch_ = fetch;
    if (playback_ != nullptr) {
        playback_->setRemoteAudio(io, std::move(fetch));
    }
}

void HistoryItemWidget::applyStyle() {
    ui::setLocalStyleSheet(this, kItemStyle);
}

QMenu* HistoryItemWidget::buildContextMenu() {
    QMenu* menu = contextMenu(this);
    const QString id = text(entry_.id);

    if (entry_.raw_text && !entry_.raw_text->empty()) {
        connect(menu->addAction(QStringLiteral("Copy")), &QAction::triggered, this,
                [this, id] { Q_EMIT copyRequested(id); });
        connect(menu->addAction(QStringLiteral("Copy Raw")), &QAction::triggered, this,
                [this, id] { Q_EMIT copyRawRequested(id); });
    } else if (!failedRecording_) {
        connect(menu->addAction(QStringLiteral("Copy Text")), &QAction::triggered, this,
                [this, id] { Q_EMIT copyRequested(id); });
    }

    const std::string version = entry_version(entry_);
    if (!version.empty() && is_local_entry(entry_)) {
        const bool showsAi = version == AI_VERSION;
        QAction* change = menu->addAction(showsAi ? QStringLiteral("Undo AI edit")
                                                  : QStringLiteral("Use AI version"));
        const QString target = text(std::string(showsAi ? ORIGINAL_VERSION : AI_VERSION));
        connect(change, &QAction::triggered, this,
                [this, id, target] { Q_EMIT versionRequested(id, target); });
    }

    if (hasAudio()) {
        connect(menu->addAction(retranscribeLabel()), &QAction::triggered, this,
                &HistoryItemWidget::requestRetranscribe);
    }

    // Always listed so the menu keeps its shape, but live only for entries whose recording is
    // still on disk.
    QAction* showInFolderAction = menu->addAction(QStringLiteral("Show in Folder"));
    showInFolderAction->setEnabled(audioPath_.has_value());
    connect(showInFolderAction, &QAction::triggered, this, &HistoryItemWidget::showInFolder);

    menu->addSeparator();

    connect(menu->addAction(QStringLiteral("Delete")), &QAction::triggered, this,
            [this, id] { Q_EMIT deleteRequested(id); });
    return menu;
}

void HistoryItemWidget::showContextMenu(const QPoint& pos) {
    QMenu* menu = buildContextMenu();
    menu->exec(mapToGlobal(pos));
    menu->deleteLater();
}

void HistoryItemWidget::showInFolder() {
    if (!audioPath_) {
        return;
    }
    const QString path = pathText(*audioPath_);
    if (!reveal(path)) {
        kLog.warning("Could not show recording in the file manager: {}", qt::toUtf8(path));
    }
}

void HistoryItemWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        QWidget* child = childAt(event->position().toPoint());
        if (qobject_cast<QPushButton*>(child) != nullptr) {
            QFrame::mousePressEvent(event);
            return;
        }
        Q_EMIT clicked(text(entry_.id));
    }
    QFrame::mousePressEvent(event);
}

} // namespace fv
