#include "meeting_intel/semantic_search.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <exception>
#include <map>
#include <stdexcept>
#include <utility>
#include <vector>

#include "base/log.h"
#include "meeting_intel/detail/text.h"
#include "meeting_intel/intel_settings.h"
#include "pyport/errors.h"
#include "pyport/text.h"

namespace fv::meeting_intel {

namespace {

constexpr log::Logger logger("meeting.semantic_search");

// The query is cut to this many code points.
constexpr std::int64_t kMaxQueryChars = 500;
constexpr std::int64_t kMaxLimit = 200;
// Code points of a passage and of its meeting's title that go to the judge.
constexpr std::int64_t kPassageChars = 900;
constexpr std::int64_t kTitleChars = 120;
// Code points of a relevant passage shown as its snippet.
constexpr std::int64_t kSnippetChars = 260;

// _rank_candidates' RuntimeError: the ranking stopped for a reason that names no user text.
class RankingStopped : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

void require_enabled(const std::function<Json()>& settings) {
    if (!resolve_typesafe_feature_enabled(settings(), kFeatureSemanticSearch)) {
        throw RankingStopped("disabled");
    }
}

// row["text"][:n]: a string sliced, anything else Python could not slice.
std::string head(const Json& row, std::string_view key, std::int64_t chars, bool missing_is_empty) {
    const auto found = row.find(std::string(key));
    if (found == row.end()) {
        if (missing_is_empty) {
            return "";
        }
        throw py::PyError("KeyError", std::string(key));
    }
    if (!found->is_string()) {
        throw py::TypeError("passage field is not text");
    }
    return py::slice(found->get_ref<const std::string&>(), 0, chars);
}

std::string question_for(const std::string& key) {
    std::string question(kRelevanceQuestion);
    const std::string placeholder = "{key}";
    question.replace(question.find(placeholder), placeholder.size(), key);
    return question;
}

std::vector<Json> judge_batch(llm::Judge& judge, const std::string& query,
                              const std::vector<Json>& batch, const Clock& clock,
                              std::chrono::steady_clock::time_point deadline,
                              const CancelToken& cancel) {
    Json passages = Json::object();
    Json questions = Json::object();
    for (std::size_t index = 0; index < batch.size(); ++index) {
        const std::string key = "p" + std::to_string(index);
        Json passage = Json::object();
        passage["text"] = head(batch[index], "text", kPassageChars, false);
        passage["title"] = head(batch[index], "title", kTitleChars, true);
        passages[key] = std::move(passage);
    }
    for (const auto& [key, _] : passages.items()) {
        questions[key] = Json{{"type", "noul"}, {"instructions", question_for(key)}};
    }
    if (questions.empty()) {
        return {};
    }
    const double remaining = std::chrono::duration<double>(deadline - clock.steady()).count();
    if (remaining <= 0) {
        throw RankingStopped("out of time");
    }
    Json state = Json::object();
    state["query"] = query;
    state["passages"] = std::move(passages);
    const std::optional<Json> answers = judge.ask(state, questions, remaining, cancel);
    if (!answers) {
        throw RankingStopped("unavailable");
    }
    std::vector<Json> relevant;
    for (std::size_t index = 0; index < batch.size(); ++index) {
        const Json& score = answers->at("p" + std::to_string(index)).at("noul");
        if (!score.is_number()) {
            throw py::TypeError("judge score is not a number");
        }
        if (score.get<double>() >= kRelevantScore) {
            Json row = batch[index];
            row["semantic_score"] = score;
            row["snippet"] = head(batch[index], "text", kSnippetChars, false);
            relevant.push_back(std::move(row));
        }
    }
    return relevant;
}

std::vector<Json> rank_candidates(HistorySource& source, const std::function<Json()>& settings,
                                  const std::string& query, llm::Judge& judge,
                                  const std::optional<std::string>& exclude_meeting_id,
                                  const Clock& clock, const CancelToken& cancel) {
    const auto deadline =
        clock.steady() + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                             std::chrono::duration<double>(kSearchBudgetS));
    const std::vector<Json> candidates =
        source.search_candidates(query, exclude_meeting_id, kMaxCandidates, !judge.is_local());
    std::vector<Json> ranked;
    // One meeting read per meeting and batch: a row carries the meeting's whole state.
    std::map<std::string, bool, std::less<>> shared;
    for (std::size_t offset = 0; offset < candidates.size(); offset += kBatchSize) {
        require_enabled(settings);
        std::vector<Json> batch(candidates.begin() + static_cast<std::ptrdiff_t>(offset),
                                candidates.begin() + static_cast<std::ptrdiff_t>(std::min(
                                                         offset + kBatchSize, candidates.size())));
        // Checked before every request, not only when the candidates were read: text leaves this
        // computer only from meetings that shared it with AI insights on.
        if (!judge.is_local()) {
            shared.clear();
            std::vector<Json> allowed;
            for (auto& row : batch) {
                const std::string meeting_id = detail::py_str(row.at("meeting_id"));
                auto known = shared.find(meeting_id);
                if (known == shared.end()) {
                    const std::optional<Json> meeting = source.get_meeting(meeting_id);
                    bool cloud = false;
                    if (meeting && meeting->is_object()) {
                        const auto flag = meeting->find("cloud_enabled");
                        cloud = flag != meeting->end() && detail::truthy(*flag);
                    }
                    known = shared.emplace(meeting_id, cloud).first;
                }
                if (known->second) {
                    allowed.push_back(std::move(row));
                }
            }
            batch = std::move(allowed);
        }
        for (auto& row : judge_batch(judge, query, batch, clock, deadline, cancel)) {
            ranked.push_back(std::move(row));
        }
    }
    require_enabled(settings);
    return ranked;
}

Json fallback_of(std::vector<Json> results) {
    Json fallback = Json::object();
    fallback["results"] = Json::array();
    for (auto& row : results) {
        fallback["results"].push_back(std::move(row));
    }
    fallback["mode"] = "keyword";
    fallback["message"] = "";
    return fallback;
}

} // namespace

Json search_history(HistorySource& source, const std::function<Json()>& settings,
                    std::string_view query, bool semantic,
                    const std::optional<std::string>& exclude_meeting_id, std::int64_t limit,
                    const JudgeFactory& judge_factory, const Clock& clock,
                    const CancelToken& cancel) {
    const std::string cleaned = py::slice(py::strip(query), 0, kMaxQueryChars);
    const std::int64_t bounded = std::clamp<std::int64_t>(limit, 1, kMaxLimit);
    if (cleaned.empty()) {
        return fallback_of({});
    }
    Json fallback = fallback_of(source.search_transcripts(cleaned, exclude_meeting_id, bounded));
    if (!semantic) {
        return fallback;
    }
    if (!resolve_typesafe_feature_enabled(settings(), kFeatureSemanticSearch)) {
        fallback["message"] = std::string(kDisabledMessage);
        return fallback;
    }
    const std::shared_ptr<llm::Judge> judge = judge_factory ? judge_factory() : nullptr;
    if (!judge) {
        fallback["message"] = "Keyword results. Add " +
                              llm::route_from_settings(settings()).key_phrase() +
                              " in Settings → App → API keys for meaning-based ranking.";
        return fallback;
    }
    try {
        std::vector<Json> ranked =
            rank_candidates(source, settings, cleaned, *judge, exclude_meeting_id, clock, cancel);
        std::stable_sort(ranked.begin(), ranked.end(), [](const Json& a, const Json& b) {
            return a.at("semantic_score").get<double>() > b.at("semantic_score").get<double>();
        });
        if (ranked.size() > static_cast<std::size_t>(bounded)) {
            ranked.resize(static_cast<std::size_t>(bounded));
        }
        Json result = Json::object();
        result["results"] = Json::array();
        for (auto& row : ranked) {
            result["results"].push_back(std::move(row));
        }
        result["mode"] = "semantic";
        result["message"] = std::string(judge->is_local() ? kLocalScope : kScope);
        return result;
    } catch (const RankingStopped& stopped) {
        logger.warning("Semantic history ranking unavailable ({}); using keyword results",
                       stopped.what());
    } catch (const std::exception& error) {
        // Type only: a judge or storage error may quote the query or a passage.
        logger.warning("Semantic history ranking failed ({}); using keyword results",
                       detail::type_name(error));
    }
    fallback["message"] = std::string(kUnavailableMessage);
    return fallback;
}

Json search_history(HistorySource& source, const Json& settings, std::string_view query,
                    bool semantic, const std::optional<std::string>& exclude_meeting_id,
                    std::int64_t limit, std::shared_ptr<llm::Judge> judge, const Clock& clock,
                    const CancelToken& cancel) {
    return search_history(
        source, [&settings] { return settings; }, query, semantic, exclude_meeting_id, limit,
        [judge = std::move(judge)] { return judge; }, clock, cancel);
}

} // namespace fv::meeting_intel
