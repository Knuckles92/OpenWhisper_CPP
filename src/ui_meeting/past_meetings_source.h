#pragma once

#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <QString>

#include "base/json.h"
#include "meeting_state/sql_repository.h"

// What Past Meetings reads from the meeting repository: the three reads that stay on
// SqlMeetingRepository (MeetingRepository does not grow). The panel and the exporter depend on
// this port, so a test hands them a fake and the application an adapter over the real
// repository.
namespace fv {

// Affinity: as the repository behind it; every call blocks on the database, never on the GUI
// thread.
class PastMeetingsSource {
public:
    virtual ~PastMeetingsSource() = default;
    // list_past_meeting_summaries(limit=, query=): a bounded page, newest first, each row with
    // its content_summary; the query is matched by the source.
    virtual std::vector<Json> list_past_meeting_summaries(int limit, std::string_view query) = 0;
    // The content summary of one meeting (summarize_meeting_content).
    virtual Json get_meeting_content_summary(std::string_view meeting_id) = 0;
    // Every past meeting's times, without its content.
    virtual std::vector<meeting_state::PastMeetingTime> list_past_meeting_times() = 0;
};

class SqlPastMeetingsSource final : public PastMeetingsSource {
public:
    explicit SqlPastMeetingsSource(std::shared_ptr<meeting_state::SqlMeetingRepository> repository);

    std::vector<Json> list_past_meeting_summaries(int limit, std::string_view query) override;
    Json get_meeting_content_summary(std::string_view meeting_id) override;
    std::vector<meeting_state::PastMeetingTime> list_past_meeting_times() override;

private:
    std::shared_ptr<meeting_state::SqlMeetingRepository> repository_;
};

// The panel's provider over a source: (limit, query) to rows, for PastMeetingsPanel.
using PastMeetingsProvider = std::function<std::vector<Json>(int limit, const QString& query)>;
PastMeetingsProvider pastMeetingsProvider(std::shared_ptr<PastMeetingsSource> source);

} // namespace fv
