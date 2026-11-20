#pragma once

#include <vector>

#include <QFrame>
#include <QString>

#include "base/clock.h"
#include "base/json.h"

class QLabel;

namespace fv {

class ElidingLabel;

namespace meeting_state {
class SqlMeetingRepository;
}

// How many meetings the empty stage offers to open.
inline constexpr int kRecentMeetingsShown = 3;

// read_recent_meetings: the newest finished meetings kept on this computer, newest first, at
// most kRecentMeetingsShown. A few spare rows are read in case the newest are still being
// captured (active, paused and ending are not history yet). Blocks on the database: call it off
// the GUI thread.
std::vector<Json> readRecentMeetings(meeting_state::SqlMeetingRepository& repository);
// The filter of readRecentMeetings over rows already read.
std::vector<Json> finishedRecentMeetings(const std::vector<Json>& rows);

// One meeting on the empty stage (objectName meetingRecent): its title, "date · duration" and
// insights pill. A click, Enter, Return or Space emits clicked with the meeting's id.
// Affinity: GUI thread.
class RecentMeetingRow : public QFrame {
    Q_OBJECT
public:
    explicit RecentMeetingRow(const Json& meeting, const Clock& clock = system_clock(),
                              QWidget* parent = nullptr);

    const QString& meetingId() const { return meetingId_; }
    ElidingLabel* titleLabel() const { return titleLabel_; }
    QLabel* metaLabel() const { return metaLabel_; }
    // Null when the meeting has no pill.
    QLabel* pill() const { return pill_; }

Q_SIGNALS:
    void clicked(const QString& meetingId);

protected:
    void mouseReleaseEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    QString meetingId_;
    ElidingLabel* titleLabel_;
    QLabel* metaLabel_;
    QLabel* pill_ = nullptr;
};

} // namespace fv
