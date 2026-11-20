#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "base/json.h"
#include "meeting_state/repository.h"
#include "meeting_state/sql_repository.h"

// The reads past-meeting recall and history search make of stored meetings. A port of its own
// because MeetingRepository does not grow (lead amendment 26): the semantic candidates are a
// member of SqlMeetingRepository only. Rows are meeting_state's JSON rows.
namespace fv::meeting_intel {

class HistorySource {
public:
    virtual ~HistorySource() = default;

    // MeetingRepository::search_transcripts.
    virtual std::vector<Json>
    search_transcripts(std::string_view query, const std::optional<std::string>& exclude_meeting_id,
                       std::int64_t limit) = 0;
    // SqlMeetingRepository::search_candidates. May throw; history search then shows keyword
    // results.
    virtual std::vector<Json>
    search_candidates(std::string_view query, const std::optional<std::string>& exclude_meeting_id,
                      std::int64_t limit, bool shared_only) = 0;
    // MeetingRepository::get_meeting.
    virtual std::optional<Json> get_meeting(std::string_view meeting_id) = 0;
    // MeetingRepository::get_segments from the start, at most `limit` rows.
    virtual std::vector<Json> get_segments(std::string_view meeting_id, std::int64_t limit) = 0;
};

// A HistorySource over any MeetingRepository, with the candidate search given separately. Without
// one, search_candidates throws std::logic_error (a repository that cannot rank, as Python's
// AttributeError). Both must outlive the source.
// Affinity: as thread-safe as the repository.
class RepositoryHistorySource final : public HistorySource {
public:
    using Candidates = std::function<std::vector<Json>(
        std::string_view query, const std::optional<std::string>& exclude_meeting_id,
        std::int64_t limit, bool shared_only)>;

    explicit RepositoryHistorySource(meeting_state::MeetingRepository& repository,
                                     Candidates candidates = {});
    // Over the SQL repository, its candidate search included.
    explicit RepositoryHistorySource(meeting_state::SqlMeetingRepository& repository);

    std::vector<Json> search_transcripts(std::string_view query,
                                         const std::optional<std::string>& exclude_meeting_id,
                                         std::int64_t limit) override;
    std::vector<Json> search_candidates(std::string_view query,
                                        const std::optional<std::string>& exclude_meeting_id,
                                        std::int64_t limit, bool shared_only) override;
    std::optional<Json> get_meeting(std::string_view meeting_id) override;
    std::vector<Json> get_segments(std::string_view meeting_id, std::int64_t limit) override;

private:
    meeting_state::MeetingRepository& repository_;
    Candidates candidates_;
};

} // namespace fv::meeting_intel
