#include "ui_meeting/past_meeting_item.h"

#include <system_error>

#include <QAction>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "base/log.h"
#include "base/utf8.h"
#include "meeting_state/content.h"
#include "meeting_state/time_utils.h"
#include "pyport/text.h"
#include "ui_meeting/detail/payload.h"
#include "ui_meeting/past_meetings_model.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/context_menu.h"
#include "ui_widgets/file_reveal.h"

namespace fv {
namespace {

constexpr log::Logger kLog{"ui_qt.widgets.past_meetings_panel"};

QString text(const std::string& utf8) {
    return qt::toQString(utf8);
}

// The pills are tinted by a property no rule of the theme names, so it is set by name.
constexpr const char* kPillTone = "pillTone";

QLabel* makePill(QWidget* parent) {
    auto* pill = new QLabel(QString(), parent);
    pill->setObjectName(QStringLiteral("pastMeetingInsightsPill"));
    pill->setAlignment(Qt::AlignCenter);
    pill->setFont(QFont(QStringLiteral("Segoe UI"), 9, QFont::DemiBold));
    pill->setFixedHeight(20);
    return pill;
}

} // namespace

PastMeetingItem::PastMeetingItem(const Json& meeting, const Clock& clock, QWidget* parent)
    : QFrame(parent), openFolder(&openFolderInFileManager), meeting_(meeting) {
    using ui_meeting::detail::get;
    using ui_meeting::detail::strOrEmpty;
    meetingId_ = text(strOrEmpty(get(meeting, "id")));
    storedOn_ = text(strOrEmpty(get(meeting, "stored_on")));
    setObjectName(QStringLiteral("pastMeetingItem"));
    setCursor(Qt::PointingHandCursor);
    ui::setStyleProperty(this, ui::prop::selected, false);
    setContextMenuPolicy(Qt::CustomContextMenu);
    connect(this, &QWidget::customContextMenuRequested, this, &PastMeetingItem::showContextMenu);

    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(14, 12, 14, 12);
    layout->setSpacing(10);

    titleLabel_ = new QLabel(text(meeting_state::fallback_meeting_title(meeting)), this);
    titleLabel_->setObjectName(QStringLiteral("pastMeetingTitle"));
    titleLabel_->setWordWrap(true);
    titleLabel_->setFont(QFont(QStringLiteral("Segoe UI"), 12, QFont::DemiBold));
    layout->addWidget(titleLabel_);

    dateLabel_ = new QLabel(
        text(meeting_state::format_meeting_started_at(get(meeting, "started_at"), clock)), this);
    dateLabel_->setObjectName(QStringLiteral("pastMeetingMeta"));
    dateLabel_->setFont(QFont(QStringLiteral("Segoe UI"), 10));
    layout->addWidget(dateLabel_);

    const std::string preview = meeting_state::meeting_preview_text(meeting);
    previewLabel_ = new QLabel(text(preview), this);
    previewLabel_->setObjectName(QStringLiteral("pastMeetingPreview"));
    previewLabel_->setWordWrap(true);
    previewLabel_->setFont(QFont(QStringLiteral("Segoe UI"), 11));
    layout->addWidget(previewLabel_);
    previewLabel_->setVisible(!preview.empty());

    const std::string note = past_meetings::content_note(meeting);
    contentLabel_ = new QLabel(text(note), this);
    contentLabel_->setObjectName(QStringLiteral("pastMeetingContentWarning"));
    contentLabel_->setWordWrap(true);
    layout->addWidget(contentLabel_);
    contentLabel_->setVisible(!note.empty());

    auto* footer = new QHBoxLayout;
    footer->setContentsMargins(0, 2, 0, 0);
    footer->setSpacing(8);

    detailLabel_ = new QLabel(text(past_meetings::detail_text(meeting, clock)), this);
    detailLabel_->setObjectName(QStringLiteral("pastMeetingMeta"));
    footer->addWidget(detailLabel_);

    insightsPill_ = makePill(this);
    footer->addWidget(insightsPill_);
    if (const auto pill = meeting_state::meeting_insights_pill(meeting)) {
        insightsPill_->setText(text(pill->first));
        ui::setStyleProperty(insightsPill_, kPillTone, text(pill->second));
        insightsPill_->show();
    } else {
        insightsPill_->hide();
    }

    const auto [location, tip] = past_meetings::meeting_location(meeting);
    locationPill_ = makePill(this);
    locationPill_->setText(text(location));
    ui::setStyleProperty(locationPill_, kPillTone, QStringLiteral("location"));
    locationPill_->setToolTip(text(tip));
    locationPill_->setVisible(!location.empty());
    footer->addWidget(locationPill_);
    footer->addStretch();
    layout->addLayout(footer);
}

void PastMeetingItem::setProgress(const QString& progress) {
    if (!progress.isEmpty()) {
        contentLabel_->setText(progress);
        contentLabel_->show();
    } else {
        contentLabel_->hide();
    }
}

void PastMeetingItem::setSelected(bool selected) {
    ui::setStyleProperty(this, ui::prop::selected, selected);
}

std::filesystem::path PastMeetingItem::spoolDir() const {
    using ui_meeting::detail::get;
    using ui_meeting::detail::strOrEmpty;
    const std::string spool = py::strip(strOrEmpty(get(meeting_, "spool_dir")));
    if (spool.empty()) {
        return {};
    }
    std::filesystem::path folder =
        std::filesystem::absolute(path_from_utf8(spool)).lexically_normal();
    if (!folder.has_filename()) {
        folder = folder.parent_path();
    }
    std::error_code error;
    return std::filesystem::is_directory(folder, error) && !error ? folder
                                                                  : std::filesystem::path();
}

void PastMeetingItem::showInFolder() {
    const std::filesystem::path folder = spoolDir();
    if (folder.empty()) {
        return;
    }
    const std::string utf8 = path_to_utf8(folder);
    if (!openFolder(text(utf8))) {
        kLog.warning("Could not show meeting recordings in the file manager: {}", utf8);
    }
}

QMenu* PastMeetingItem::buildContextMenu() {
    using ui_meeting::detail::get;
    using ui_meeting::detail::strOrEmpty;
    QMenu* menu = contextMenu(this);
    if (!storedOn_.isEmpty()) {
        connect(menu->addAction(QStringLiteral("Open (from %1)").arg(storedOn_)),
                &QAction::triggered, this, &PastMeetingItem::emitSelected);
        menu->addSeparator();
        connect(menu->addAction(QStringLiteral("Delete from %1").arg(storedOn_)),
                &QAction::triggered, this, [this] { Q_EMIT deleteRequested(meetingId_); });
        return menu;
    }
    QAction* copy = menu->addAction(QStringLiteral("Copy transcript"));
    copy->setEnabled(past_meetings::has_transcript(meeting_));
    connect(copy, &QAction::triggered, this,
            [this] { Q_EMIT copyTranscriptRequested(meetingId_); });

    // Always listed so the menu keeps its shape, but only live while this meeting's recordings
    // are still spooled on disk.
    if (speakerUploadAvailable_ && meeting_.value("status", "") == "ended" &&
        past_meetings::has_transcript(meeting_) && !spoolDir().empty()) {
        auto* speakers = menu->addAction(QStringLiteral("Identify speakers with OpenAI…"));
        speakers->setObjectName(QStringLiteral("identifySavedSpeakers"));
        connect(speakers, &QAction::triggered, this,
                [this] { Q_EMIT speakerUploadRequested(meetingId_); });
    }
    QAction* showInFolderAction = menu->addAction(QStringLiteral("Show in Folder"));
    showInFolderAction->setEnabled(!spoolDir().empty());
    connect(showInFolderAction, &QAction::triggered, this, &PastMeetingItem::showInFolder);

    menu->addSeparator();
    const auto host = text(strOrEmpty(get(meeting_, "_host_copy_name")));
    const auto fingerprint = text(strOrEmpty(get(meeting_, "_host_copy_fingerprint")));
    const auto revision = text(strOrEmpty(get(meeting_, "_host_copy_revision")));
    if (!host.isEmpty() && !fingerprint.isEmpty() && !revision.isEmpty()) {
        const auto title = text(strOrEmpty(get(meeting_, "_host_copy_title")));
        connect(menu->addAction(QStringLiteral("Delete copy on %1…").arg(host)),
                &QAction::triggered, this, [this, host, title, fingerprint, revision] {
                    Q_EMIT hostCopyDeleteRequested(meetingId_, host, title, fingerprint, revision);
                });
    }
    connect(menu->addAction(QStringLiteral("Delete")), &QAction::triggered, this,
            [this] { Q_EMIT deleteRequested(meetingId_); });
    return menu;
}

void PastMeetingItem::showContextMenu(const QPoint& pos) {
    QMenu* menu = buildContextMenu();
    menu->exec(mapToGlobal(pos));
    menu->deleteLater();
}

void PastMeetingItem::emitSelected() {
    if (!meetingId_.isEmpty()) {
        Q_EMIT meetingSelected(meetingId_);
    }
}

void PastMeetingItem::mousePressEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton) {
        emitSelected();
    }
    QFrame::mousePressEvent(event);
}

} // namespace fv
