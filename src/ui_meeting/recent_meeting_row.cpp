#include "ui_meeting/recent_meeting_row.h"

#include <algorithm>
#include <string>

#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QVBoxLayout>

#include "appkit/qt_bridge.h"
#include "meeting_state/content.h"
#include "meeting_state/sql_repository.h"
#include "meeting_state/time_utils.h"
#include "pyport/text.h"
#include "ui_meeting/detail/payload.h"
#include "ui_theme/style_props.h"
#include "ui_widgets/eliding_label.h"

namespace fv {

namespace {

// Meetings still being captured are not history yet.
bool isLive(const Json& meeting) {
    const std::string status =
        py::lower(ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(meeting, "status")));
    return status == "active" || status == "paused" || status == "ending";
}

} // namespace

std::vector<Json> finishedRecentMeetings(const std::vector<Json>& rows) {
    std::vector<Json> finished;
    for (const Json& row : rows) {
        if (!isLive(row)) {
            finished.push_back(row);
        }
    }
    if (finished.size() > static_cast<std::size_t>(kRecentMeetingsShown)) {
        finished.resize(kRecentMeetingsShown);
    }
    return finished;
}

std::vector<Json> readRecentMeetings(meeting_state::SqlMeetingRepository& repository) {
    meeting_state::PastMeetingQuery query;
    query.limit = kRecentMeetingsShown + 3;
    query.query = "";
    return finishedRecentMeetings(repository.list_past_meeting_summaries(query));
}

RecentMeetingRow::RecentMeetingRow(const Json& meeting, const Clock& clock, QWidget* parent)
    : QFrame(parent), meetingId_(qt::toQString(
                          ui_meeting::detail::strOrEmpty(ui_meeting::detail::get(meeting, "id")))) {
    setObjectName(QStringLiteral("meetingRecent"));
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(10, 8, 12, 8);
    row->setSpacing(12);
    auto* words = new QVBoxLayout();
    words->setSpacing(2);
    const QString title = qt::toQString(meeting_state::fallback_meeting_title(meeting));
    titleLabel_ = new ElidingLabel(title);
    titleLabel_->setObjectName(QStringLiteral("meetingTitle"));
    words->addWidget(titleLabel_);
    QStringList parts;
    for (const std::string& part : {meeting_state::format_meeting_started_at(
                                        ui_meeting::detail::get(meeting, "started_at"), clock),
                                    meeting_state::format_meeting_duration(meeting, clock)}) {
        if (!part.empty()) {
            parts.push_back(qt::toQString(part));
        }
    }
    const QString meta = parts.join(QStringLiteral(" · "));
    metaLabel_ = new QLabel(meta);
    metaLabel_->setObjectName(QStringLiteral("meetingHint"));
    words->addWidget(metaLabel_);
    row->addLayout(words, 1);
    if (const auto pill = meeting_state::meeting_insights_pill(meeting)) {
        pill_ = new QLabel(qt::toQString(pill->first));
        pill_->setObjectName(QStringLiteral("meetingRecentPill"));
        ui::setStyleProperty(pill_, ui::prop::tone, qt::toQString(pill->second));
        row->addWidget(pill_, 0, Qt::AlignVCenter);
    }
    setAccessibleName(QStringLiteral("Open %1. %2").arg(title, meta));
}

void RecentMeetingRow::mouseReleaseEvent(QMouseEvent* event) {
    if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) {
        Q_EMIT clicked(meetingId_);
    }
    QFrame::mouseReleaseEvent(event);
}

void RecentMeetingRow::keyPressEvent(QKeyEvent* event) {
    const int key = event->key();
    if (key == Qt::Key_Return || key == Qt::Key_Enter || key == Qt::Key_Space) {
        Q_EMIT clicked(meetingId_);
        return;
    }
    QFrame::keyPressEvent(event);
}

} // namespace fv
