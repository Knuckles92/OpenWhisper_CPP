#include "meeting_intel/history_source.h"

#include <stdexcept>
#include <utility>

namespace fv::meeting_intel {

RepositoryHistorySource::RepositoryHistorySource(meeting_state::MeetingRepository& repository,
                                                 Candidates candidates)
    : repository_(repository), candidates_(std::move(candidates)) {}

RepositoryHistorySource::RepositoryHistorySource(meeting_state::SqlMeetingRepository& repository)
    : RepositoryHistorySource(
          repository, [&repository](std::string_view query,
                                    const std::optional<std::string>& exclude_meeting_id,
                                    std::int64_t limit, bool shared_only) {
              return repository.search_candidates(query, exclude_meeting_id, limit, shared_only);
          }) {}

std::vector<Json>
RepositoryHistorySource::search_transcripts(std::string_view query,
                                            const std::optional<std::string>& exclude_meeting_id,
                                            std::int64_t limit) {
    return repository_.search_transcripts(query, exclude_meeting_id, limit);
}

std::vector<Json>
RepositoryHistorySource::search_candidates(std::string_view query,
                                           const std::optional<std::string>& exclude_meeting_id,
                                           std::int64_t limit, bool shared_only) {
    if (!candidates_) {
        throw std::logic_error("this meeting repository has no candidate search");
    }
    return candidates_(query, exclude_meeting_id, limit, shared_only);
}

std::optional<Json> RepositoryHistorySource::get_meeting(std::string_view meeting_id) {
    return repository_.get_meeting(meeting_id);
}

std::vector<Json> RepositoryHistorySource::get_segments(std::string_view meeting_id,
                                                        std::int64_t limit) {
    return repository_.get_segments(meeting_id, -1.0, limit);
}

} // namespace fv::meeting_intel
