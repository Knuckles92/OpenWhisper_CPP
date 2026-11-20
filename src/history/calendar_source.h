#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/clock.h"
#include "base/json.h"
#include "db/database.h"
#include "history/calendar.h"
#include "history/entry.h"

// services/history_calendar.py CalendarSource: reads what the History calendar shows from
// History, past meetings and the paired host. History is read here; meetings and the host's
// records come through the two interfaces below, whose null implementations list nothing.
namespace fv::history {

// One meeting.persist.repository list_past_meeting_times() row.
struct MeetingTimeRow {
    std::string id;
    std::optional<std::string> started_at;
    std::optional<std::string> ended_at;
    std::optional<double> paused_total_s;
    std::optional<std::string> origin_device_name;
};

// Past meetings as the calendar reads them (SqlMeetingRepository and meeting.content).
// Affinity: called on the calendar's reading thread.
class MeetingCalendarSource {
public:
    virtual ~MeetingCalendarSource() = default;
    // Every past meeting's id, times and paired computer, without its content.
    virtual std::vector<MeetingTimeRow> list_past_meeting_times() = 0;
    // Past meeting summaries (dicts) started in [started_from, started_before), compared as
    // stored text, at most `limit`.
    virtual std::vector<Json> list_past_meeting_summaries(int limit, std::string_view started_from,
                                                          std::string_view started_before) = 0;
    // meeting.content.fallback_meeting_title: "Untitled meeting" when nothing is stored.
    virtual std::string fallback_meeting_title(const Json& meeting) = 0;
    // meeting.content.meeting_preview_text, or "".
    virtual std::string meeting_preview_text(const Json& meeting) = 0;
    // meeting.time_utils.meeting_duration_s: start to end less pauses; nullopt when a time is
    // missing.
    virtual std::optional<double> meeting_duration_s(const Json& meeting) = 0;
};

// No meetings: this build has no meeting store.
class NullMeetingCalendarSource final : public MeetingCalendarSource {
public:
    std::vector<MeetingTimeRow> list_past_meeting_times() override { return {}; }
    std::vector<Json> list_past_meeting_summaries(int, std::string_view,
                                                  std::string_view) override {
        return {};
    }
    // Never asked: nothing is listed. A host-kept meeting shown without a meeting store reads
    // as untitled, with no preview and no length.
    std::string fallback_meeting_title(const Json&) override { return "Untitled meeting"; }
    std::string meeting_preview_text(const Json&) override { return ""; }
    std::optional<double> meeting_duration_s(const Json&) override { return std::nullopt; }
};

// The records this computer's History and meetings keep on a paired host
// (services.remote_records.sync.record_sync). Any call may throw; the source logs and carries on.
// Affinity: called on the calendar's reading thread; may wait on the network.
class RemoteRecordSource {
public:
    virtual ~RemoteRecordSource() = default;
    // Whether the host may keep records of `kind` ("dictation" or "meeting").
    virtual bool listing_wanted(std::string_view kind) = 0;
    // The host's newest records of `kind` matching `query`, at most `limit`, as dicts carrying
    // stored_on.
    virtual std::vector<Json> list_remote(std::string_view kind, std::string_view query,
                                          int limit) = 0;
    // Which of `ids` the host also keeps.
    virtual std::vector<std::string> copies_on_host(std::string_view kind,
                                                    const std::vector<std::string>& ids) = 0;
    // status().host_name: the paired host's name, or "".
    virtual std::string host_name() = 0;
    // history_manager.remote_history_entry: a host-kept dictation as an entry.
    virtual HistoryEntry remote_history_entry(const Json& item) = 0;
};

// No paired host.
class NullRemoteRecordSource final : public RemoteRecordSource {
public:
    bool listing_wanted(std::string_view) override { return false; }
    std::vector<Json> list_remote(std::string_view, std::string_view, int) override { return {}; }
    std::vector<std::string> copies_on_host(std::string_view,
                                            const std::vector<std::string>&) override {
        return {};
    }
    std::string host_name() override { return ""; }
    // Never asked: nothing is listed.
    HistoryEntry remote_history_entry(const Json&) override { return {}; }
};

// A History entry (this computer's or a host-kept one) as a calendar item; a title the entry
// was given is shown in place of its source's label.
CalendarItem entry_item(const HistoryEntry& entry, const py::DateTime& when,
                        std::string also_on = "");
// A past meeting summary (this computer's or a host-kept one) as a calendar item.
CalendarItem meeting_item(MeetingCalendarSource& meetings, const Json& meeting,
                          const py::DateTime& when, std::string also_on = "");

// Days are local to the clock's zone, read at each call.
// Affinity: blocking, never on the GUI thread; one call at a time (load_month reuses what
// load_remote listed).
class CalendarSource {
public:
    // The host lists at most this many of a kind (host_store.MAX_LIST).
    static constexpr int REMOTE_LIMIT = 500;

    // Null meetings or records mean none.
    CalendarSource(Database& database, const Clock& clock,
                   MeetingCalendarSource* meetings = nullptr,
                   RemoteRecordSource* records = nullptr);

    // Marks for every record kept on this computer; unreadable times are left out.
    std::vector<Mark> load_index();
    // Whether the paired host may keep records of this computer's.
    bool remote_wanted();
    // Marks for the records only the paired host keeps (ids in local_ids are listed once, as
    // this computer's). Sets notice() when the host could not be asked.
    std::vector<Mark> load_remote(const std::vector<std::string>& local_ids);
    // Every record of the local month, oldest first.
    std::vector<CalendarItem> load_month(int year, int month);

    // Why the host's records could not be listed, or "".
    const std::string& notice() const { return notice_; }

private:
    std::vector<CalendarItem> month_dictations(int year, int month, const std::string& start,
                                               const std::string& end);
    std::vector<CalendarItem> month_meetings(int year, int month, const std::string& start,
                                             const std::string& end);
    std::map<std::string, std::string> copies_on_host(std::string_view kind,
                                                      const std::vector<std::string>& ids);
    const std::chrono::time_zone& zone() const { return *clock_->zone(); }

    // Not owned. A null meetings or records argument points at a shared, stateless null
    // implementation, never at a member, so a copied or moved source stays valid.
    Database* database_;
    const Clock* clock_;
    MeetingCalendarSource* meetings_;
    RemoteRecordSource* records_;
    std::vector<Json> remote_dictations_;
    std::vector<Json> remote_meetings_;
    std::string notice_;
};

} // namespace fv::history
