#include "ui_record/stats_display.h"

#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

#include "history/format_utils.h"
#include "ui_theme/local_style.h"

namespace fv {
namespace {

QString shown(const std::string& text) {
    return QString::fromStdString(text);
}

} // namespace

QString remoteTimingText(const std::optional<remote::RemoteTiming>& remote) {
    if (!remote || remote->host.empty()) {
        return {};
    }
    QString text = QStringLiteral("on ") + QString::fromStdString(remote->host);
    const auto network = remote->network_s();
    if (network && remote->requests > 0) {
        text += QStringLiteral(u" · %1 network").arg(shown(format_short_duration(*network)));
    }
    return text;
}

TranscriptionStatsWidget::TranscriptionStatsWidget(QWidget* parent) : QWidget(parent) {
    setObjectName(QStringLiteral("statsWidget"));

    auto* mainLayout = new QHBoxLayout(this);
    mainLayout->setContentsMargins(16, 12, 16, 12);
    mainLayout->setSpacing(24);

    mainLayout->addStretch();
    QWidget* timeItem = createStatItem(QStringLiteral("Transcription Time"), QStringLiteral("--"),
                                       transcriptionTimeValue_);
    // Which computer did the work, under the time, for a remote engine.
    remoteDetail_ = new QLabel;
    remoteDetail_->setObjectName(QStringLiteral("statsRemoteDetail"));
    remoteDetail_->setFont(QFont(QStringLiteral("Segoe UI"), 9));
    ui::setLocalStyleSheet(remoteDetail_, QStringLiteral("color: @accent;"));
    remoteDetail_->hide();
    timeItem->layout()->addWidget(remoteDetail_);
    // Top-aligned, so the labels and values stay in line and the detail hangs below the time.
    mainLayout->addWidget(timeItem, 0, Qt::AlignTop);
    mainLayout->addWidget(createSeparator());
    mainLayout->addWidget(
        createStatItem(QStringLiteral("Audio Duration"), QStringLiteral("--"), audioDurationValue_),
        0, Qt::AlignTop);
    mainLayout->addWidget(createSeparator());
    mainLayout->addWidget(
        createStatItem(QStringLiteral("File Size"), QStringLiteral("--"), fileSizeValue_), 0,
        Qt::AlignTop);
    mainLayout->addStretch();

    hide();
}

QWidget* TranscriptionStatsWidget::createStatItem(const QString& labelText,
                                                  const QString& valueText, QLabel*& valueLabel) {
    auto* widget = new QWidget;
    auto* layout = new QVBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto* label = new QLabel(labelText);
    label->setObjectName(QStringLiteral("statsLabel"));
    label->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    ui::setLocalStyleSheet(label, QStringLiteral("color: @text-secondary;"));
    layout->addWidget(label);

    auto* value = new QLabel(valueText);
    value->setObjectName(QStringLiteral("statsValue"));
    value->setFont(QFont(QStringLiteral("Segoe UI"), 13, QFont::Bold));
    ui::setLocalStyleSheet(value, QStringLiteral("color: @text;"));
    layout->addWidget(value);

    valueLabel = value;
    return widget;
}

QWidget* TranscriptionStatsWidget::createSeparator() {
    auto* separator = new QWidget;
    separator->setFixedWidth(1);
    ui::setLocalStyleSheet(separator, QStringLiteral("background-color: @border;"));
    return separator;
}

void TranscriptionStatsWidget::setStats(double transcriptionTime, double audioDuration,
                                        std::optional<std::int64_t> fileSize,
                                        double /*cleanupTime*/,
                                        const std::optional<remote::RemoteTiming>& remote) {
    transcriptionTimeValue_->setText(shown(format_audio_duration(transcriptionTime)));
    const QString detail = remoteTimingText(remote);
    remoteDetail_->setText(detail);
    remoteDetail_->setVisible(!detail.isEmpty());
    audioDurationValue_->setText(shown(format_audio_duration(audioDuration)));
    fileSizeValue_->setText(fileSize ? shown(format_file_size(static_cast<double>(*fileSize)))
                                     : QStringLiteral("--"));
    show();
    Q_EMIT visibilityChanged(true);
}

void TranscriptionStatsWidget::clear() {
    transcriptionTimeValue_->setText(QStringLiteral("--"));
    remoteDetail_->clear();
    remoteDetail_->hide();
    audioDurationValue_->setText(QStringLiteral("--"));
    fileSizeValue_->setText(QStringLiteral("--"));
    hide();
    Q_EMIT visibilityChanged(false);
}

} // namespace fv
