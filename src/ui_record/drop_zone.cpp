#include "ui_record/drop_zone.h"

#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFont>
#include <QLabel>
#include <QMimeData>
#include <QMouseEvent>
#include <QUrl>
#include <QVBoxLayout>

#include "ui_theme/icons.h"
#include "ui_theme/local_style.h"
#include "ui_widgets/wrapped_label.h"

namespace fv {
namespace {

// The icon rule repeats the ancestor so it outranks the blanket label rule; on its own,
// QLabel#dropZoneIcon is the less specific selector.
constexpr const char* kChildStyle = R"(
        QFrame#dropZone QLabel {
            background-color: transparent;
            border: none;
        }
        QFrame#dropZone QLabel#dropZoneIcon {
            background-color: rgba(@accent-rgb, 0.14);
            border-radius: 16px;
        }
    )";
constexpr const char* kIdleStyle = R"(
        QFrame#dropZone {
            background-color: rgba(@overlay-rgb, 0.025);
            border: 2px dashed @border-strong;
            border-radius: 16px;
        }
        QFrame#dropZone:hover {
            border-color: @accent;
            background-color: rgba(@accent-rgb, 0.06);
        }
    )";
constexpr const char* kHoverStyle = R"(
        QFrame#dropZone {
            background-color: rgba(@accent-rgb, 0.12);
            border: 2px solid @accent;
            border-radius: 16px;
        }
        QFrame#dropZone QLabel#dropZoneIcon {
            background-color: rgba(@accent-rgb, 0.28);
        }
    )";
constexpr const char* kRejectStyle = R"(
        QFrame#dropZone {
            background-color: rgba(@danger-rgb, 0.10);
            border: 2px dashed @danger;
            border-radius: 16px;
        }
    )";

QString idleStyle() {
    return QString::fromUtf8(kIdleStyle) + QString::fromUtf8(kChildStyle);
}
QString hoverStyle() {
    return QString::fromUtf8(kHoverStyle) + QString::fromUtf8(kChildStyle);
}
QString rejectStyle() {
    return QString::fromUtf8(kRejectStyle) + QString::fromUtf8(kChildStyle);
}

QLabel* centeredLabel(const QString& text, int pointSize, QFont::Weight weight,
                      const char* colorToken) {
    auto* label = new QLabel(text);
    label->setFont(QFont(QStringLiteral("Segoe UI"), pointSize, weight));
    ui::setLocalStyleSheet(label, QStringLiteral("color: %1;").arg(QLatin1String(colorToken)));
    label->setAlignment(Qt::AlignCenter);
    return label;
}

} // namespace

QString audioFileFilters() {
    return QStringLiteral("Audio Files (*.wav *.mp3 *.m4a *.ogg *.flac *.wma);;"
                          "WAV Files (*.wav);;MP3 Files (*.mp3);;All Files (*.*)");
}

QString noAudioDroppedMessage() {
    return QStringLiteral("Only audio files can be transcribed (WAV, MP3, M4A, OGG, FLAC, WMA).");
}

bool isSupportedAudio(const QString& path) {
    for (const char* extension : kSupportedAudioExtensions) {
        if (path.endsWith(QLatin1String(extension), Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

DroppedAudio audioPathsFromMime(const QMimeData* mime) {
    DroppedAudio result;
    if (mime == nullptr || !mime->hasUrls()) {
        return result;
    }
    for (const QUrl& url : mime->urls()) {
        const QString path = url.toLocalFile();
        if (!path.isEmpty() && isSupportedAudio(path)) {
            result.paths.append(path);
        } else {
            ++result.skipped;
        }
    }
    return result;
}

QStringList browseForAudioFiles(QWidget* parent) {
    return QFileDialog::getOpenFileNames(parent, QStringLiteral("Select Audio Files"), QString(),
                                         audioFileFilters());
}

DropZoneWidget::DropZoneWidget(QWidget* parent) : QFrame(parent) {
    setObjectName(QStringLiteral("dropZone"));
    setAcceptDrops(true);
    setCursor(Qt::PointingHandCursor);
    setMinimumHeight(150);
    ui::setLocalStyleSheet(this, idleStyle());

    // Stretches center the content rather than layout alignment: an aligned layout gives a
    // wrapped label its height for a wider width than it lays the label out at, which cut the
    // notice's first and last lines.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(4);
    layout->addStretch(1);

    auto* iconLabel = new QLabel;
    iconLabel->setObjectName(QStringLiteral("dropZoneIcon"));
    iconLabel->setFixedSize(52, 52);
    iconLabel->setAlignment(Qt::AlignCenter);
    iconLabel->setPixmap(ui::tablerPixmap(QStringLiteral("cloud-upload-blue.svg"), 26));
    layout->addWidget(iconLabel, 0, Qt::AlignHCenter);

    layout->addSpacing(8);
    layout->addWidget(
        centeredLabel(QStringLiteral("Drop audio files here"), 13, QFont::DemiBold, "@text"));
    layout->addWidget(
        centeredLabel(QStringLiteral(u"or click to browse  ·  several files become a queue"), 11,
                      QFont::Normal, "@text-secondary"));
    layout->addSpacing(8);
    layout->addWidget(centeredLabel(QStringLiteral(u"WAV  ·  MP3  ·  M4A  ·  OGG  ·  FLAC  ·  WMA"),
                                    10, QFont::Normal, "@text-muted"));

    notice_ = new WrappedLabel(QString(), nullptr, true);
    notice_->setObjectName(QStringLiteral("dropZoneNotice"));
    notice_->setTextFormat(Qt::PlainText);
    notice_->setFont(QFont(QStringLiteral("Segoe UI"), 11));
    ui::setLocalStyleSheet(notice_, QStringLiteral("color: @danger-text;"));
    notice_->setAlignment(Qt::AlignHCenter | Qt::AlignTop);
    notice_->hide();
    layout->addSpacing(4);
    layout->addWidget(notice_);
    layout->addStretch(1);
}

void DropZoneWidget::setNotice(const QString& text) {
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
}

void DropZoneWidget::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) {
        if (!audioPathsFromMime(event->mimeData()).paths.isEmpty()) {
            event->acceptProposedAction();
            ui::setLocalStyleSheet(this, hoverStyle());
            return;
        }
        ui::setLocalStyleSheet(this, rejectStyle());
    }
    event->ignore();
}

void DropZoneWidget::dragLeaveEvent(QDragLeaveEvent*) {
    ui::setLocalStyleSheet(this, idleStyle());
}

void DropZoneWidget::dropEvent(QDropEvent* event) {
    ui::setLocalStyleSheet(this, idleStyle());
    const DroppedAudio dropped = audioPathsFromMime(event->mimeData());
    if (!dropped.paths.isEmpty()) {
        event->acceptProposedAction();
        Q_EMIT filesSelected(dropped.paths, dropped.skipped);
        return;
    }
    if (dropped.skipped > 0) {
        setNotice(noAudioDroppedMessage());
    }
    event->ignore();
}

void DropZoneWidget::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        Q_EMIT browseRequested();
    }
}

} // namespace fv
