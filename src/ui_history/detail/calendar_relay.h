#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <QMetaType>
#include <QObject>
#include <QString>

#include "history/calendar.h"

namespace fv::history_calendar_detail {

// The marks of an index read; null when the read failed.
struct Marks {
    std::shared_ptr<const std::vector<history::Mark>> marks;
};

// A month's records.
struct Items {
    std::shared_ptr<const std::vector<history::CalendarItem>> items;
};

// _CalendarRelay: brings reads and host downloads to the GUI thread for whichever calendars
// exist then. It lives for the session (never deleted), so a worker never holds or emits on a
// dialog: holding one would let the worker end it off the GUI thread, and emitting on one would
// race it closing. Qt drops a deleted dialog's connections. Emitted on the GUI thread (an inline
// io executor), the connections run at once, as Python's threaded=False does.
// Affinity: created on the GUI thread; emitted from any thread.
class CalendarRelay : public QObject {
    Q_OBJECT
public:
    static CalendarRelay& instance();
    // Generations and tokens, unique across dialogs because they share the relay.
    static std::uint64_t nextToken();

Q_SIGNALS:
    // The read's generation, the marks, the host's notice.
    void indexLoaded(quint64 generation, const fv::history_calendar_detail::Marks& marks,
                     const QString& notice);
    // The read's generation, the year and month, its records.
    void monthLoaded(quint64 generation, int year, int month,
                     const fv::history_calendar_detail::Items& items);
    // The asking dialog's token, the meeting id, the progress text.
    void fetchProgress(quint64 token, const QString& meetingId, const QString& text);
    // The asking dialog's token, the meeting id, the error ("" when it arrived).
    void fetchDone(quint64 token, const QString& meetingId, const QString& error);

private:
    CalendarRelay() = default;
};

} // namespace fv::history_calendar_detail

Q_DECLARE_METATYPE(fv::history_calendar_detail::Marks)
Q_DECLARE_METATYPE(fv::history_calendar_detail::Items)
