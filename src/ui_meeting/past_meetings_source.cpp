#include "ui_meeting/past_meetings_source.h"

#include <utility>

#include "appkit/qt_bridge.h"

namespace fv {

SqlPastMeetingsSource::SqlPastMeetingsSource(
    std::shared_ptr<meeting_state::SqlMeetingRepository> repository)
    : repository_(std::move(repository)) {}

std::vector<Json> SqlPastMeetingsSource::list_past_meeting_summaries(int limit,
                                                                     std::string_view query) {
    meeting_state::PastMeetingQuery request;
    request.limit = limit;
    request.query = std::string(query);
    return repository_->list_past_meeting_summaries(request);
}

Json SqlPastMeetingsSource::get_meeting_content_summary(std::string_view meeting_id) {
    return repository_->get_meeting_content_summary(meeting_id);
}

std::vector<meeting_state::PastMeetingTime> SqlPastMeetingsSource::list_past_meeting_times() {
    return repository_->list_past_meeting_times();
}

PastMeetingsProvider pastMeetingsProvider(std::shared_ptr<PastMeetingsSource> source) {
    return [source = std::move(source)](int limit, const QString& query) {
        return source->list_past_meeting_summaries(limit, qt::toUtf8(query));
    };
}

} // namespace fv
