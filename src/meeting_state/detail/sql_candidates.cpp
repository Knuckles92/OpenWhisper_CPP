// SqlMeetingRepository::search_candidates (repository.py search_candidates).

#include <algorithm>
#include <array>
#include <exception>
#include <set>
#include <string>
#include <utility>

#include "base/log.h"
#include "db/database.h"
#include "meeting_state/detail/sql_internal.h"
#include "meeting_state/sql_repository.h"
#include "pyport/regex.h"
#include "pyport/text.h"

namespace fv::meeting_state {

namespace {

constexpr log::Logger logger("meeting.persist.repository");

// Most terms the match expression ORs together.
constexpr std::size_t kMaxTerms = 20;
// Most passages a call returns.
constexpr std::int64_t kMaxCandidates = 48;

constexpr std::array<std::string_view, 12> kStopWords = {
    "the", "did", "what", "when", "where", "how", "was", "were", "about", "with", "that", "meeting",
};

constexpr std::array<std::string_view, 7> kKeys = {"segment_id", "meeting_id", "start_s",   "end_s",
                                                   "text",       "title",      "started_at"};

std::vector<std::string> query_terms(std::string_view query) {
    static const py::Regex word(R"(\w+)");
    std::vector<std::string> terms;
    for (auto& term : word.findall(query)) {
        if (py::len(term) <= 2) {
            continue;
        }
        const std::string lowered = py::lower(term);
        if (std::find(kStopWords.begin(), kStopWords.end(), lowered) != kStopWords.end()) {
            continue;
        }
        terms.push_back(std::move(term));
        if (terms.size() == kMaxTerms) {
            break;
        }
    }
    return terms;
}

std::vector<Json> rows_of(sql::Statement& statement) {
    std::vector<Json> rows;
    while (statement.step()) {
        Json row = Json::object();
        for (int index = 0; index < static_cast<int>(kKeys.size()); ++index) {
            row[std::string(kKeys[static_cast<std::size_t>(index)])] =
                detail::from_value(statement.column(index));
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace

std::vector<Json>
SqlMeetingRepository::search_candidates(std::string_view query,
                                        const std::optional<std::string>& exclude_meeting_id,
                                        std::int64_t limit, bool shared_only) {
    const std::vector<std::string> terms = query_terms(query);
    const std::int64_t bounded = std::clamp<std::int64_t>(limit, 1, kMaxCandidates);
    const std::string exclude = exclude_meeting_id.value_or("");
    const std::int64_t any = shared_only ? 0 : 1;

    return database_.read([&](sql::Connection& connection) {
        std::vector<Json> matches;
        if (!terms.empty()) {
            std::string match;
            for (const auto& term : terms) {
                match += (match.empty() ? "\"" : " OR \"") + term + "\"";
            }
            try {
                sql::Statement statement = connection.prepare(
                    "SELECT ms.id AS segment_id, ms.meeting_id, ms.start_s, ms.end_s, ms.text, "
                    "s.title, s.started_at FROM meeting_segments_fts JOIN meeting_segments ms ON "
                    "ms.rowid = meeting_segments_fts.rowid JOIN meeting_sessions s ON s.id = "
                    "ms.meeting_id WHERE meeting_segments_fts MATCH :q AND (:any = 1 OR "
                    "s.cloud_enabled = 1) AND s.id != :exclude ORDER BY "
                    "bm25(meeting_segments_fts), s.started_at DESC LIMIT :limit");
                statement.bind(":q", match);
                statement.bind(":any", any);
                statement.bind(":exclude", exclude);
                statement.bind(":limit", bounded);
                matches = rows_of(statement);
            } catch (const std::exception&) {
                logger.debug(
                    "Search index unavailable for semantic candidates; using recent passages");
                matches.clear();
            }
        }
        sql::Statement statement = connection.prepare(
            "WITH passages AS (SELECT ms.id AS segment_id, ms.meeting_id, ms.start_s, ms.end_s, "
            "ms.text, s.title, s.started_at, row_number() OVER (PARTITION BY ms.meeting_id, "
            "CAST(ms.start_s / 60 AS INTEGER) ORDER BY length(ms.text) DESC, ms.id) AS minute_row "
            "FROM meeting_segments ms JOIN meeting_sessions s ON s.id = ms.meeting_id WHERE "
            "(:any = 1 OR s.cloud_enabled = 1) AND s.id != :exclude AND length(ms.text) > 20), "
            "diverse AS (SELECT *, row_number() OVER (PARTITION BY meeting_id ORDER BY start_s "
            "DESC) AS meeting_row FROM passages WHERE minute_row = 1) SELECT segment_id, "
            "meeting_id, start_s, end_s, text, title, started_at FROM diverse ORDER BY "
            "meeting_row, started_at DESC LIMIT :limit");
        statement.bind(":any", any);
        statement.bind(":exclude", exclude);
        statement.bind(":limit", bounded);
        const std::vector<Json> recent = rows_of(statement);

        // Half the budget is reserved for passages that may share no word with the query.
        const auto reserved = static_cast<std::size_t>(bounded / 2);
        const auto cap = static_cast<std::size_t>(bounded);
        std::vector<Json> chosen;
        std::set<std::string, std::less<>> seen;
        const auto take = [&](const Json& row) {
            std::string id = detail::text_value(detail::to_value(row.at("segment_id")));
            if (seen.insert(std::move(id)).second) {
                chosen.push_back(row);
            }
        };
        for (std::size_t index = 0; index < std::min(reserved, matches.size()); ++index) {
            take(matches[index]);
        }
        std::vector<const Json*> rest;
        for (const auto& row : recent) {
            rest.push_back(&row);
        }
        for (std::size_t index = reserved; index < matches.size(); ++index) {
            rest.push_back(&matches[index]);
        }
        for (const Json* row : rest) {
            if (chosen.size() >= cap) {
                break;
            }
            take(*row);
        }
        return chosen;
    });
}

} // namespace fv::meeting_state
